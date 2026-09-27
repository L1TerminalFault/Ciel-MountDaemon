/*
 * mtp_fs.c - see mtp_fs.h
 *
 * How browsing works: MTP has no path strings at all - every object
 * (file or folder) is identified by a numeric "object handle" plus
 * which numbered "storage" (e.g. internal memory vs SD card) it lives
 * on. FUSE, on the other hand, only ever gives us paths like
 * "/Internal storage/DCIM/Camera/IMG_0001.jpg". So the bulk of this
 * file is resolve_path(): walking a path one component at a time,
 * asking the device "what's in this folder?" at each step, until we
 * either find the object we're after or run out of path.
 *
 * That means every operation - even a plain `ls` - does one MTP
 * round-trip per path component. Real-world MTP transport is slow
 * (individual round-trips are often tens of milliseconds), so a deep
 * directory structure will feel sluggish. The natural next step is
 * caching each directory's listing for a few seconds; deliberately left
 * out here to keep the control flow easy to follow.
 *
 * Writes are staged to a local temp file and uploaded as a whole object
 * on close() (see release()), rather than attempted as in-place partial
 * writes - MTP's in-place edit operations are an optional device
 * capability that not everything supports, whereas "write a new object
 * on release" only ever depends on Send_File, LIBMTP_Delete_Object,
 * which are universally implemented.
 */

#define FUSE_USE_VERSION 31

#include "mtp_fs.h"
#include "log.h"
#include "util.h"

#include <fuse3/fuse.h>
#include <libmtp.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* One MTP device per process (see mtp_fs.h for why) - a single static
 * context is simpler than threading fuse_context->private_data through
 * every callback, and is safe precisely because this process does
 * nothing else and is never reused for a second device. */
static LIBMTP_mtpdevice_t *g_device = NULL;
static uid_t g_owner_uid = 0;
static gid_t g_owner_gid = 0;

#define MAX_COMPONENTS 32
#define MAX_NAME 256

/* Result of walking a FUSE path down to the MTP object (if any) it
 * refers to. */
typedef struct {
    bool exists;         /* false if the path doesn't currently exist   */
    bool is_dir;          /* true for the virtual root, a storage root,
                              or an MTP folder                          */
    uint32_t storage_id;  /* which storage this path is/would be on;
                              meaningless if path == "/"                */
    uint32_t item_id;     /* MTP object handle; 0 for "/" and for a
                              not-yet-existing path                     */
    uint32_t parent_id;   /* folder id of the *containing* directory,
                              LIBMTP_FILES_AND_FOLDERS_ROOT if directly
                              under a storage root; used by create/mkdir/
                              rename to know where to put a new object  */
    uint64_t filesize;
    time_t mtime;
    char leaf[MAX_NAME];  /* final path component, sanitized-free (MTP
                              filenames are used as-is); "" for "/"     */
} resolved_t;

/* Per-open-file state, stashed in struct fuse_file_info::fh. */
typedef struct {
    uint32_t storage_id;
    uint32_t parent_id;
    uint32_t item_id;   /* 0 until the object has actually been created
                            on the device (i.e. on the first release()
                            of a brand new file)                        */
    char name[MAX_NAME];
    int tmp_fd;          /* -1 until a write() actually stages data     */
    bool dirty;
} mtp_handle_t;

/* ---- small helpers -------------------------------------------------- */

/* Replace characters that would be meaningless or dangerous as a
 * directory entry (path separators, control chars) with '_'. Only used
 * for the synthetic top-level "storage name" directories we invent -
 * actual file/folder names on the device are trusted as-is, the same
 * way any local filesystem trusts what's already stored on disk. */
static void sanitize_storage_name(const char *in, char *out, size_t out_len)
{
    if (!in || !in[0]) {
        safe_copy(out, out_len, "storage");
        return;
    }
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < out_len; i++) {
        char c = in[i];
        out[j++] = (c == '/' || (unsigned char)c < 0x20) ? '_' : c;
    }
    out[j] = '\0';
}

/* Frees an entire LIBMTP_file_t linked list (LIBMTP_destroy_file_t only
 * frees one node), used after every Get_Files_And_Folders() call. */
static void free_file_list(LIBMTP_file_t *list)
{
    while (list) {
        LIBMTP_file_t *next = list->next;
        LIBMTP_destroy_file_t(list);
        list = next;
    }
}

/* Splits `path` (a leading-"/"-style FUSE path) into components in
 * place. Returns the component count, or -1 if there are more than
 * MAX_COMPONENTS (pathological input we simply refuse). */
static int split_path(const char *path, char storage[MAX_COMPONENTS][MAX_NAME])
{
    if (strcmp(path, "/") == 0)
        return 0;

    char copy[2048];
    safe_copy(copy, sizeof(copy), path[0] == '/' ? path + 1 : path);

    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (n >= MAX_COMPONENTS)
            return -1;
        safe_copy(storage[n], MAX_NAME, tok);
        n++;
    }
    return n;
}

/* Finds the file/folder named `name` directly inside (storage_id,
 * parent_id). Fills *out on success. Returns true if found. */
static bool find_child(uint32_t storage_id, uint32_t parent_id, const char *name,
                        LIBMTP_file_t *out)
{
    LIBMTP_file_t *list = LIBMTP_Get_Files_And_Folders(g_device, storage_id, parent_id);
    bool found = false;
    for (LIBMTP_file_t *f = list; f; f = f->next) {
        if (f->filename && strcmp(f->filename, name) == 0) {
            *out = *f; /* shallow copy - out->filename below is re-owned */
            out->filename = NULL; /* don't leave a dangling pointer into
                                      the list we're about to free; only
                                      out->item_id/parent_id/storage_id/
                                      filesize/filetype are used by
                                      callers, never out->filename       */
            found = true;
            break;
        }
    }
    free_file_list(list);
    return found;
}

/* The core path walker described at the top of this file. */
static int resolve_path(const char *path, resolved_t *r)
{
    memset(r, 0, sizeof(*r));

    char comps[MAX_COMPONENTS][MAX_NAME];
    int n = split_path(path, comps);
    if (n < 0)
        return -ENAMETOOLONG;

    if (n == 0) {
        r->exists = true;
        r->is_dir = true;
        r->item_id = 0;
        return 0;
    }

    /* First component selects a storage. */
    LIBMTP_devicestorage_t *storage = NULL;
    for (LIBMTP_devicestorage_t *s = g_device->storage; s; s = s->next) {
        char name[MAX_NAME];
        sanitize_storage_name(s->StorageDescription, name, sizeof(name));
        if (strcmp(name, comps[0]) == 0) {
            storage = s;
            break;
        }
    }
    if (!storage) {
        r->exists = false;
        return 0;
    }

    r->storage_id = storage->id;
    uint32_t parent_id = LIBMTP_FILES_AND_FOLDERS_ROOT;

    if (n == 1) {
        r->exists = true;
        r->is_dir = true;
        r->item_id = 0; /* storage roots have no MTP object handle */
        r->parent_id = LIBMTP_FILES_AND_FOLDERS_ROOT;
        safe_copy(r->leaf, sizeof(r->leaf), comps[0]);
        return 0;
    }

    /* Walk every component except the last, each must be a folder. */
    for (int i = 1; i < n - 1; i++) {
        LIBMTP_file_t found;
        if (!find_child(storage->id, parent_id, comps[i], &found)) {
            r->exists = false;
            return 0;
        }
        if (found.filetype != LIBMTP_FILETYPE_FOLDER)
            return -ENOTDIR;
        parent_id = found.item_id;
    }

    /* Last component: may or may not exist yet (callers like create()
     * and mkdir() need parent_id/storage_id even when it doesn't). */
    const char *leaf = comps[n - 1];
    safe_copy(r->leaf, sizeof(r->leaf), leaf);
    r->parent_id = parent_id;

    LIBMTP_file_t found;
    if (!find_child(storage->id, parent_id, leaf, &found)) {
        r->exists = false;
        return 0;
    }

    r->exists = true;
    r->is_dir = (found.filetype == LIBMTP_FILETYPE_FOLDER);
    r->item_id = found.item_id;
    r->filesize = found.filesize;
    r->mtime = found.modificationdate;
    return 0;
}

/* Guesses an MTP filetype from a filename's extension, for objects we
 * upload. Devices mostly use this for categorization (e.g. showing it
 * in a "Photos" view); getting it wrong doesn't affect whether the
 * bytes round-trip correctly, so this is deliberately a short list
 * covering common cases rather than exhaustive. */
static LIBMTP_filetype_t guess_filetype(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return LIBMTP_FILETYPE_UNKNOWN;
    dot++;

    static const struct { const char *ext; LIBMTP_filetype_t type; } table[] = {
        {"jpg", LIBMTP_FILETYPE_JPEG}, {"jpeg", LIBMTP_FILETYPE_JPEG},
        {"png", LIBMTP_FILETYPE_PNG},  {"gif", LIBMTP_FILETYPE_GIF},
        {"bmp", LIBMTP_FILETYPE_BMP},  {"tiff", LIBMTP_FILETYPE_TIFF},
        {"mp3", LIBMTP_FILETYPE_MP3},  {"wav", LIBMTP_FILETYPE_WAV},
        {"flac", LIBMTP_FILETYPE_FLAC},{"m4a", LIBMTP_FILETYPE_M4A},
        {"mp4", LIBMTP_FILETYPE_MP4},  {"avi", LIBMTP_FILETYPE_AVI},
        {"mkv", LIBMTP_FILETYPE_UNKNOWN}, /* no MTP type for mkv */
        {"txt", LIBMTP_FILETYPE_TEXT}, {"html", LIBMTP_FILETYPE_HTML},
        {"xml", LIBMTP_FILETYPE_XML},  {"doc", LIBMTP_FILETYPE_DOC},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcasecmp(dot, table[i].ext) == 0)
            return table[i].type;
    return LIBMTP_FILETYPE_UNKNOWN;
}

/* ---- fuse_operations callbacks --------------------------------------- */

static int op_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    (void)fi;
    memset(st, 0, sizeof(*st));

    resolved_t r;
    int rc = resolve_path(path, &r);
    if (rc != 0)
        return rc;
    if (!r.exists)
        return -ENOENT;

    st->st_uid = g_owner_uid;
    st->st_gid = g_owner_gid;
    st->st_atime = st->st_ctime = st->st_mtime = r.mtime ? r.mtime : time(NULL);

    if (r.is_dir) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
    } else {
        st->st_mode = S_IFREG | 0644;
        st->st_nlink = 1;
        st->st_size = (off_t)r.filesize;
    }
    return 0;
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags)
{
    (void)offset; (void)fi; (void)flags;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    resolved_t r;
    int rc = resolve_path(path, &r);
    if (rc != 0)
        return rc;
    if (!r.exists)
        return -ENOENT;
    if (!r.is_dir)
        return -ENOTDIR;

    if (strcmp(path, "/") == 0) {
        /* Root: one synthetic entry per storage (internal memory, SD
         * card, ...) rather than a real MTP folder listing. */
        for (LIBMTP_devicestorage_t *s = g_device->storage; s; s = s->next) {
            char name[MAX_NAME];
            sanitize_storage_name(s->StorageDescription, name, sizeof(name));
            filler(buf, name, NULL, 0, 0);
        }
        return 0;
    }

    uint32_t parent_id = (r.item_id != 0) ? r.item_id : LIBMTP_FILES_AND_FOLDERS_ROOT;
    LIBMTP_file_t *list = LIBMTP_Get_Files_And_Folders(g_device, r.storage_id, parent_id);
    for (LIBMTP_file_t *f = list; f; f = f->next)
        if (f->filename)
            filler(buf, f->filename, NULL, 0, 0);
    free_file_list(list);
    return 0;
}

static int op_open(const char *path, struct fuse_file_info *fi)
{
    resolved_t r;
    int rc = resolve_path(path, &r);
    if (rc != 0)
        return rc;
    if (!r.exists)
        return -ENOENT;
    if (r.is_dir)
        return -EISDIR;

    mtp_handle_t *h = calloc(1, sizeof(*h));
    if (!h)
        return -ENOMEM;
    h->storage_id = r.storage_id;
    h->parent_id = r.parent_id;
    h->item_id = r.item_id;
    safe_copy(h->name, sizeof(h->name), r.leaf);
    h->tmp_fd = -1;
    h->dirty = false;

    fi->fh = (uint64_t)(uintptr_t)h;
    return 0;
}

/* Ensures h->tmp_fd is a local staging file, downloading the object's
 * current content into it first if it already exists on the device.
 * Shared by write() and truncate() - both need "somewhere local to
 * apply the change" before we can re-upload on release(). */
static int ensure_staged(mtp_handle_t *h)
{
    if (h->tmp_fd >= 0)
        return 0;

    char tmpl[] = "/tmp/ciel-mountd-mtp-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0)
        return -errno;
    unlink(tmpl); /* unnamed as soon as it's open; freed automatically on close */

    if (h->item_id != 0) {
        if (LIBMTP_Get_File_To_File_Descriptor(g_device, h->item_id, fd, NULL, NULL) != 0) {
            close(fd);
            return -EIO;
        }
    }
    h->tmp_fd = fd;
    return 0;
}

static int op_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)mode;

    resolved_t r;
    int rc = resolve_path(path, &r);
    if (rc != 0)
        return rc;
    if (r.exists && r.is_dir)
        return -EISDIR;

    /* r.parent_id/r.storage_id/r.leaf are filled in regardless of
     * whether the leaf itself exists yet - exactly what a fresh file
     * needs. If it does already exist, this create() behaves like
     * O_TRUNC: the old object is deleted and replaced on release(). */
    mtp_handle_t *h = calloc(1, sizeof(*h));
    if (!h)
        return -ENOMEM;
    h->storage_id = r.storage_id;
    h->parent_id = r.parent_id;
    h->item_id = r.exists ? r.item_id : 0;
    safe_copy(h->name, sizeof(h->name), r.leaf);
    h->tmp_fd = -1;
    h->dirty = false;

    int erc = ensure_staged(h); /* opens an empty (or, if overwriting,
                                    pre-populated) staging file now, so
                                    a create()+immediate-read sees
                                    consistent content even before any
                                    write() happens */
    if (erc != 0) {
        free(h);
        return erc;
    }

    fi->fh = (uint64_t)(uintptr_t)h;
    return 0;
}

static int op_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    (void)path;
    mtp_handle_t *h = (mtp_handle_t *)(uintptr_t)fi->fh;

    if (h->tmp_fd >= 0) {
        /* File has local staged writes (or was opened for writing) -
         * read back from there so a program that writes then reads its
         * own data (common in editors/downloaders) sees what it wrote,
         * not stale device content. */
        ssize_t got = pread(h->tmp_fd, buf, size, offset);
        return got < 0 ? -errno : (int)got;
    }

    if (h->item_id == 0)
        return 0; /* brand new, nothing staged yet, nothing to read */

    unsigned char *data = NULL;
    unsigned int got = 0;
    int rc = LIBMTP_GetPartialObject(g_device, h->item_id, (uint64_t)offset,
                                      (uint32_t)size, &data, &got);
    if (rc != 0) {
        free(data);
        return -EIO;
    }
    if (got > size)
        got = (unsigned int)size; /* defensive; shouldn't happen */
    memcpy(buf, data, got);
    free(data);
    return (int)got;
}

static int op_write(const char *path, const char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
    (void)path;
    mtp_handle_t *h = (mtp_handle_t *)(uintptr_t)fi->fh;

    int rc = ensure_staged(h);
    if (rc != 0)
        return rc;

    ssize_t written = pwrite(h->tmp_fd, buf, size, offset);
    if (written < 0)
        return -errno;
    h->dirty = true;
    return (int)written;
}

static int op_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)path;
    if (!fi) {
        /* Truncating a path with no open file descriptor. We only
         * support the common `open(O_TRUNC)` case, which FUSE routes
         * through here *with* fi attached, or through create() above;
         * a bare truncate(2) syscall on a closed MTP file is rare
         * enough (and awkward enough - it'd mean opening, staging,
         * and immediately re-releasing purely to resize) that it's
         * left unimplemented here. */
        return -ENOSYS;
    }

    mtp_handle_t *h = (mtp_handle_t *)(uintptr_t)fi->fh;
    int rc = ensure_staged(h);
    if (rc != 0)
        return rc;
    if (ftruncate(h->tmp_fd, size) != 0)
        return -errno;
    h->dirty = true;
    return 0;
}

static int op_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    mtp_handle_t *h = (mtp_handle_t *)(uintptr_t)fi->fh;
    int result = 0;

    if (h->dirty && h->tmp_fd >= 0) {
        struct stat st;
        if (fstat(h->tmp_fd, &st) == 0) {
            /* MTP has no "replace contents in place" that's guaranteed
             * to exist on every device - the portable way to "modify" a
             * file is delete-then-recreate-with-the-same-name. This
             * briefly makes the file disappear from a device-side
             * listing mid-write, which is an acceptable trade-off for
             * working uniformly across devices. */
            if (h->item_id != 0)
                LIBMTP_Delete_Object(g_device, h->item_id);

            LIBMTP_file_t *meta = LIBMTP_new_file_t();
            meta->filename = strdup(h->name);
            meta->filesize = (uint64_t)st.st_size;
            meta->parent_id = h->parent_id;
            meta->storage_id = h->storage_id;
            meta->filetype = guess_filetype(h->name);

            lseek(h->tmp_fd, 0, SEEK_SET);
            if (LIBMTP_Send_File_From_File_Descriptor(g_device, h->tmp_fd, meta,
                                                        NULL, NULL) != 0) {
                log_warn("MTP upload failed for %s", h->name);
                result = -EIO;
            } else {
                log_info("MTP: uploaded %s (%llu bytes)",
                          h->name, (unsigned long long)st.st_size);
            }
            LIBMTP_destroy_file_t(meta); /* also frees the strdup'd filename */
        }
    }

    if (h->tmp_fd >= 0)
        close(h->tmp_fd);
    free(h);
    return result;
}

static int op_unlink(const char *path)
{
    resolved_t r;
    int rc = resolve_path(path, &r);
    if (rc != 0)
        return rc;
    if (!r.exists)
        return -ENOENT;
    if (r.is_dir)
        return -EISDIR;

    return LIBMTP_Delete_Object(g_device, r.item_id) == 0 ? 0 : -EIO;
}

static int op_mkdir(const char *path, mode_t mode)
{
    (void)mode;
    resolved_t r;
    int rc = resolve_path(path, &r);
    if (rc != 0)
        return rc;
    if (r.exists)
        return -EEXIST;
    if (r.storage_id == 0)
        return -ENOENT; /* e.g. mkdir directly under "/" - not a real storage */

    char name[MAX_NAME];
    safe_copy(name, sizeof(name), r.leaf); /* Create_Folder wants char*,
                                               not const char*           */
    uint32_t new_id = LIBMTP_Create_Folder(g_device, name, r.parent_id, r.storage_id);
    return new_id != 0 ? 0 : -EIO;
}

static int op_rmdir(const char *path)
{
    resolved_t r;
    int rc = resolve_path(path, &r);
    if (rc != 0)
        return rc;
    if (!r.exists)
        return -ENOENT;
    if (!r.is_dir)
        return -ENOTDIR;
    if (r.item_id == 0)
        return -EBUSY; /* a storage root itself, e.g. "/Internal storage" */

    /* Most MTP responders refuse to delete a non-empty folder and
     * report that as a generic failure - we can't distinguish "not
     * empty" from other errors without a device-specific error code
     * lookup, so any failure here is reported as -ENOTEMPTY, which is
     * right far more often than not. */
    return LIBMTP_Delete_Object(g_device, r.item_id) == 0 ? 0 : -ENOTEMPTY;
}

static int op_rename(const char *from, const char *to, unsigned int flags)
{
    if (flags != 0)
        return -EINVAL; /* RENAME_EXCHANGE/RENAME_NOREPLACE: not supported */

    resolved_t src, dst;
    int rc = resolve_path(from, &src);
    if (rc != 0) return rc;
    if (!src.exists) return -ENOENT;

    rc = resolve_path(to, &dst);
    if (rc != 0) return rc;
    if (dst.exists) return -EEXIST; /* keep this simple: no clobbering */

    if (src.storage_id != dst.storage_id || src.parent_id != dst.parent_id) {
        if (LIBMTP_Move_Object(g_device, src.item_id, dst.storage_id, dst.parent_id) != 0)
            return -EIO;
    }

    if (strcmp(src.leaf, dst.leaf) != 0) {
        char newname[MAX_NAME];
        safe_copy(newname, sizeof(newname), dst.leaf); /* wants char*, not const */
        if (LIBMTP_Set_Object_Filename(g_device, src.item_id, newname) != 0)
            return -EIO;
    }
    return 0;
}

static int op_statfs(const char *path, struct statvfs *stv)
{
    memset(stv, 0, sizeof(*stv));
    stv->f_bsize = 4096;
    stv->f_namemax = 255;

    resolved_t r;
    if (resolve_path(path, &r) != 0 || !r.exists)
        return -ENOENT;

    /* Sum every storage for "/" itself; a specific storage subtree
     * reports just that storage's numbers. */
    uint64_t cap = 0, free_bytes = 0;
    for (LIBMTP_devicestorage_t *s = g_device->storage; s; s = s->next) {
        if (strcmp(path, "/") == 0 || s->id == r.storage_id) {
            cap += s->MaxCapacity;
            free_bytes += s->FreeSpaceInBytes;
        }
    }
    stv->f_blocks = cap / stv->f_bsize;
    stv->f_bfree = stv->f_bavail = free_bytes / stv->f_bsize;
    return 0;
}

static const struct fuse_operations mtp_ops = {
    .getattr  = op_getattr,
    .readdir  = op_readdir,
    .open     = op_open,
    .create   = op_create,
    .read     = op_read,
    .write    = op_write,
    .truncate = op_truncate,
    .release  = op_release,
    .unlink   = op_unlink,
    .mkdir    = op_mkdir,
    .rmdir    = op_rmdir,
    .rename   = op_rename,
    .statfs   = op_statfs,
};

/* ---- device discovery + entry point ---------------------------------- */

/* libmtp identifies raw devices only by USB bus/address, not by any
 * stable ID - so we match against the busnum/devnum udev_monitor.c
 * already read out of sysfs for us, to make sure we open the exact
 * physical device that generated the udev event (not just "the first
 * MTP device libmtp happens to see", which would be wrong the moment
 * two phones are plugged in at once). */
static LIBMTP_raw_device_t *find_raw_device(int busnum, int devnum)
{
    LIBMTP_raw_device_t *devices = NULL;
    int count = 0;

    if (LIBMTP_Detect_Raw_Devices(&devices, &count) != LIBMTP_ERROR_NONE || count == 0)
        return NULL;

    for (int i = 0; i < count; i++) {
        if ((int)devices[i].bus_location == busnum && (int)devices[i].devnum == devnum) {
            LIBMTP_raw_device_t *found = malloc(sizeof(*found));
            if (found)
                *found = devices[i];
            free(devices);
            return found;
        }
    }
    free(devices);
    return NULL;
}

int mtp_fs_run(const device_info_t *info, const char *mountpoint)
{
    LIBMTP_Init();

    LIBMTP_raw_device_t *raw = find_raw_device(info->usb_busnum, info->usb_devnum);
    if (!raw) {
        log_error("MTP: could not find raw device at bus %d addr %d (%s %s)",
                  info->usb_busnum, info->usb_devnum, info->id_vendor, info->id_model);
        return 1;
    }

    g_device = LIBMTP_Open_Raw_Device_Uncached(raw);
    free(raw);
    if (!g_device) {
        log_error("MTP: failed to open %s %s", info->id_vendor, info->id_model);
        return 1;
    }

    if (LIBMTP_Get_Storage(g_device, LIBMTP_STORAGE_SORTBY_NOTSORTED) != 0 ||
        g_device->storage == NULL) {
        log_error("MTP: %s %s reported no storage (locked screen? "
                  "needs \"file transfer\" mode selected on the device?)",
                  info->id_vendor, info->id_model);
        LIBMTP_Release_Device(g_device);
        return 1;
    }

    g_owner_uid = getuid();
    g_owner_gid = getgid();

    log_info("MTP: mounting %s %s at %s", info->id_vendor, info->id_model, mountpoint);

    /* argv for fuse_main(): "-f" keeps it in our foreground (we already
     * did our own fork - see mount_manager.c), "-s" forces single-
     * threaded operation, required because libmtp's device handle is
     * not safe to call into from multiple threads at once. */
    char *argv[] = { (char *)"ciel-ountd-mtp", (char *)"-f", (char *)"-s",
                      (char *)mountpoint, NULL };
    int argc = 4;

    int rc = fuse_main(argc, argv, &mtp_ops, NULL);

    LIBMTP_Release_Device(g_device);
    g_device = NULL;

    /* fuse_main() only returns after the mount is fully torn down (its
     * default SIGTERM/SIGINT handling calls fuse_session_exit(), which
     * unmounts before the loop returns) - so it's safe to remove the
     * directory now; nothing else can still be using it as a mountpoint. */
    if (rmdir(mountpoint) != 0)
        log_debug("rmdir(%s) failed: %s", mountpoint, strerror(errno));

    return rc;
}
