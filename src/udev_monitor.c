#include "udev_monitor.h"
#include "log.h"
#include "util.h"

#include <libudev.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct udev_ctx {
    struct udev *udev;
    struct udev_monitor *mon;
};

static void copy_prop(struct udev_device *dev, const char *key,
                      char *out, size_t out_len)
{
    const char *val = udev_device_get_property_value(dev, key);
    safe_copy(out, out_len, val ? val : "");
}

/*
 * Reliably detects if a block device is removable:
 * 1. Matches USB, MMC, SD, or CD-ROM bus/drive properties.
 * 2. Checks the sysfs "removable" attribute of the disk itself.
 * 3. If dev is a partition, checks the parent disk's properties.
 */
static bool detect_removable(struct udev_device *dev, const char *id_bus)
{
    if (id_bus && (strcmp(id_bus, "usb") == 0 || strcmp(id_bus, "mmc") == 0))
        return true;

    const char *cdrom = udev_device_get_property_value(dev, "ID_CDROM");
    if (cdrom && strcmp(cdrom, "1") == 0)
        return true;

    const char *flash = udev_device_get_property_value(dev, "ID_DRIVE_FLASH_SD");
    if (flash && strcmp(flash, "1") == 0)
        return true;

    /* Check device's own sysfs "removable" attribute (valid if dev is a disk) */
    const char *removable = udev_device_get_sysattr_value(dev, "removable");
    if (removable && strcmp(removable, "1") == 0)
        return true;

    /* If dev is a partition, check the parent disk device */
    struct udev_device *disk =
        udev_device_get_parent_with_subsystem_devtype(dev, "block", "disk");
    if (disk) {
        removable = udev_device_get_sysattr_value(disk, "removable");
        if (removable && strcmp(removable, "1") == 0)
            return true;

        const char *parent_bus = udev_device_get_property_value(disk, "ID_BUS");
        if (parent_bus && (strcmp(parent_bus, "usb") == 0 || strcmp(parent_bus, "mmc") == 0))
            return true;

        cdrom = udev_device_get_property_value(disk, "ID_CDROM");
        if (cdrom && strcmp(cdrom, "1") == 0)
            return true;
    }

    return false;
}

static void fill_block_info(struct udev_device *dev, device_info_t *info)
{
    const char *devnode = udev_device_get_devnode(dev);
    const char *syspath = udev_device_get_syspath(dev);
    const char *sysname = udev_device_get_sysname(dev);
    const char *devtype = udev_device_get_devtype(dev);

    safe_copy(info->devnode, sizeof(info->devnode), devnode ? devnode : "");
    safe_copy(info->syspath, sizeof(info->syspath), syspath ? syspath : "");
    safe_copy(info->sysname, sizeof(info->sysname), sysname ? sysname : "");

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

    /*
     * If properties were not set on the partition itself, inherit
     * vendor, model, and bus from the parent disk.
     */
    if (info->is_partition) {
        struct udev_device *disk =
            udev_device_get_parent_with_subsystem_devtype(dev, "block", "disk");
        if (disk) {
            if (info->id_vendor[0] == '\0')
                copy_prop(disk, "ID_VENDOR", info->id_vendor, sizeof(info->id_vendor));
            if (info->id_model[0] == '\0')
                copy_prop(disk, "ID_MODEL", info->id_model, sizeof(info->id_model));
            if (info->id_bus[0] == '\0')
                copy_prop(disk, "ID_BUS", info->id_bus, sizeof(info->id_bus));
        }
    }

    info->is_removable = detect_removable(dev, info->id_bus);
}

static void fill_usb_info(struct udev_device *dev, device_info_t *info)
{
    const char *is_mtp = udev_device_get_property_value(dev, "ID_MTP_DEVICE");
    const char *is_media = udev_device_get_property_value(dev, "ID_MEDIA_PLAYER");

    if ((!is_mtp || strcmp(is_mtp, "1") != 0) &&
        (!is_media || strcmp(is_media, "1") != 0)) {
        info->class = DEV_CLASS_UNKNOWN;
        return;
    }

    info->class = DEV_CLASS_MTP;

    const char *devnode = udev_device_get_devnode(dev);
    const char *syspath = udev_device_get_syspath(dev);
    const char *sysname = udev_device_get_sysname(dev);
    safe_copy(info->devnode, sizeof(info->devnode), devnode ? devnode : "");
    safe_copy(info->syspath, sizeof(info->syspath), syspath ? syspath : "");
    safe_copy(info->sysname, sizeof(info->sysname), sysname ? sysname : "");

    copy_prop(dev, "ID_VENDOR", info->id_vendor, sizeof(info->id_vendor));
    copy_prop(dev, "ID_MODEL",  info->id_model,  sizeof(info->id_model));
    copy_prop(dev, "ID_SERIAL", info->id_serial, sizeof(info->id_serial));
    safe_copy(info->id_bus, sizeof(info->id_bus), "usb");
    info->is_removable = true;

    const char *busnum = udev_device_get_sysattr_value(dev, "busnum");
    const char *devnum = udev_device_get_sysattr_value(dev, "devnum");
    info->usb_busnum = busnum ? atoi(busnum) : -1;
    info->usb_devnum = devnum ? atoi(devnum) : -1;
}

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
            return false;
        fill_block_info(dev, info);
        return true;
    }

    if (strcmp(subsystem, "usb") == 0) {
        fill_usb_info(dev, info);
        return info->class == DEV_CLASS_MTP;
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

    /*
     * Make the netlink socket strictly non-blocking.
     * Prevents udev_monitor_receive_device() from freezing inside recvmsg().
     */
    int fd = udev_monitor_get_fd(ctx->mon);
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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
    if (!ctx || !ctx->mon)
        return -1;
    return udev_monitor_get_fd(ctx->mon);
}

void udev_monitor_process(udev_ctx_t *ctx, device_event_cb cb, void *user_data)
{
    if (!ctx || !ctx->mon || !cb)
        return;

    struct udev_device *dev;
    while ((dev = udev_monitor_receive_device(ctx->mon)) != NULL) {
        const char *action = udev_device_get_action(dev);

        device_info_t info;
        if (fill_device_info(dev, action, &info))
            cb(&info, user_data);

        udev_device_unref(dev);
    }
}

static void enumerate_subsystem(struct udev *udev, const char *subsystem,
                                const char *devtype, device_event_cb cb,
                                void *user_data)
{
    struct udev_enumerate *en = udev_enumerate_new(udev);
    if (!en)
        return;

    udev_enumerate_add_match_subsystem(en, subsystem);
    if (devtype)
        udev_enumerate_add_match_property(en, "DEVTYPE", devtype);

    udev_enumerate_scan_devices(en);

    struct udev_list_entry *entry;
    struct udev_list_entry *devices = udev_enumerate_get_list_entry(en);

    udev_list_entry_foreach(entry, devices) {
        const char *syspath = udev_list_entry_get_name(entry);
        struct udev_device *dev = udev_device_new_from_syspath(udev, syspath);
        if (!dev)
            continue;

        device_info_t info;
        if (fill_device_info(dev, "add", &info))
            cb(&info, user_data);

        udev_device_unref(dev);
    }

    udev_enumerate_unref(en);
}

void udev_enumerate_existing(udev_ctx_t *ctx, device_event_cb cb, void *user_data)
{
    if (!ctx || !ctx->udev || !cb)
        return;

    /* 1. Enumerate Block Devices */
    enumerate_subsystem(ctx->udev, "block", NULL, cb, user_data);

    /* 2. Enumerate Connected MTP Devices */
    enumerate_subsystem(ctx->udev, "usb", "usb_device", cb, user_data);
}
