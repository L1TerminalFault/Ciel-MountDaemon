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

/* Syslog identity, shown in `journalctl -t ciel-mountd` */
#define LOG_IDENT "ciel-mountd"

/* How long epoll_wait() blocks before looping again, purely so the daemon
 * can notice g_running was cleared by a signal even with no udev traffic. */
#define EPOLL_TIMEOUT_MS 2000

/* Max number of epoll events processed per wakeup */
#define MAX_EPOLL_EVENTS 16

#endif /* CONFIG_H */
