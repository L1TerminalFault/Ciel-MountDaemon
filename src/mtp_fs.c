#define FUSE_USE_VERSION 31

#include "mtp_fs.h"
#include "log.h"
#include "util.h"

#include <fuse3/fuse.h>
#include <libmtp.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

static LIBMTP_mtpdevice_t *g_device = NULL;
static uid_t g_owner_uid = 0;
static gid_t g_owner_gid = 0;

#define MAX_COMPONENTS 32
#define MAX_NAME 256

typedef struct {
    bool exists;
    bool is_dir;
    uint32_t storage_id;
    uint32_t item_id;
    uint32_t parent_id;
    uint64_t filesize;
    time_t mtime;
    char leaf[MAX_NAME];
} resolved_t;

typedef struct {
    uint32_t storage_id;
    uint32_t parent_id;
    uint32_t item_id;
    char name[MAX_NAME];
    int tmp_fd;
    bool dirty;
    bool staging_failed;
} mtp_handle_t;

static void sanitize_storage_name(const char *in, uint32_t storage_id, char *out, size_t out_len)
{
    if (!in || !in[0]) {
        snprintf(out, out_len, "storage_%08x", storage_id);
        return;
    }
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < out_len; i++) {
        char c = in[i];
        out[j++] = (c == '/' || (unsigned char)c < 0x20) ? '_' : c;
    }
    out[j] = '\0';
}

static void free_file_list(LIBMTP_file_t *list)
{
    while (list) {
        LIBMTP_file_t *next = list->next;
        LIBMTP_destroy_file_t(list);
        list = next;
    }
}

static int split_path(const char *path, char storage[MAX_COMPONENTS][MAX_NAME])
{
    if (!path || strcmp(path, "/") == 0)
        return 0;

    if (strlen(path) >= 2048)
        return -ENAMETOOLONG;

    char copy[2048];
    safe_copy(copy, sizeof(copy), path[0] == '/' ? path + 1 : path);

    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (n >= MAX_COMPONENTS)
            return -ENAMETOOLONG;
        safe_copy(storage[n], MAX_NAME, tok);
        n++;
    }
    return n;
}

static bool find_child(uint32_t storage_id, uint32_t parent_id, const char *name,
                       LIBMTP_file_t *out)
{
    if (!g_device)
        return false;

    LIBMTP_file_t *list = LIBMTP_Get_Files_And_Folders(g_device, storage_id, parent_id);
    bool found = false;
    for (LIBMTP_file_t *f = list; f; f = f->next) {
        if (f->filename && strcmp(f->filename, name) == 0) {
            *out = *f;
            out->filename = NULL; /* Detach pointer before list is freed */
            found = true;
            break;
        }
    }
    free_file_list(list);
    return found;
}

static int resolve_path(const char *path, resolved_t *r)
{
    memset(r, 0, sizeof(*r));
    if (!g_device)
        return -EIO;

    char comps[MAX_COMPONENTS][MAX_NAME];
    int n = split_path(path, comps);
    if (n < 0)
        return n;

    if (n == 0) {
        r->exists = true;
        r->is_dir = true;
        r->item_id = 0;
        return 0;
    }

    LIBMTP_devicestorage_t *storage = NULL;
    for (LIBMTP_devicestorage_t *s = g_device->storage; s; s = s->next) {
        char name[MAX_NAME];
        sanitize_storage_name(s->StorageDescription, s->id, name, sizeof(name));
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
        r->item_id = 0;
        r->parent_id = LIBMTP_FILES_AND_FOLDERS_ROOT;
        safe_copy(r->leaf, sizeof(r->leaf), comps[0]);
        return 0;
    }

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
        {"txt", LIBMTP_FILETYPE_TEXT}, {"html", LIBMTP_FILETYPE_HTML},
        {"xml", LIBMTP_FILETYPE_XML},  {"doc", LIBMTP_FILETYPE_DOC},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcasecmp(dot, table[i].ext) == 0)
            return table[i].type;
    }
    return LIBMTP_FILETYPE_UNKNOWN;
}

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

    struct stat st_dir;
    memset(&st_dir, 0, sizeof(st_dir));
    st_dir.st_uid = g_owner_uid;
    st_dir.st_gid = g_owner_gid;
    st_dir.st_mode = S_IFDIR | 0755;
    st_dir.st_nlink = 2;

    filler(buf, ".", &st_dir, 0, FUSE_FILL_DIR_PLUS);
    filler(buf, "..", &st_dir, 0, FUSE_FILL_DIR_PLUS);

    resolved_t r;
    int rc = resolve_path(path, &r);
    if (rc != 0)
        return rc;
    if (!r.exists)
        return -ENOENT;
    if (!r.is_dir)
        return -ENOTDIR;

    if (strcmp(path, "/") == 0) {
        for (LIBMTP_devicestorage_t *s = g_device->storage; s; s = s->next) {
            char name[MAX_NAME];
            sanitize_storage_name(s->StorageDescription, s->id, name, sizeof(name));
            filler(buf, name, &st_dir, 0, FUSE_FILL_DIR_PLUS);
        }
        return 0;
    }

    uint32_t parent_id = (r.item_id != 0) ? r.item_id : LIBMTP_FILES_AND_FOLDERS_ROOT;
    LIBMTP_file_t *list = LIBMTP_Get_Files_And_Folders(g_device, r.storage_id, parent_id);
    for (LIBMTP_file_t *f = list; f; f = f->next) {
        if (!f->filename)
            continue;

        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_uid = g_owner_uid;
        st.st_gid = g_owner_gid;
        st.st_atime = st.st_ctime = st.st_mtime = f->modificationdate ? f->modificationdate : time(NULL);

        if (f->filetype == LIBMTP_FILETYPE_FOLDER) {
            st.st_mode = S_IFDIR | 0755;
            st.st_nlink = 2;
        } else {
            st.st_mode = S_IFREG | 0644;
            st.st_nlink = 1;
            st.st_size = (off_t)f->filesize;
        }

        /* FUSE_FILL_DIR_PLUS avoids thousands of getattr calls */
        filler(buf, f->filename, &st, 0, FUSE_FILL_DIR_PLUS);
    }
    free_file_list(list);
    return 0;
}

static int ensure_staged(mtp_handle_t *h)
{
    if (h->tmp_fd >= 0)
        return 0;

    char tmpl[] = "/tmp/ciel-mountd-mtp-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0)
        return -errno;

    unlink(tmpl); /* Auto-cleanup on close */

    if (h->item_id != 0) {
        if (LIBMTP_Get_File_To_File_Descriptor(g_device, h->item_id, fd, NULL, NULL) != 0) {
            close(fd);
            return -EIO;
        }
    }
    h->tmp_fd = fd;
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

static int op_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)mode;

    resolved_t r;
    int rc = resolve_path(path, &r);
    if (rc != 0)
        return rc;
    if (r.exists && r.is_dir)
        return -EISDIR;

    mtp_handle_t *h = calloc(1, sizeof(*h));
    if (!h)
        return -ENOMEM;

    h->storage_id = r.storage_id;
    h->parent_id = r.parent_id;
    h->item_id = r.exists ? r.item_id : 0;
    safe_copy(h->name, sizeof(h->name), r.leaf);
    h->tmp_fd = -1;
    h->dirty = false;

    int erc = ensure_staged(h);
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
        ssize_t got = pread(h->tmp_fd, buf, size, offset);
        return got < 0 ? -errno : (int)got;
    }

    if (h->item_id == 0)
        return 0;

    unsigned char *data = NULL;
    unsigned int got = 0;
    int rc = LIBMTP_GetPartialObject(g_device, h->item_id, (uint64_t)offset,
                                     (uint32_t)size, &data, &got);
    if (rc == 0) {
        if (got > size)
            got = (unsigned int)size;
        memcpy(buf, data, got);
        free(data);
        return (int)got;
    }

    /* Fallback: device lacks GetPartialObject; download to local cache */
    free(data);
    rc = ensure_staged(h);
    if (rc != 0)
        return rc;

    ssize_t got_staged = pread(h->tmp_fd, buf, size, offset);
    return got_staged < 0 ? -errno : (int)got_staged;
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
    if (!fi)
        return -ENOSYS;

    mtp_handle_t *h = (mtp_handle_t *)(uintptr_t)fi->fh;
    int rc = ensure_staged(h);
    if (rc != 0)
        return rc;

    if (ftruncate(h->tmp_fd, size) != 0)
        return -errno;

    h->dirty = true;
    return 0;
}

/*
 * Uploads staged content back to the device safely.
 * Never deletes the old object until the new upload has successfully landed.
 */
static int op_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    mtp_handle_t *h = (mtp_handle_t *)(uintptr_t)fi->fh;
    int result = 0;

    if (h->dirty && h->tmp_fd >= 0) {
        struct stat st;
        if (fstat(h->tmp_fd, &st) == 0) {
            LIBMTP_file_t *meta = LIBMTP_new_file_t();
            if (!meta) {
                result = -ENOMEM;
            } else {
                meta->filename = strdup(h->name);
                meta->filesize = (uint64_t)st.st_size;
                meta->parent_id = h->parent_id;
                meta->storage_id = h->storage_id;
                meta->filetype = guess_filetype(h->name);

                lseek(h->tmp_fd, 0, SEEK_SET);

                /* Upload new file */
                if (LIBMTP_Send_File_From_File_Descriptor(g_device, h->tmp_fd, meta,
                                                         NULL, NULL) != 0) {
                    log_error("MTP upload failed for %s, retaining original file", h->name);
                    result = -EIO;
                } else {
                    /* Upload succeeded: delete the previous revision now */
                    if (h->item_id != 0 && h->item_id != meta->item_id) {
                        LIBMTP_Delete_Object(g_device, h->item_id);
                    }
                    log_info("MTP: uploaded %s (%llu bytes)",
                             h->name, (unsigned long long)st.st_size);
                }
                LIBMTP_destroy_file_t(meta);
            }
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
    if (r.item_id == 0)
        return -EPERM;

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
        return -EPERM;

    char name[MAX_NAME];
    safe_copy(name, sizeof(name), r.leaf);
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
        return -EPERM; /* Cannot delete a physical storage root */

    return LIBMTP_Delete_Object(g_device, r.item_id) == 0 ? 0 : -ENOTEMPTY;
}

static int op_rename(const char *from, const char *to, unsigned int flags)
{
    if (flags != 0)
        return -EINVAL;

    resolved_t src, dst;
    int rc = resolve_path(from, &src);
    if (rc != 0) return rc;
    if (!src.exists) return -ENOENT;
    if (src.item_id == 0) return -EPERM;

    rc = resolve_path(to, &dst);
    if (rc != 0) return rc;
    if (dst.exists) return -EEXIST;
    if (dst.storage_id == 0) return -EPERM;

    /* Cross-storage moves must report -EXDEV so cp/rm fallback triggers */
    if (src.storage_id != dst.storage_id)
        return -EXDEV;

    if (src.parent_id != dst.parent_id) {
        if (LIBMTP_Move_Object(g_device, src.item_id, dst.storage_id, dst.parent_id) != 0)
            return -EIO;
    }

    if (strcmp(src.leaf, dst.leaf) != 0) {
        char newname[MAX_NAME];
        safe_copy(newname, sizeof(newname), dst.leaf);
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

    if (!g_device)
        return -EIO;

    resolved_t r;
    if (resolve_path(path, &r) != 0 || !r.exists)
        return -ENOENT;

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
        log_error("MTP: could not locate raw device at bus %d addr %d (%s %s)",
                  info->usb_busnum, info->usb_devnum, info->id_vendor, info->id_model);
        rmdir(mountpoint);
        return 1;
    }

    g_device = LIBMTP_Open_Raw_Device_Uncached(raw);
    free(raw);
    if (!g_device) {
        log_error("MTP: failed to open raw device %s %s", info->id_vendor, info->id_model);
        rmdir(mountpoint);
        return 1;
    }

    if (LIBMTP_Get_Storage(g_device, LIBMTP_STORAGE_SORTBY_NOTSORTED) != 0 ||
        g_device->storage == NULL) {
        log_error("MTP: %s %s reported no storage (locked screen or wrong USB mode)",
                  info->id_vendor, info->id_model);
        LIBMTP_Release_Device(g_device);
        g_device = NULL;
        rmdir(mountpoint);
        return 1;
    }

    /* Assign ownership to regular user if invoked under sudo, or current process */
    const char *sudo_uid = getenv("SUDO_UID");
    const char *sudo_gid = getenv("SUDO_GID");
    g_owner_uid = sudo_uid ? (uid_t)atoi(sudo_uid) : getuid();
    g_owner_gid = sudo_gid ? (gid_t)atoi(sudo_gid) : getgid();

    log_info("MTP: mounting %s %s at %s", info->id_vendor, info->id_model, mountpoint);

    /*
     * Options:
     * -f: Run in foreground (parent handles supervisor duties).
     * -s: Single-threaded (libmtp handle is not thread-safe).
     * -o allow_other: Permit non-root desktop users to access the mount.
     */
    char *argv[] = {
        (char *)"ciel-mountd-mtp",
        (char *)"-f",
        (char *)"-s",
        (char *)"-o",
        (char *)"allow_other,default_permissions",
        (char *)mountpoint,
        NULL
    };
    int argc = 6;

    int rc = fuse_main(argc, argv, &mtp_ops, NULL);

    LIBMTP_Release_Device(g_device);
    g_device = NULL;

    rmdir(mountpoint);
    return rc;
}
