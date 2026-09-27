/*
 * udev_monitor.h - the "detection" half of the daemon.
 *
 * Wraps libudev so the rest of the code never has to touch it directly.
 * Two ways devices reach the callback:
 *
 *   1. udev_enumerate_existing() - a one-shot scan of devices already
 *      present when the daemon starts (so a USB stick inserted before
 *      boot gets mounted too, not just ones plugged in afterwards).
 *
 *   2. udev_monitor_process() - called whenever the monitor's fd (from
 *      udev_monitor_get_fd()) becomes readable, i.e. from your epoll/
 *      select loop. Delivers live add/remove/change events.
 */

#ifndef UDEV_MONITOR_H
#define UDEV_MONITOR_H

#include "device.h"

typedef struct udev_ctx udev_ctx_t;

/* Called once per device, for both enumeration and live events.
 * `info` is only valid for the duration of the call - copy anything you
 * need to keep. */
typedef void (*device_event_cb)(const device_info_t *info, void *user_data);

/* Sets up libudev + a netlink monitor filtered to subsystems we care
 * about (block, and usb for MTP detection). Returns NULL on failure. */
udev_ctx_t *udev_monitor_create(void);
void udev_monitor_destroy(udev_ctx_t *ctx);

/* fd to add to your epoll/select set. Readable = at least one event
 * pending. Do not read() it yourself; call udev_monitor_process().
 *
 * Named udev_monitor_fd() rather than udev_monitor_get_fd() on purpose:
 * libudev itself already exports a function called udev_monitor_get_fd()
 * with a different signature, and the two would collide. */
int udev_monitor_fd(const udev_ctx_t *ctx);

/* Drains all currently-pending events from the netlink socket, invoking
 * cb once per device. Safe to call even if epoll woke you spuriously. */
void udev_monitor_process(udev_ctx_t *ctx, device_event_cb cb, void *user_data);

/* Walks devices already present at call time (subsystem=block). Each is
 * reported to cb with action == DEV_ACTION_ADD, as if it had just been
 * plugged in. Intended to run once at startup. */
void udev_enumerate_existing(udev_ctx_t *ctx, device_event_cb cb, void *user_data);

#endif /* UDEV_MONITOR_H */
