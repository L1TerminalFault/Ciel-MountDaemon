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
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if (pid > 0)
        exit(EXIT_SUCCESS);

    if (setsid() < 0)
    {
        perror("setsid");
        exit(EXIT_FAILURE);
    }

    pid = fork();
    if (pid < 0)
    {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* Reset umask so daemon-created mountpoints are accessible */
    umask(0022);

    if (chdir("/") != 0)
    {
        perror("chdir");
        exit(EXIT_FAILURE);
    }

    if (!freopen("/dev/null", "r", stdin))
        log_warn("stdin redirect failed");
    if (!freopen("/dev/null", "w", stdout))
        log_warn("stdout redirect failed");
    if (!freopen("/dev/null", "w", stderr))
        log_warn("stderr redirect failed");
}

static void ensure_base_dir(void)
{
    struct stat st;
    if (stat(MOUNT_BASE_DIR, &st) == 0)
    {
        if (!S_ISDIR(st.st_mode))
            log_error("Mount base path %s exists but is not a directory!",
                      MOUNT_BASE_DIR);
        return;
    }
    if (mkdir(MOUNT_BASE_DIR, 0755) != 0 && errno != EEXIST)
    {
        log_warn("mkdir(%s) failed: %s", MOUNT_BASE_DIR, strerror(errno));
    }
}

static void on_device_event(const device_info_t *info, void *user_data)
{
    (void)user_data;
    switch (info->action)
    {
    case DEV_ACTION_ADD:
        mount_manager_handle_add(info);
        break;
    case DEV_ACTION_REMOVE:
        mount_manager_handle_remove(info);
        break;
    case DEV_ACTION_CHANGE:
        log_debug("change event for %s (ignored)", info->devnode);
        break;
    default:
        break;
    }
}

static void shutdown_daemon(udev_ctx_t *udev_ctx)
{
    log_info("ciel-mountd stopping...");
    dbus_interface_shutdown();
    mount_manager_shutdown();
    if (udev_ctx)
        udev_monitor_destroy(udev_ctx);
    log_info("ciel-mountd stopped");
    log_close();
}

int main(int argc, char **argv)
{
    bool foreground = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--foreground") == 0)
        {
            foreground = true;
        }
        else
        {
            fprintf(stderr, "Usage: %s [-f|--foreground]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    log_init(LOG_IDENT, foreground);

    if (!foreground)
        daemonize();

    /* Ignore SIGPIPE so broken sockets don't kill the daemon */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    /* Set up signalfd for race-free termination */
    sigset_t sigmask;
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGTERM);
    sigaddset(&sigmask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0)
    {
        log_error("sigprocmask failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    int sig_fd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd < 0)
    {
        log_error("signalfd failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    ensure_base_dir();

    udev_ctx_t *udev_ctx = udev_monitor_create();
    if (!udev_ctx)
    {
        log_error("Failed to initialize udev monitoring");
        close(sig_fd);
        log_close();
        return EXIT_FAILURE;
    }

    mount_manager_init();

    if (!dbus_interface_init())
        log_warn("Failed to initialize DBus interface, continuing without IPC");

    log_info("Scanning for existing media...");
    udev_enumerate_existing(udev_ctx, on_device_event, NULL);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
    {
        log_error("epoll_create1() failed: %s", strerror(errno));
        close(sig_fd);
        shutdown_daemon(udev_ctx);
        return EXIT_FAILURE;
    }

    /* 1. Watch Signals */
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = sig_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sig_fd, &ev);

    /* 2. Watch udev events */
    int udev_fd = udev_monitor_fd(udev_ctx);
    ev.events = EPOLLIN;
    ev.data.fd = udev_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, udev_fd, &ev) < 0)
    {
        log_error("epoll_ctl(udev) failed: %s", strerror(errno));
        close(sig_fd);
        close(epfd);
        shutdown_daemon(udev_ctx);
        return EXIT_FAILURE;
    }

    log_info("ciel-mountd running");

    bool running = true;
    struct epoll_event events[MAX_EPOLL_EVENTS];

    while (running)
    {
        int n = epoll_wait(epfd, events, MAX_EPOLL_EVENTS, 100);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;

            log_error("epoll_wait failed: %s", strerror(errno));
            break;
        }

        /*
         * Give GLib/GDBus a chance to process incoming D-Bus
         * messages and send pending replies.
         */
        dbus_interface_tick();

        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;

            if (fd == sig_fd)
            {
                struct signalfd_siginfo sinfo;
                if (read(sig_fd, &sinfo, sizeof(sinfo)) > 0)
                {
                    log_info("Received signal %d, exiting...", sinfo.ssi_signo);
                    running = false;
                    break;
                }
            }
            else if (fd == udev_fd)
            {
                udev_monitor_process(udev_ctx, on_device_event, NULL);
            }
        }
    }

    close(sig_fd);
    close(epfd);
    shutdown_daemon(udev_ctx);
    return EXIT_SUCCESS;
}
