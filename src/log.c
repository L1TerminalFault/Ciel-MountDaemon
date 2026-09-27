/*
 * log.c - see log.h
 */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

#define ANSI_COLOR_RESET   "\x1b[0m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_RED     "\x1b[31m"

static bool g_foreground = false;

void log_init(const char *ident, bool foreground)
{
    g_foreground = foreground;
    /* LOG_PID: prefix messages with our pid, useful when systemd shows
     * multiple instances during restarts.
     * LOG_DAEMON: the standard facility for system daemons. */
    openlog(ident, LOG_PID, LOG_DAEMON);
}

void log_close(void)
{
    closelog();
}

/* Shared implementation for all severities, keeps the four public
 * functions to a one-liner each. */
static void log_vwrite(int priority, const char *color, const char *prefix, const char *fmt, va_list ap)
{
    va_list ap_copy;
    va_copy(ap_copy, ap);

    vsyslog(priority, fmt, ap);

    if (g_foreground) {
        fprintf(stderr, "%s[%s]%s ", color, prefix, ANSI_COLOR_RESET);
        vfprintf(stderr, fmt, ap_copy);
        fprintf(stderr, "\n");
    }

    va_end(ap_copy);
}

void log_debug(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vwrite(LOG_DEBUG, ANSI_COLOR_BLUE, "DEBUG", fmt, ap);
    va_end(ap);
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vwrite(LOG_INFO, ANSI_COLOR_GREEN, "INFO", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vwrite(LOG_WARNING, ANSI_COLOR_YELLOW, "WARN", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vwrite(LOG_ERR, ANSI_COLOR_RED, "ERROR", fmt, ap);
    va_end(ap);
}
