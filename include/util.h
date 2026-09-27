/*
 * util.h - tiny shared helpers.
 */

#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stddef.h>

/* snprintf-based "safe strcpy": always NUL-terminates dst and silently
 * truncates rather than ever overflowing. Used everywhere we copy a
 * udev-provided string (whose length we don't control) into one of our
 * fixed-size buffers, instead of strncpy - which doesn't guarantee NUL
 * termination and trips GCC's -Wstringop-truncation for no benefit. */
static inline void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0)
        return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

#endif /* UTIL_H */
