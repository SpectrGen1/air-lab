/* Low-rate diagnostic events shared by the compositor and its presenter. */
#ifndef AIR_EVENT_H
#define AIR_EVENT_H

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static inline const char *aevent_bool(bool value) {
    return value ? "true" : "false";
}

/* Escape strings into a complete JSON value, even if the source is too long. */
static inline void aevent_quote(char *out, size_t capacity, const char *input) {
    size_t used = 0;
    out[used++] = '"';
    for (const unsigned char *p = (const unsigned char *)(input ? input : "");
         *p && used + 8 < capacity; p++) {
        if (*p == '"' || *p == '\\') {
            out[used++] = '\\';
            out[used++] = *p;
        } else if (*p < 32 || *p >= 127) {
            used += snprintf(out + used, capacity - used, "\\u%04x", (unsigned)*p);
        } else {
            out[used++] = *p;
        }
    }
    out[used++] = '"';
    out[used] = 0;
}

static inline void __attribute__((format(printf, 3, 4)))
aevent_write(const char *component, const char *event, const char *format, ...) {
    const char *path = getenv("AIR_SPATIAL_EVENT_LOG");
    if (!path || path[0] != '/')
        return;
    int previous_errno = errno;
    char record[2048], timestamp[32];
    struct timespec stamp;
    struct tm utc;
    if (clock_gettime(CLOCK_REALTIME, &stamp) < 0 || !gmtime_r(&stamp.tv_sec, &utc) ||
        !strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &utc))
        goto done;
    int used = snprintf(record, sizeof(record),
        "{\"ts\":\"%s.%03ldZ\",\"component\":\"%s\",\"event\":\"%s\",\"pid\":%ld",
        timestamp, stamp.tv_nsec / 1000000, component, event, (long)getpid());
    if (used < 0 || used >= (int)sizeof(record) - 2)
        goto done;
    va_list args;
    va_start(args, format);
    int size = vsnprintf(record + used, sizeof(record) - used, format, args);
    va_end(args);
    if (size < 0 || size >= (int)sizeof(record) - used - 2)
        goto done;
    used += size;
    record[used++] = '}';
    record[used++] = '\n';

    /* One append per record keeps compositor, presenter, and supervisor lines intact. */
    int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0) {
        ssize_t count;
        do {
            count = write(fd, record, used);
        } while (count < 0 && errno == EINTR);
        close(fd);
    }
done:
    errno = previous_errno;
}

#endif
