/*
 * device.h - the common "device event" record passed around the daemon.
 *
 * udev_monitor.c is the only place that knows about libudev types
 * (struct udev, struct udev_device, ...). Everywhere else in the codebase
 * only sees this plain struct, so mount_manager.c and main.c don't need to
 * link against libudev's internals or understand its API at all. This
 * keeps the "detect" and "act" halves of the daemon decoupled, which
 * matters a lot if you later want to swap MTP detection onto a different
 * mechanism, or unit-test mount_manager.c with fake devices.
 */

#ifndef DEVICE_H
#define DEVICE_H

#include <stdbool.h>

#define MAX_PROP_LEN 256

/* What kind of udev event this is. */
typedef enum {
    DEV_ACTION_ADD,
    DEV_ACTION_REMOVE,
    DEV_ACTION_CHANGE,
    DEV_ACTION_UNKNOWN
} dev_action_t;

/* Coarse classification used to decide *how* to handle the device.
 * See classify_device() in udev_monitor.c for the detection rules. */
typedef enum {
    DEV_CLASS_BLOCK_PARTITION, /* a mountable partition, e.g. /dev/sdb1  */
    DEV_CLASS_BLOCK_DISK,      /* a whole disk with no partition table,
                                  e.g. a USB stick formatted without
                                  partitions (superfloppy layout)        */
    DEV_CLASS_MTP,             /* Android phone / camera in MTP mode.
                                  NOT a block device - see README for why
                                  this is currently detect-only.         */
    DEV_CLASS_UNKNOWN
} dev_class_t;

typedef struct device_info {
    dev_action_t action;
    dev_class_t  class;

    char devnode[MAX_PROP_LEN];  /* e.g. /dev/sdb1                       */
    char syspath[MAX_PROP_LEN];  /* e.g. /sys/devices/.../block/sdb/sdb1 */
    char sysname[MAX_PROP_LEN];  /* e.g. sdb1                            */

    char id_bus[MAX_PROP_LEN];      /* usb, ata, mmc, ...                */
    char id_fs_type[MAX_PROP_LEN];  /* vfat, ext4, ntfs, exfat, ...      */
    char id_fs_label[MAX_PROP_LEN]; /* volume label, may be empty        */
    char id_fs_uuid[MAX_PROP_LEN];  /* filesystem UUID, may be empty     */
    char id_vendor[MAX_PROP_LEN];   /* e.g. SanDisk                      */
    char id_model[MAX_PROP_LEN];    /* e.g. Cruzer_Blade                 */
    char id_serial[MAX_PROP_LEN];   /* used to build a unique mountpoint
                                        name when label/uuid are absent  */

    /* USB bus/device address (from the "busnum"/"devnum" sysfs
     * attributes). Only meaningful for class == DEV_CLASS_MTP - it's how
     * mtp_fs.c picks the exact physical device out of libmtp's device
     * list when more than one MTP device is plugged in at once. */
    int usb_busnum;
    int usb_devnum;

    bool is_removable; /* true if the underlying disk's "removable"
                           sysfs attribute is 1, or the bus is usb/mmc  */
    bool is_partition; /* devtype == "partition" vs "disk"              */
} device_info_t;

#endif /* DEVICE_H */
