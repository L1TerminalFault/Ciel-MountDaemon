/*
 * log.h - minimal logging wrapper.
 *
 * Goes to syslog/journal always, and additionally to stderr when the
 * daemon is run with -f/--foreground (handy while developing, since you
 * see output immediately instead of tailing the journal).
 */

#ifndef LOG_H
#define LOG_H

#include <stdbool.h>

void log_init(const char *ident, bool foreground);
void log_close(void);

void log_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_info(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
void log_warn(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
void log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* LOG_H */
