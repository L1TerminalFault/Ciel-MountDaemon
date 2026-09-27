/*
 * udev_monitor.c - see udev_monitor.h
 *
 * Two udev concepts worth knowing before reading this file:
 *
 *   - "enumerate" = a synchronous snapshot query ("what block devices
 *     exist right now?"). We use this once at startup.
 *   - "monitor"   = a netlink socket that streams add/remove/change
 *     events as they happen. We use this for the daemon's main loop.
 *
 * Both hand us `struct udev_device *` objects; fill_device_info() below
 * is the one place that reads properties off of them and turns them
 * into our own plain device_info_t, so the rest of the codebase never
 * needs to know libudev exists.
 */

#include "udev_monitor.h"
#include "log.h"
#include "util.h"

#include <libudev.h>
#include <string.h>
#include <stdlib.h>

struct udev_ctx {
    struct udev *udev;
    struct udev_monitor *mon;
};

/* Small helper: copy a udev property into a fixed-size buffer, tolerating
 * the property being absent (udev_device_get_property_value returns NULL
 * for unset properties - e.g. a freshly-formatted disk with no label). */
static void copy_prop(struct udev_device *dev, const char *key,
                       char *out, size_t out_len)
{
    const char *val = udev_device_get_property_value(dev, key);
    safe_copy(out, out_len, val); /* val may be NULL: property unset */
}

/* Is this block device on removable media? Two independent signals:
 *   1. ID_BUS=usb (or mmc, for SD cards) - set by udev's usb/mmc rules.
 *   2. The "removable" sysfs attribute of the *disk* device (not the
 *      partition) - some USB enclosures/bus types don't set ID_BUS
 *      reliably, so this is a useful fallback.
 */
static bool detect_removable(struct udev_device *dev, const char *id_bus)
{
    if (strcmp(id_bus, "usb") == 0 || strcmp(id_bus, "mmc") == 0)
        return true;

    /* Walk up to the parent "disk" device (a partition's parent is the
     * whole-disk device) and check its removable attribute. For a device
     * that's already the whole disk, this just returns dev's own parent
     * chain in the block subsystem, which still works. */
    struct udev_device *disk =
        udev_device_get_parent_with_subsystem_devtype(dev, "block", "disk");
    if (!disk)
        return false;

    const char *removable = udev_device_get_sysattr_value(disk, "removable");
    return removable && strcmp(removable, "1") == 0;
}

/* Fills a device_info_t for a subsystem=="block" udev_device. */
static void fill_block_info(struct udev_device *dev, device_info_t *info)
{
    const char *devnode = udev_device_get_devnode(dev);
    const char *syspath = udev_device_get_syspath(dev);
    const char *sysname = udev_device_get_sysname(dev);
    const char *devtype = udev_device_get_devtype(dev); /* "disk"|"partition" */

    safe_copy(info->devnode, sizeof(info->devnode), devnode);
    safe_copy(info->syspath, sizeof(info->syspath), syspath);
    safe_copy(info->sysname, sizeof(info->sysname), sysname);

    info->is_partition = devtype && strcmp(devtype, "partition") == 0;
    info->class = info->is_partition ? DEV_CLASS_BLOCK_PARTITION
                                      : DEV_CLASS_BLOCK_DISK;

    copy_prop(dev, "ID_BUS",       info->id_bus,       sizeof(info->id_bus));
    copy_prop(dev, "ID_FS_TYPE",   info->id_fs_type,   sizeof(info->id_fs_type));
    copy_prop(dev, "ID_FS_LABEL",  info->id_fs_label,  sizeof(info->id_fs_label));
    copy_prop(dev, "ID_FS_UUID",   info->id_fs_uuid,   sizeof(info->id_fs_uuid));
    copy_prop(dev, "ID_VENDOR",    info->id_vendor,    sizeof(info->id_vendor));
    copy_prop(dev, "ID_MODEL",     info->id_model,     sizeof(info->id_model));
    copy_prop(dev, "ID_SERIAL",    info->id_serial,    sizeof(info->id_serial));

    info->is_removable = detect_removable(dev, info->id_bus);
}

/* Fills a device_info_t for a subsystem=="usb", devtype=="usb_device"
 * udev_device, used only for MTP detection.
 *
 * MTP heuristic: distros that ship libmtp install a udev rule
 * (commonly 69-libmtp.rules) that runs `mtp-probe` against every new USB
 * device and, if it identifies as an MTP responder, sets the
 * ID_MTP_DEVICE=1 and ID_MEDIA_PLAYER properties on it. We just read
 * that property rather than re-implementing PTP/MTP protocol probing
 * ourselves. If ID_MTP_DEVICE never shows up on your system, install
 * `mtp-tools`/`libmtp-dev` (Debian/Ubuntu) or check that
 * 69-libmtp.rules is present in /usr/lib/udev/rules.d/. */
static void fill_usb_info(struct udev_device *dev, device_info_t *info)
{
    const char *is_mtp = udev_device_get_property_value(dev, "ID_MTP_DEVICE");
    if (!is_mtp || strcmp(is_mtp, "1") != 0) {
        info->class = DEV_CLASS_UNKNOWN; /* an ordinary USB device: ignore */
        return;
    }

    info->class = DEV_CLASS_MTP;

    const char *devnode = udev_device_get_devnode(dev); /* /dev/bus/usb/.../.. */
    const char *syspath = udev_device_get_syspath(dev);
    const char *sysname = udev_device_get_sysname(dev);
    safe_copy(info->devnode, sizeof(info->devnode), devnode);
    safe_copy(info->syspath, sizeof(info->syspath), syspath);
    safe_copy(info->sysname, sizeof(info->sysname), sysname);

    copy_prop(dev, "ID_VENDOR", info->id_vendor, sizeof(info->id_vendor));
    copy_prop(dev, "ID_MODEL",  info->id_model,  sizeof(info->id_model));
    copy_prop(dev, "ID_SERIAL", info->id_serial, sizeof(info->id_serial));
    safe_copy(info->id_bus, sizeof(info->id_bus), "usb");
    info->is_removable = true;

    /* These sysfs attributes live directly on a usb_device node (unlike
     * the block-device case, no parent walk needed) and let mtp_fs.c
     * open the exact same physical device via libmtp's raw-device list,
     * which only identifies devices by bus/address, not by devnode. */
    const char *busnum = udev_device_get_sysattr_value(dev, "busnum");
    const char *devnum = udev_device_get_sysattr_value(dev, "devnum");
    info->usb_busnum = busnum ? atoi(busnum) : -1;
    info->usb_devnum = devnum ? atoi(devnum) : -1;
}

/* Central dispatch: turns a raw udev_device + action string into our
 * device_info_t, or returns false if this device isn't one we handle
 * (e.g. a plain USB device that isn't MTP, or a block device with no
 * devnode such as a CD-ROM drive with no disc inserted). */
static bool fill_device_info(struct udev_device *dev, const char *action_str,
                              device_info_t *info)
{
    memset(info, 0, sizeof(*info));

    if (!action_str)
        info->action = DEV_ACTION_UNKNOWN;
    else if (strcmp(action_str, "add") == 0)
        info->action = DEV_ACTION_ADD;
    else if (strcmp(action_str, "remove") == 0)
        info->action = DEV_ACTION_REMOVE;
    else if (strcmp(action_str, "change") == 0)
        info->action = DEV_ACTION_CHANGE;
    else
        info->action = DEV_ACTION_UNKNOWN;

    const char *subsystem = udev_device_get_subsystem(dev);
    if (!subsystem)
        return false;

    if (strcmp(subsystem, "block") == 0) {
        if (!udev_device_get_devnode(dev))
            return false; /* e.g. empty optical drive slot */
        fill_block_info(dev, info);
        return true;
    }

    if (strcmp(subsystem, "usb") == 0) {
        fill_usb_info(dev, info);
        return info->class == DEV_CLASS_MTP; /* discard non-MTP usb noise */
    }

    return false;
}

udev_ctx_t *udev_monitor_create(void)
{
    udev_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->udev = udev_new();
    if (!ctx->udev) {
        log_error("udev_new() failed");
        free(ctx);
        return NULL;
    }

    /* "udev" (not "kernel") gives us events *after* udev has finished
     * running its rules and populated ID_FS_TYPE/ID_MTP_DEVICE/etc, which
     * is exactly the enriched data fill_device_info() relies on. */
    ctx->mon = udev_monitor_new_from_netlink(ctx->udev, "udev");
    if (!ctx->mon) {
        log_error("udev_monitor_new_from_netlink() failed");
        udev_unref(ctx->udev);
        free(ctx);
        return NULL;
    }

    udev_monitor_filter_add_match_subsystem_devtype(ctx->mon, "block", NULL);
    udev_monitor_filter_add_match_subsystem_devtype(ctx->mon, "usb", "usb_device");

    if (udev_monitor_enable_receiving(ctx->mon) < 0) {
        log_error("udev_monitor_enable_receiving() failed");
        udev_monitor_unref(ctx->mon);
        udev_unref(ctx->udev);
        free(ctx);
        return NULL;
    }

    return ctx;
}

void udev_monitor_destroy(udev_ctx_t *ctx)
{
    if (!ctx)
        return;
    if (ctx->mon)
        udev_monitor_unref(ctx->mon);
    if (ctx->udev)
        udev_unref(ctx->udev);
    free(ctx);
}

int udev_monitor_fd(const udev_ctx_t *ctx)
{
    return udev_monitor_get_fd(ctx->mon); /* libudev's function, different signature */
}

void udev_monitor_process(udev_ctx_t *ctx, device_event_cb cb, void *user_data)
{
    /* The netlink socket is non-blocking-ish in the sense that once
     * drained, receive_device() returns NULL - so loop until it does,
     * in case several events arrived in a single epoll wakeup. */
    struct udev_device *dev;
    while ((dev = udev_monitor_receive_device(ctx->mon)) != NULL) {
        const char *action = udev_device_get_action(dev);

        device_info_t info;
        if (fill_device_info(dev, action, &info))
            cb(&info, user_data);

        udev_device_unref(dev);
    }
}

void udev_enumerate_existing(udev_ctx_t *ctx, device_event_cb cb, void *user_data)
{
    struct udev_enumerate *en = udev_enumerate_new(ctx->udev);
    if (!en) {
        log_error("udev_enumerate_new() failed");
        return;
    }

    udev_enumerate_add_match_subsystem(en, "block");
    udev_enumerate_scan_devices(en);

    struct udev_list_entry *entry;
    struct udev_list_entry *devices = udev_enumerate_get_list_entry(en);

    udev_list_entry_foreach(entry, devices) {
        const char *syspath = udev_list_entry_get_name(entry);
        struct udev_device *dev = udev_device_new_from_syspath(ctx->udev, syspath);
        if (!dev)
            continue;

        device_info_t info;
        /* Startup enumeration is treated exactly like a fresh "add" event
         * so mount_manager doesn't need a separate code path for it. */
        if (fill_device_info(dev, "add", &info))
            cb(&info, user_data);

        udev_device_unref(dev);
    }

    udev_enumerate_unref(en);
}
