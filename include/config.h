/*
 * config.h - Compile-time configuration for ciel-mountd
 *
 * These are the "knobs" you'll most likely want to tweak first.
 * Nothing here requires touching the rest of the codebase.
 */

#ifndef CONFIG_H
#define CONFIG_H

/* Base directory under which removable volumes get their own mount-point
 * subdirectory, e.g. /media/MY_USB_LABEL
 *
 * udisks2 traditionally uses /run/media/<user>/<label>, which is
 * per-user-session aware. We use plain /media/<label> for now because we
 * don't yet talk to logind to figure out "which user is at the seat".
 * See README.md, section "Known limitations", for how to extend this. */
#define MOUNT_BASE_DIR "/media"

/* Every filesystem we mount gets ownership mapped to this uid/gid via
 * mount options (for fs types that support uid=/gid=, e.g. vfat/exfat/ntfs3).
 * 1000 is the conventional first non-root user on most distros.
 *
 * TODO: this is a placeholder. A "real" replacement for udisks2 should
 * determine the uid of the user logged in at the active seat (via
 * sd-login.h / logind) instead of hardcoding it. */
// #define MOUNT_UID 1000
// #define MOUNT_GID 1000

/* Syslog identity, shown in `journalctl -t ciel-mountd` */
#define LOG_IDENT "ciel-mountd"

/* How long epoll_wait() blocks before looping again, purely so the daemon
 * can notice g_running was cleared by a signal even with no udev traffic. */
#define EPOLL_TIMEOUT_MS 2000

/* Max number of epoll events processed per wakeup */
#define MAX_EPOLL_EVENTS 8

#endif /* CONFIG_H */
