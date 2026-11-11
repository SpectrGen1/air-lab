/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIR_SCREEN_PUBLISH_H
#define AIR_SCREEN_PUBLISH_H
#include "air-screen.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

/* Runs on the existing MCU reader, including replies consumed by synchronous
 * display queries. Never open another HID handle or change device settings. */
static void air_screen_observe(const void *packet, size_t length) {
    static int state = -1;
    static bool wear_known;
    static uint64_t published;
    const char *ready = getenv("AIR_TRACKER_READY_FILE");
    if (!ready || ready[0] != '/')
        return;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t now = (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
    int wear = ascreen_wear_packet(packet, length);
    int event;
    if (wear >= 0) {
        wear_known = true;
        event = wear;
    } else {
        event = wear_known ? -1 : ascreen_packet(packet, length);
    }
    int previous = state;
    if (event >= 0)
        state = event;
    if (state == previous && now - published < 250000000)
        return;
    char path[4096], temp[4096];
    int a = snprintf(path, sizeof(path), "%s.screen", ready);
    int b = snprintf(temp, sizeof(temp), "%s.screen.%ld.tmp", ready, (long)getpid());
    if (a < 0 || (size_t)a >= sizeof(path) || b < 0 || (size_t)b >= sizeof(temp))
        return;
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(temp); return; }
    int written = fprintf(f, "%ld %llu %d\n", (long)getpid(), (unsigned long long)now, state);
    int closed = fclose(f);
    if (written > 0 && !closed && !rename(temp, path))
        published = now;
    else
        unlink(temp);
}
#endif
