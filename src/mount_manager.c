/*
 * mount_manager.c - see mount_manager.h
 *
 * Deliberately uses the raw mount(2)/umount2(2) syscalls rather than
 * shelling out to `/bin/mount` or linking libmount. We already get
 * everything mount(2) needs (filesystem type, and enough identity to
 * build a mount point) straight from udev properties, so there's no
 * parsing work libmount would otherwise save us - and avoiding the
 * dependency keeps the codebase easier to read start-to-finish.
 * If you later want automatic fstype detection/fallback the way
 * `mount -t auto` does, that's the natural place to bring libmount in
 * (see README.md, "Known limitations").
 */

#include "mount_manager.h"
#include "config.h"
#include "log.h"
#include "mtp_fs.h"
#include "util.h"
#include "dbus_interface.h"   /* for the emit helpers */

#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* One entry per filesystem we currently have mounted, so a later REMOVE
 * event (or shutdown) knows what to unmount and which directory to
 * rmdir(). A simple singly-linked list is plenty - this daemon expects
 * to track a handful of removable volumes, not thousands.
 *
 * Doubles up for both kinds of mount this daemon does:
 *   - block devices: pid == 0, we unmount directly with umount2().
 *   - MTP devices:   pid != 0, it's the pid of the child process
 *     running mtp_fs_run() (see mtp_fs.c); we ask it to unmount by
 *     signaling it, since only that process holds the open MTP session. */
typedef struct mounted_entry {
    char devnode[MAX_PROP_LEN];
    char mountpoint[MAX_PROP_LEN];
    char label[MAX_PROP_LEN];
    bool is_readonly;
    pid_t pid;
    struct mounted_entry *next;
} mounted_entry_t;

static mounted_entry_t *g_mounted = NULL;

/* Volume labels/UUIDs/device names are capped to this length when used
 * to build a directory name - real ones are always far shorter (FAT
 * labels max out at 11 chars, ext4 at 16), so this only ever bites
 * something pathological, and keeping it well under MAX_PROP_LEN means
 * "%s/%s-%d" can never come close to overflowing the mountpoint buffer. */
#define MAX_NAME_LEN 64

void mount_manager_init(void)
{
    g_mounted = NULL;
}

/* Replace anything that isn't [A-Za-z0-9._-] with '_', so a volume label
 * or model string full of spaces/slashes/unicode-junk can never be used
 * to escape MOUNT_BASE_DIR or otherwise confuse the filesystem. */
static void sanitize(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < out_len; i++) {
        char c = in[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        out[j++] = ok ? c : '_';
    }
    out[j] = '\0';
    if (j == 0)
        safe_copy(out, out_len, "volume");
}

/* Picks the best available human-meaningful name for the mount point:
 * filesystem label > filesystem UUID > kernel device name (sdb1). In
 * that order, because a label is what a user actually recognizes their
 * USB stick by. */
static void pick_base_name(const device_info_t *info, char out[MAX_NAME_LEN])
{
    if (info->id_fs_label[0] != '\0')
        sanitize(info->id_fs_label, out, MAX_NAME_LEN);
    else if (info->id_fs_uuid[0] != '\0')
        sanitize(info->id_fs_uuid, out, MAX_NAME_LEN);
    else
        sanitize(info->sysname, out, MAX_NAME_LEN);
}

/* Builds a mount point path under MOUNT_BASE_DIR that doesn't already
 * exist, appending -2, -3, ... if the plain name collides (e.g. two
 * unlabeled "sdb1"-named sticks plugged in over the same session, or two
 * drives that happen to share a volume label). */
static int build_mount_point(const device_info_t *info, char *out, size_t out_len)
{
    char base_name[MAX_NAME_LEN];
    pick_base_name(info, base_name);

    for (int suffix = 0; suffix < 100; suffix++) {
        struct stat st;
        if (suffix == 0)
            snprintf(out, out_len, "%s/%s", MOUNT_BASE_DIR, base_name);
        else
            snprintf(out, out_len, "%s/%s-%d", MOUNT_BASE_DIR, base_name, suffix + 1);

        if (stat(out, &st) != 0 && errno == ENOENT)
            return 0; /* free path found */
    }

    log_error("could not find a free mount point for %s under %s",
              info->devnode, MOUNT_BASE_DIR);
    return -1;
}

/* Builds the mount(2) `data` option string. Different filesystem drivers
 * accept wildly different options; we only set uid=/gid=/umask= for the
 * FAT/NTFS-family drivers that support mapping ownership to a Unix uid,
 * since e.g. ext4 or btrfs will simply reject unknown mount options. */
static void build_options(const device_info_t *info, char *out, size_t out_len)
{
    const char *fstype = info->id_fs_type;

    if (strcmp(fstype, "vfat") == 0 || strcmp(fstype, "exfat") == 0) {
        snprintf(out, out_len,
                 "uid=%d,gid=%d,umask=000,shortname=mixed,utf8=1",
                 getuid(), getgid());
    } else if (strcmp(fstype, "ntfs3") == 0 || strcmp(fstype, "ntfs") == 0) {
        snprintf(out, out_len, "uid=%d,gid=%d,umask=000", getuid(), getgid());
    } else {
        /* ext2/3/4, btrfs, xfs, f2fs, iso9660, udf, ... - ownership on
         * these comes from the filesystem's own inode metadata, not
         * mount options, so we pass nothing extra. */
        out[0] = '\0';
    }
}

/* Builds a mount-point name for an MTP device from vendor/model/serial
 * (MTP devices have no filesystem label to draw on the way block
 * devices do). Falls back gracefully as fields are missing. */
static void pick_mtp_name(const device_info_t *info, char out[MAX_NAME_LEN])
{
    /* Sized to MAX_PROP_LEN (id_model/id_vendor's own buffer size), not
     * MAX_NAME_LEN, purely so the copies below can never truncate -
     * sanitize() at the end is what actually enforces the MAX_NAME_LEN
     * limit on the final directory name. */
    char raw[MAX_PROP_LEN];
    if (info->id_model[0] != '\0')
        safe_copy(raw, sizeof(raw), info->id_model);
    else if (info->id_vendor[0] != '\0')
        snprintf(raw, sizeof(raw), "%.248s-device", info->id_vendor);
    else
        safe_copy(raw, sizeof(raw), "mtp-device");
    sanitize(raw, out, MAX_NAME_LEN);
}

static int handle_add_mtp(const device_info_t *info)
{
    if (info->usb_busnum < 0 || info->usb_devnum < 0) {
        log_warn("MTP device %s %s has no usable bus/dev address, skipping",
                  info->id_vendor, info->id_model);
        return -1;
    }

    char base_name[MAX_NAME_LEN];
    pick_mtp_name(info, base_name);

    char mountpoint[MAX_PROP_LEN];
    snprintf(mountpoint, sizeof(mountpoint), "%s/mtp-%s", MOUNT_BASE_DIR, base_name);

    if (mkdir(mountpoint, 0755) != 0 && errno != EEXIST) {
        log_error("mkdir(%s) failed: %s", mountpoint, strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_error("fork() for MTP mount failed: %s", strerror(errno));
        rmdir(mountpoint);
        return -1;
    }

    if (pid == 0) {
        /* Child: becomes the dedicated FUSE/libmtp process for this one
         * device for as long as it stays plugged in. mtp_fs_run() blocks
         * until the mount is torn down (normally via SIGTERM from
         * unmount_entry() below, which FUSE's built-in signal handling
         * turns into a clean unmount+cleanup). */
        int rc = mtp_fs_run(info, mountpoint);
        _exit(rc == 0 ? 0 : 1);
    }

    /* Parent: just remember the child so a later remove event or daemon
     * shutdown can signal it. We deliberately don't waitpid() here -
     * main.c sets SIGCHLD to SIG_IGN, which auto-reaps children, and
     * blocking the event loop on this child's exit would stop us from
     * handling any other device that shows up while a phone is slow to
     * unmount. */
    mounted_entry_t *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        log_error("out of memory tracking MTP mount of %s", info->devnode);
        kill(pid, SIGTERM);
        rmdir(mountpoint);
        return -1;
    }

    safe_copy(entry->devnode,    sizeof(entry->devnode),    info->devnode);
    safe_copy(entry->mountpoint, sizeof(entry->mountpoint), mountpoint);
    if (info->id_fs_label[0])
        safe_copy(entry->label, sizeof(entry->label), info->id_fs_label);
    else
        entry->label[0] = '\0';
    entry->is_readonly = false;   /* extend later if you parse mount flags */
    entry->pid = pid;             /* 0 for block devices */
    entry->next = g_mounted;
    g_mounted = entry;

    dbus_emit_device_mounted(entry->devnode,
                            entry->label[0] ? entry->label : "(none)",
                            entry->mountpoint);

    log_info("MTP: launched mount process (pid %d) for %s %s at %s",
              (int)pid, info->id_vendor, info->id_model, mountpoint);
    return 0;
}

int mount_manager_handle_add(const device_info_t *info)
{
    if (info->class == DEV_CLASS_MTP)
        return handle_add_mtp(info);

    if (!info->is_removable) {
        log_debug("ignoring non-removable device %s", info->devnode);
        return 0;
    }

    if (info->class == DEV_CLASS_BLOCK_DISK) {
        /* A whole-disk device with no partition table at all (a
         * "superfloppy" layout) can itself hold a filesystem directly -
         * but if it has partitions, we want to mount *those*, not the
         * raw disk, so we only proceed here when there's a filesystem
         * type directly on the disk node. */
        if (info->id_fs_type[0] == '\0') {
            log_debug("disk %s has no direct filesystem (likely "
                      "partitioned) - waiting for partition events",
                      info->devnode);
            return 0;
        }
    }

    if (info->id_fs_type[0] == '\0') {
        log_debug("ignoring %s: no recognized filesystem (unformatted, "
                  "or an extended/protective partition)", info->devnode);
        return 0;
    }

    char mountpoint[MAX_PROP_LEN];
    if (build_mount_point(info, mountpoint, sizeof(mountpoint)) != 0)
        return -1;

    if (mkdir(mountpoint, 0755) != 0 && errno != EEXIST) {
        log_error("mkdir(%s) failed: %s", mountpoint, strerror(errno));
        return -1;
    }

    char options[MAX_PROP_LEN];
    build_options(info, options, sizeof(options));

    /* MS_NOSUID / MS_NODEV: refuse setuid binaries and device nodes on
     * removable media, a standard hardening measure (this mirrors what
     * udisks2 does by default) since we don't want a malicious USB stick
     * to plant a setuid-root binary. Adjust here if your use case needs
     * to run programs directly off removable media. */
    unsigned long flags = MS_NOSUID | MS_NODEV;

    if (mount(info->devnode, mountpoint, info->id_fs_type, flags,
              options[0] ? options : NULL) != 0) {
        log_error("mount(%s -> %s, type=%s) failed: %s",
                  info->devnode, mountpoint, info->id_fs_type, strerror(errno));
        rmdir(mountpoint); /* best-effort cleanup of the empty dir */
        return -1;
    }

    mounted_entry_t *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        log_error("out of memory tracking mount of %s", info->devnode);
        umount2(mountpoint, MNT_DETACH);
        rmdir(mountpoint);
        return -1;
    }
    safe_copy(entry->devnode, sizeof(entry->devnode), info->devnode);
    safe_copy(entry->mountpoint, sizeof(entry->mountpoint), mountpoint);
    entry->next = g_mounted;
    g_mounted = entry;

    log_info("mounted %s (%s, label=\"%s\") at %s",
              info->devnode, info->id_fs_type,
              info->id_fs_label[0] ? info->id_fs_label : "(none)",
              mountpoint);
    return 0;
}

/* Shared by handle_remove() and shutdown: tears down one tracked entry
 * and removes its mount point directory where we can. Unlinks it from
 * the list and frees it - callers must not touch *entry again after. */
static void unmount_entry(mounted_entry_t *entry)
{
    if (entry->pid != 0) {
        /* MTP: ask the dedicated child process to unmount itself. FUSE's
         * default signal handling turns SIGTERM into a clean
         * fuse_session_exit(), which unmounts before that process
         * exits - see mtp_fs.c. We don't rmdir() here: only the child
         * knows once the unmount has actually finished, so this is
         * intentionally fire-and-forget rather than the parent racing
         * to remove a directory that might still be a live mountpoint.
         * A future improvement could have the child rmdir() its own
         * mountpoint right before exiting, once it's done unmounting. */
        if (kill(entry->pid, SIGTERM) != 0 && errno != ESRCH)
            log_warn("could not signal MTP mount process %d: %s",
                      (int)entry->pid, strerror(errno));
        log_info("MTP: requested unmount of %s (pid %d)",
                  entry->mountpoint, (int)entry->pid);
        free(entry);
        return;
    }

    /* Plain umount2() first; if something still has a file open on the
     * volume (or a shell is cd'd into it) that fails with EBUSY, so we
     * fall back to a lazy/detach unmount, which succeeds immediately and
     * finishes the actual detach once the volume is no longer in use.
     * This mirrors what `udisks2`/`umount -l` do and matters especially
     * on shutdown: we'd rather detach late than hang the daemon. */
    if (umount2(entry->mountpoint, 0) != 0) {
        if (errno == EBUSY) {
            log_warn("%s busy, detaching lazily", entry->mountpoint);
            umount2(entry->mountpoint, MNT_DETACH);
        } else {
            log_warn("umount2(%s) failed: %s", entry->mountpoint, strerror(errno));
        }
    }

    if (rmdir(entry->mountpoint) != 0)
        log_debug("rmdir(%s) failed: %s", entry->mountpoint, strerror(errno));

    log_info("unmounted %s from %s", entry->devnode, entry->mountpoint);
    dbus_emit_device_unmounted(entry->devnode);

    free(entry);
}

int mount_manager_handle_remove(const device_info_t *info)
{
    mounted_entry_t **link = &g_mounted;
    while (*link) {
        if (strcmp((*link)->devnode, info->devnode) == 0) {
            mounted_entry_t *found = *link;
            *link = found->next; /* unlink before freeing */
            unmount_entry(found);
            return 0;
        }
        link = &(*link)->next;
    }

    /* Not an error: this happens for every device we deliberately didn't
     * mount (non-removable disks, unformatted partitions, MTP, ...). */
    log_debug("remove event for untracked device %s (nothing to do)",
              info->devnode);
    return 0;
}

const MountedVolume *mount_manager_get_volumes(size_t *out_count)
{
    /* Snapshot into a static buffer – fine for a daemon that tracks a
     * handful of volumes.  Callers must not free the returned pointer. */
    static MountedVolume snapshot[64];
    size_t n = 0;

    for (mounted_entry_t *e = g_mounted; e && n < 64; e = e->next) {
        safe_copy(snapshot[n].device_path,  sizeof(snapshot[n].device_path),  e->devnode);
        safe_copy(snapshot[n].label,        sizeof(snapshot[n].label),        e->label);
        safe_copy(snapshot[n].mount_point,  sizeof(snapshot[n].mount_point),  e->mountpoint);
        snapshot[n].is_readonly = e->is_readonly;
        n++;
    }
    *out_count = n;
    return snapshot;
}

bool mount_manager_eject(const char *device_path, char *err_buf, size_t err_len)
{
    mounted_entry_t **link = &g_mounted;
    while (*link) {
        if (strcmp((*link)->devnode, device_path) == 0) {
            mounted_entry_t *found = *link;
            *link = found->next;
            unmount_entry(found);
            if (err_buf && err_len)
                err_buf[0] = '\0';
            return true;
        }
        link = &(*link)->next;
    }
    if (err_buf && err_len)
        snprintf(err_buf, err_len, "device not mounted by this daemon");
    return false;
}

bool mount_manager_mount(const char *device_path)
{
    if (!device_path || !*device_path)
        return false;

    device_info_t info = {0};
    safe_copy(info.devnode, sizeof(info.devnode), device_path);
    info.class        = DEV_CLASS_BLOCK_DISK;   /* or DEV_CLASS_BLOCK_PART */
    info.is_removable = true;

    /* Optional: you can leave id_fs_type empty and let handle_add
     * reject unformatted devices, or you can add a quick
     * blkid / libmount probe here later. */

    return mount_manager_handle_add(&info) == 0;
}

void mount_manager_shutdown(void)
{
    while (g_mounted) {
        mounted_entry_t *entry = g_mounted;
        g_mounted = entry->next;
        unmount_entry(entry);
    }
}
