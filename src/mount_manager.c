/*
 * mount_manager.c - Removable media and MTP lifecycle manager.
 */

#include "mount_manager.h"
#include "config.h"
#include "device.h"
#include "log.h"
#include "mtp_fs.h"
#include "util.h"
#include "dbus_interface.h"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/cdrom.h>
#include <sys/ioctl.h>
#endif

typedef struct mounted_entry {
    char devnode[MAX_PROP_LEN];
    char mountpoint[MAX_PROP_LEN];
    char label[MAX_PROP_LEN];
    bool is_readonly;
    pid_t pid; /* 0 for block devices, PID for MTP FUSE processes */
    struct mounted_entry *next;
} mounted_entry_t;

static mounted_entry_t *g_mounted = NULL;

#define MAX_NAME_LEN 64

void mount_manager_init(void)
{
    g_mounted = NULL;
}

/*
 * Sanitizes directory component names.
 * Replaces non-alphanumerics with '_' and strictly forbids leading/trailing
 * dots to eliminate any possibility of path traversal (e.g. "..").
 */
static void sanitize(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len == 0)
        return;

    size_t j = 0;
    bool has_alnum = false;

    for (size_t i = 0; in[i] != '\0' && (j + 1) < out_len; i++) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
            has_alnum = true;
        } else if (c == '-' || c == '_') {
            out[j++] = c;
        } else if (c == '.' && j > 0) {
            /* Allow dots only after valid characters; never leading */
            out[j++] = '.';
        } else {
            /* Avoid consecutive underscores */
            if (j > 0 && out[j - 1] != '_')
                out[j++] = '_';
        }
    }
    out[j] = '\0';

    /* Strip any trailing dots or underscores */
    while (j > 0 && (out[j - 1] == '.' || out[j - 1] == '_')) {
        out[--j] = '\0';
    }

    /* Fallback if string is invalid, purely dots, or empty */
    if (j == 0 || !has_alnum || strcmp(out, ".") == 0 || strcmp(out, "..") == 0) {
        safe_copy(out, out_len, "volume");
    }
}

static void pick_base_name(const device_info_t *info, char out[MAX_NAME_LEN])
{
    if (info->id_fs_label[0] != '\0')
        sanitize(info->id_fs_label, out, MAX_NAME_LEN);
    else if (info->id_fs_uuid[0] != '\0')
        sanitize(info->id_fs_uuid, out, MAX_NAME_LEN);
    else
        sanitize(info->sysname, out, MAX_NAME_LEN);
}

/*
 * Atomically reserves and creates a unique directory under MOUNT_BASE_DIR.
 * Eliminates the TOCTOU race condition by using mkdir() directly.
 */
static int build_and_create_mount_point(const char *base_name, char *out, size_t out_len)
{
    for (int suffix = 0; suffix < 100; suffix++) {
        if (suffix == 0)
            snprintf(out, out_len, "%s/%s", MOUNT_BASE_DIR, base_name);
        else
            snprintf(out, out_len, "%s/%s-%d", MOUNT_BASE_DIR, base_name, suffix + 1);

        /* Atomic claim: if mkdir succeeds, we own the directory */
        if (mkdir(out, 0755) == 0)
            return 0;

        if (errno != EEXIST) {
            log_error("mkdir(%s) failed: %s", out, strerror(errno));
            return -1;
        }
    }

    log_error("Could not find a free mount point under %s for base '%s'",
              MOUNT_BASE_DIR, base_name);
    return -1;
}

/*
 * Formulates filesystem-specific mount options.
 */
static void build_options(const device_info_t *info, char *out, size_t out_len)
{
    const char *fstype = info->id_fs_type;

    if (strcmp(fstype, "vfat") == 0) {
        snprintf(out, out_len,
                 "uid=%d,gid=%d,fmask=0133,dmask=0022,shortname=mixed,utf8=1",
                 getuid(), getgid());
    } else if (strcmp(fstype, "exfat") == 0) {
        /* exfat does NOT support shortname or utf8 mount options */
        snprintf(out, out_len,
                 "uid=%d,gid=%d,fmask=0133,dmask=0022",
                 getuid(), getgid());
    } else if (strcmp(fstype, "ntfs3") == 0 || strcmp(fstype, "ntfs") == 0) {
        snprintf(out, out_len,
                 "uid=%d,gid=%d,fmask=0133,dmask=0022",
                 getuid(), getgid());
    } else {
        /* Native filesystems (ext4, btrfs, xfs, etc.) use their own metadata */
        out[0] = '\0';
    }
}

static void pick_mtp_name(const device_info_t *info, char out[MAX_NAME_LEN])
{
    char raw[MAX_PROP_LEN];
    if (info->id_model[0] != '\0')
        safe_copy(raw, sizeof(raw), info->id_model);
    else if (info->id_vendor[0] != '\0')
        snprintf(raw, sizeof(raw), "%s-device", info->id_vendor);
    else
        safe_copy(raw, sizeof(raw), "mtp-device");

    sanitize(raw, out, MAX_NAME_LEN);
}

static int handle_add_mtp(const device_info_t *info)
{
    if (info->usb_busnum < 0 || info->usb_devnum < 0) {
        log_warn("MTP device %s %s has invalid bus address, skipping",
                 info->id_vendor, info->id_model);
        return -1;
    }

    /* Prevent duplicate mounts */
    for (mounted_entry_t *e = g_mounted; e; e = e->next) {
        if (strcmp(e->devnode, info->devnode) == 0) {
            log_debug("MTP device %s already mounted at %s", info->devnode, e->mountpoint);
            return 0;
        }
    }

    char base_name[MAX_NAME_LEN];
    char prefixed_name[MAX_NAME_LEN];
    pick_mtp_name(info, base_name);
    snprintf(prefixed_name, sizeof(prefixed_name), "mtp-%s", base_name);

    char mountpoint[MAX_PROP_LEN];
    if (build_and_create_mount_point(prefixed_name, mountpoint, sizeof(mountpoint)) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        log_error("fork() for MTP mount failed: %s", strerror(errno));
        rmdir(mountpoint);
        return -1;
    }

    if (pid == 0) {
        int rc = mtp_fs_run(info, mountpoint);
        _exit(rc == 0 ? 0 : 1);
    }

    mounted_entry_t *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        log_error("Out of memory tracking MTP mount: %s", info->devnode);
        kill(pid, SIGTERM);
        rmdir(mountpoint);
        return -1;
    }

    safe_copy(entry->devnode,    sizeof(entry->devnode),    info->devnode);
    safe_copy(entry->mountpoint, sizeof(entry->mountpoint), mountpoint);
    safe_copy(entry->label,      sizeof(entry->label),      base_name);
    entry->is_readonly = false;
    entry->pid = pid;
    entry->next = g_mounted;
    g_mounted = entry;

    dbus_emit_device_mounted(entry->devnode, entry->label, entry->mountpoint);
    log_info("MTP: launched mount process (pid %d) for %s at %s",
             (int)pid, info->devnode, mountpoint);
    return 0;
}

int mount_manager_handle_add(const device_info_t *info)
{
    if (info->class == DEV_CLASS_MTP)
        return handle_add_mtp(info);

    if (!info->is_removable) {
        log_debug("Ignoring non-removable device: %s", info->devnode);
        return 0;
    }

    if (info->id_fs_type[0] == '\0') {
        log_debug("Ignoring %s: no filesystem detected", info->devnode);
        return 0;
    }

    /* Prevent duplicate mounts */
    for (mounted_entry_t *e = g_mounted; e; e = e->next) {
        if (strcmp(e->devnode, info->devnode) == 0) {
            log_debug("Device %s is already mounted at %s", info->devnode, e->mountpoint);
            return 0;
        }
    }

    char base_name[MAX_NAME_LEN];
    pick_base_name(info, base_name);

    char mountpoint[MAX_PROP_LEN];
    if (build_and_create_mount_point(base_name, mountpoint, sizeof(mountpoint)) != 0)
        return -1;

    char options[MAX_PROP_LEN];
    build_options(info, options, sizeof(options));

    unsigned long flags = MS_NOSUID | MS_NODEV;
    bool is_readonly = false;

    /* Initial mount attempt (Read-Write) */
    if (mount(info->devnode, mountpoint, info->id_fs_type, flags,
              options[0] ? options : NULL) != 0) {
        
        /* Fallback for write-protected or read-only devices */
        if (errno == EROFS || errno == EACCES) {
            log_info("%s is write-protected, retrying with MS_RDONLY", info->devnode);
            flags |= MS_RDONLY;
            if (mount(info->devnode, mountpoint, info->id_fs_type, flags,
                      options[0] ? options : NULL) == 0) {
                is_readonly = true;
            }
        }

        if (!is_readonly) {
            log_error("mount(%s -> %s, type=%s) failed: %s",
                      info->devnode, mountpoint, info->id_fs_type, strerror(errno));
            rmdir(mountpoint);
            return -1;
        }
    }

    mounted_entry_t *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        log_error("Out of memory tracking mount of %s", info->devnode);
        umount2(mountpoint, MNT_DETACH);
        rmdir(mountpoint);
        return -1;
    }

    safe_copy(entry->devnode,    sizeof(entry->devnode),    info->devnode);
    safe_copy(entry->mountpoint, sizeof(entry->mountpoint), mountpoint);
    safe_copy(entry->label,      sizeof(entry->label),      info->id_fs_label);
    entry->is_readonly = is_readonly;
    entry->pid = 0;
    entry->next = g_mounted;
    g_mounted = entry;

    log_info("Mounted %s (%s%s) at %s",
             info->devnode, info->id_fs_type,
             is_readonly ? ", ro" : "", mountpoint);

    dbus_emit_device_mounted(entry->devnode,
                             entry->label[0] ? entry->label : "(none)",
                             entry->mountpoint);
    return 0;
}

static bool perform_eject_ioctl(const char *devnode)
{
#ifdef CDROMEJECT
    int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        ioctl(fd, CDROMEJECT);
        close(fd);
        return true;
    }
#else
    (void)devnode;
#endif
    return false;
}

static void unmount_entry(mounted_entry_t *entry)
{
    if (entry->pid != 0) {
        /* MTP cleanup */
        if (kill(entry->pid, SIGTERM) == 0) {
            /* Give FUSE child a short window to exit cleanly */
            usleep(50000);
        }
        umount2(entry->mountpoint, MNT_DETACH);
        rmdir(entry->mountpoint);
        log_info("MTP: unmounted %s (pid %d)", entry->mountpoint, (int)entry->pid);
        dbus_emit_device_unmounted(entry->devnode);
        free(entry);
        return;
    }

    /* Standard block device unmount */
    if (umount2(entry->mountpoint, 0) != 0) {
        if (errno == EBUSY) {
            log_warn("%s is busy; detaching lazily", entry->mountpoint);
            umount2(entry->mountpoint, MNT_DETACH);
        } else {
            log_warn("umount2(%s) failed: %s", entry->mountpoint, strerror(errno));
        }
    }

    /* Clean up the directory */
    if (rmdir(entry->mountpoint) != 0 && errno != ENOENT) {
        log_debug("rmdir(%s) deferred (lazy unmount in progress): %s",
                  entry->mountpoint, strerror(errno));
    }

    perform_eject_ioctl(entry->devnode);

    log_info("Unmounted %s from %s", entry->devnode, entry->mountpoint);
    dbus_emit_device_unmounted(entry->devnode);

    free(entry);
}

int mount_manager_handle_remove(const device_info_t *info)
{
    mounted_entry_t **link = &g_mounted;
    while (*link) {
        if (strcmp((*link)->devnode, info->devnode) == 0) {
            mounted_entry_t *found = *link;
            *link = found->next;
            unmount_entry(found);
            return 0;
        }
        link = &(*link)->next;
    }

    log_debug("Remove event for untracked device %s", info->devnode);
    return 0;
}

const MountedVolume *mount_manager_get_volumes(size_t *out_count)
{
    static MountedVolume snapshot[64];
    memset(snapshot, 0, sizeof(snapshot));
    size_t n = 0;

    for (mounted_entry_t *e = g_mounted; e && n < 64; e = e->next) {
        safe_copy(snapshot[n].device_path, sizeof(snapshot[n].device_path), e->devnode);
        safe_copy(snapshot[n].label,       sizeof(snapshot[n].label),       e->label);
        safe_copy(snapshot[n].mount_point, sizeof(snapshot[n].mount_point), e->mountpoint);
        snapshot[n].is_readonly = e->is_readonly;
        n++;
    }
    *out_count = n;
    return snapshot;
}

bool mount_manager_eject(const char *device_path, char *err_buf, size_t err_len)
{
    if (!device_path || !*device_path) {
        if (err_buf && err_len)
            snprintf(err_buf, err_len, "Invalid device path");
        return false;
    }

    mounted_entry_t **link = &g_mounted;
    while (*link) {
        /* Allow matching by devnode OR mountpoint */
        if (strcmp((*link)->devnode, device_path) == 0 ||
            strcmp((*link)->mountpoint, device_path) == 0) {
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
        snprintf(err_buf, err_len, "Device not tracked by mount manager");

    dbus_emit_eject_failed(device_path, err_buf ? err_buf : "Device not mounted");
    return false;
}

/*
 * Probes block device metadata directly from udev's runtime database
 * (/run/udev/data/b<maj>:<min>). Enables manual DBus MountDevice() calls.
 */
static bool probe_block_device(const char *devnode, device_info_t *out_info)
{
    struct stat st;
    if (stat(devnode, &st) != 0 || !S_ISBLK(st.st_mode))
        return false;

    memset(out_info, 0, sizeof(*out_info));
    safe_copy(out_info->devnode, sizeof(out_info->devnode), devnode);
    out_info->class = DEV_CLASS_BLOCK_PARTITION;
    out_info->is_removable = true;

    const char *slash = strrchr(devnode, '/');
    safe_copy(out_info->sysname, sizeof(out_info->sysname), slash ? slash + 1 : devnode);

    char udev_path[128];
    snprintf(udev_path, sizeof(udev_path), "/run/udev/data/b%u:%u",
             major(st.st_rdev), minor(st.st_rdev));

    FILE *fp = fopen(udev_path, "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (strncmp(line, "E:ID_FS_TYPE=", 13) == 0)
                safe_copy(out_info->id_fs_type, sizeof(out_info->id_fs_type), line + 13);
            else if (strncmp(line, "E:ID_FS_LABEL=", 14) == 0)
                safe_copy(out_info->id_fs_label, sizeof(out_info->id_fs_label), line + 14);
            else if (strncmp(line, "E:ID_FS_UUID=", 13) == 0)
                safe_copy(out_info->id_fs_uuid, sizeof(out_info->id_fs_uuid), line + 13);
        }
        fclose(fp);
    }

    return out_info->id_fs_type[0] != '\0';
}

bool mount_manager_mount(const char *device_path)
{
    if (!device_path || !*device_path)
        return false;

    device_info_t info;
    if (!probe_block_device(device_path, &info)) {
        log_error("Could not detect filesystem on %s", device_path);
        return false;
    }

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
