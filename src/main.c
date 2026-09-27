/*
 * main.c - ciel-mountd entry point.
 *
 * Flow:
 *   1. Parse args, init logging.
 *   2. (unless -f) daemonize: fork, detach from the controlling
 *      terminal, become session leader.
 *   3. Set up udev monitoring and the mount manager.
 *   4. Mount whatever removable media is already present.
 *   5. Enter the epoll loop: block until udev has an event, dispatch it,
 *      repeat until SIGTERM/SIGINT.
 *   6. On exit, unmount everything we mounted (mount_manager_shutdown)
 *      so stopping the daemon doesn't leave orphaned mounts.
 */

#include "config.h"
#include "dbus_interface.h"
#include "device.h"
#include "log.h"
#include "mount_manager.h"
#include "udev_monitor.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <unistd.h>

/* Cleared by the signal handler; the main loop checks it once per
 * epoll_wait() wakeup (or every EPOLL_TIMEOUT_MS if idle) and exits its
 * loop when false. Kept as sig_atomic_t because it's touched from a
 * signal handler - see `man 7 signal-safety`. */
static volatile sig_atomic_t g_running = 1;

static void handle_signal(int signum) {
  (void)signum;
  g_running = 0;
}

/* Classic double-fork daemonization. Only used without -f; under
 * systemd (Type=simple) you want -f instead, since systemd already does
 * process supervision and expects the process it started to be the one
 * that keeps running - see systemd/ciel-mountd.service. */
static void daemonize(void) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    exit(EXIT_FAILURE);
  }
  if (pid > 0)
    exit(EXIT_SUCCESS); /* parent exits, first child carries on */

  if (setsid() < 0) {
    perror("setsid");
    exit(EXIT_FAILURE);
  }

  /* Second fork so we can never re-acquire a controlling terminal. */
  pid = fork();
  if (pid < 0) {
    perror("fork");
    exit(EXIT_FAILURE);
  }
  if (pid > 0)
    exit(EXIT_SUCCESS);

  if (chdir("/") != 0) {
    perror("chdir");
    exit(EXIT_FAILURE);
  }

  /* Detach stdio. We've already called log_init() by this point, so
   * all real output goes to syslog; this just stops a stray printf
   * somewhere from writing to whatever terminal happened to start us,
   * or from blocking if that terminal's pipe fills up. */
  if (!freopen("/dev/null", "r", stdin))
    log_warn("could not redirect stdin to /dev/null");
  if (!freopen("/dev/null", "w", stdout))
    log_warn("could not redirect stdout to /dev/null");
  if (!freopen("/dev/null", "w", stderr))
    log_warn("could not redirect stderr to /dev/null");
}

/* Ensures MOUNT_BASE_DIR (e.g. /media) exists before we try to create
 * subdirectories under it. */
static void ensure_base_dir(void) {
  if (mkdir(MOUNT_BASE_DIR, 0755) != 0 && errno != EEXIST)
    log_warn("mkdir(%s) failed: %s", MOUNT_BASE_DIR, strerror(errno));
}

/* The single callback wired to both udev_enumerate_existing() (startup
 * scan) and udev_monitor_process() (live events) - dispatches purely on
 * info->action, so mount_manager doesn't care whether a device was
 * already plugged in at boot or just appeared. */
static void on_device_event(const device_info_t *info, void *user_data) {
  (void)user_data;

  switch (info->action) {
  case DEV_ACTION_ADD:
    mount_manager_handle_add(info);
    break;
  case DEV_ACTION_REMOVE:
    mount_manager_handle_remove(info);
    break;
  case DEV_ACTION_CHANGE:
    /* A "change" event on a block device commonly means its
     * partition table was (re-)read - e.g. right after a device
     * first appears, or after `partprobe`. We don't act on it
     * directly: the kernel/udev will emit separate "add" events for
     * any partitions that result. Logged at debug level only so it
     * doesn't clutter normal operation. */
    log_debug("change event for %s (ignored)", info->devnode);
    break;
  default:
    break;
  }
}

static void print_usage(const char *argv0) {
  fprintf(stderr,
          "Usage: %s [-f|--foreground] [-h|--help]\n"
          "  -f, --foreground   Stay in the foreground, log to stderr too\n"
          "                     (this is what the systemd unit uses)\n"
          "  -h, --help         Show this help\n",
          argv0);
}

static void shutdown(udev_ctx_t *udev_ctx) {
  log_info("ciel-mountd shutting down, unmounting tracked volumes...");
  mount_manager_shutdown();
  dbus_interface_shutdown();
  udev_monitor_destroy(udev_ctx);
  log_info("ciel-mountd stopped");
  log_close();
}

int main(int argc, char **argv) {
  bool foreground = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--foreground") == 0) {
      foreground = true;
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return EXIT_SUCCESS;
    } else {
      fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      print_usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  log_init(LOG_IDENT, foreground);

  if (!foreground)
    daemonize();

  /* Signals are handled the same whether daemonized or not: request a
   * clean shutdown (which unmounts everything) rather than dying
   * immediately, so SIGTERM from systemd on `stop`/restart is safe. */
  struct sigaction sa = {0};
  sa.sa_handler = handle_signal;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  /* We don't fork children, but be defensive anyway. */
  signal(SIGCHLD, SIG_IGN);

  ensure_base_dir();

  udev_ctx_t *udev_ctx = udev_monitor_create();
  if (!udev_ctx) {
    log_error("failed to initialize udev monitoring, exiting");
    log_close();
    return EXIT_FAILURE;
  }

  mount_manager_init();

  if (!dbus_interface_init()) {
    log_warn("failed to initialize DBus interface, ignoring DBus");
    // shutdown(udev_ctx);
    // return EXIT_FAILURE;
  }

  log_info("ciel-mountd starting up, scanning for already-present media...");
  udev_enumerate_existing(udev_ctx, on_device_event, NULL);

  int epfd = epoll_create1(0);
  if (epfd < 0) {
    log_error("epoll_create1() failed: %s", strerror(errno));
    shutdown(udev_ctx);
  }

  struct epoll_event ev = {0};
  ev.events = EPOLLIN;
  ev.data.fd = udev_monitor_fd(udev_ctx);
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, ev.data.fd, &ev) != 0) {
    log_error("epoll_ctl() failed: %s", strerror(errno));
    close(epfd);
    shutdown(udev_ctx);
  }

  int dbus_fd = dbus_interface_get_fd();
  if (dbus_fd >= 0) {
    // We reuse 'ev' by overwriting its inner target fd!
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET; /* or without ET if you prefer */
    ev.data.fd = dbus_fd;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, dbus_fd, &ev) != 0) {
      log_error("epoll_ctl(dbus) failed: %s", strerror(errno));
      close(epfd);
      shutdown(udev_ctx);
      return EXIT_FAILURE;
    }
  }

  log_info("ciel-mountd ready, watching for device events");

  struct epoll_event events[MAX_EPOLL_EVENTS];
  while (g_running) {
    int n = epoll_wait(epfd, events, MAX_EPOLL_EVENTS, EPOLL_TIMEOUT_MS);
    if (n < 0) {
      if (errno == EINTR)
        continue; /* a signal arrived; loop re-checks g_running */
      log_error("epoll_wait() failed: %s", strerror(errno));
      break;
    }
    for (int i = 0; i < n; i++) {
      if (events[i].data.fd == udev_monitor_fd(udev_ctx)) {
        udev_monitor_process(udev_ctx, on_device_event, NULL);
      } else if (events[i].data.fd == dbus_fd) {
        dbus_interface_tick();
      }
    }
  }

  close(epfd);

  shutdown(udev_ctx);
  return EXIT_SUCCESS;
}
