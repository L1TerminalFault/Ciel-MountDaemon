/*
 * mtp_fs.h - MTP (Media Transfer Protocol) mounting via libmtp + FUSE.
 *
 * Unlike block devices, an MTP device (phone, camera, ...) has no
 * filesystem the kernel understands - mount(2) can't touch it. Instead
 * we speak MTP ourselves via libmtp and expose the result as a FUSE
 * filesystem.
 *
 * This module is designed to run inside its own process: mtp_fs_run()
 * opens the device, calls fuse_main() (which blocks, servicing
 * filesystem calls until unmounted), and returns only once that's
 * done. mount_manager.c forks a child specifically to call this, so a
 * misbehaving or slow MTP device can never block the main daemon's
 * event loop, and each mounted phone gets an isolated process (a crash
 * handling one phone can't take down the whole daemon or other mounts).
 */

#ifndef MTP_FS_H
#define MTP_FS_H

#include "device.h"

/* Opens the MTP device identified by info->usb_busnum/usb_devnum, mounts
 * it at `mountpoint` via FUSE, and blocks servicing filesystem requests
 * until the mount is torn down (normally by this process receiving
 * SIGTERM - see mount_manager.c - which FUSE's own default signal
 * handling turns into a clean unmount).
 *
 * Meant to be called from a freshly-forked child that does nothing else;
 * returns an exit-code-suitable int (0 success) once the mount ends. */
int mtp_fs_run(const device_info_t *info, const char *mountpoint);

#endif /* MTP_FS_H */
