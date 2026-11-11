/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIR_SCREEN_H
#define AIR_SCREEN_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Internal OLED state is distinct from a connected/powered DP output. Unknown
 * must not enable automatic entry. Parse bounded, CRC-checked MCU packets. */
static inline bool ascreen_valid_packet(const uint8_t *p, size_t n) {
    if (n < 23 || p[0] != 0xfd)
        return false;
    size_t len = p[5] | (size_t)p[6] << 8;
    if (len < 18 || len > 59 || len + 5 > n)
        return false;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 5; i < len + 5; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    uint32_t supplied = (uint32_t)p[1] | (uint32_t)p[2] << 8 |
                        (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
    if (~crc != supplied)
        return false;
    return true;
}
static inline int ascreen_packet(const uint8_t *p, size_t n) {
    if (!ascreen_valid_packet(p,n)) return -1;
    size_t len = p[5] | (size_t)p[6] << 8;
    unsigned id = p[15] | (unsigned)p[16] << 8;
    if (id == 0x6c04 && p[22] <= 1)
        return p[22];
    if (id == 0x6c05 && len >= 26 && p[26] == 1 && p[30] <= 1)
        return p[30];
    /* Original Air firmware reports opening only through this log, while
     * closing also emits 6c04. Verified with passive USB observation. */
    if (id == 0x6c09) {
        size_t text = len - 17;
        if (text >= 9 && !memcmp(p + 22, "Open OLED", 9))
            return 1;
        if (text >= 10 && !memcmp(p + 22, "Close OLED", 10))
            return 0;
    }
    return -1;
}
/* Virtual display-toggle reports track wear. Firmware OLED logs also occur
 * during mode changes, so they must not override an established wear signal. */
static inline int ascreen_wear_packet(const uint8_t *p, size_t n) {
    if (!ascreen_valid_packet(p,n)) return -1;
    size_t len=p[5] | (size_t)p[6]<<8;
    unsigned id=p[15] | (unsigned)p[16]<<8;
    return id==0x6c05 && len>=26 && p[26]==1 && p[30]<=1 ? p[30] : -1;
}
static inline int ascreen_fresh(long owner, uint64_t stamp, int state,
                                long expected_owner, uint64_t now) {
    if (owner <= 0 || owner != expected_owner || stamp > now ||
        now - stamp > 3000000000ULL || (state != 0 && state != 1))
        return -1;
    return state;
}
/* Read only state from this session's tracker owner, with a recent live-reader
 * heartbeat. A previous process/release cannot enable automatic mode. */
static inline int ascreen_read_state(const char *ready, const char *suffix, uint64_t now) {
    if (!ready || ready[0] != '/')
        return -1;
    FILE *f = fopen(ready, "r");
    if (!f)
        return -1;
    char json[1024];
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = 0;
    char *key = strstr(json, "\"pid\"");
    char *colon = key ? strchr(key + 5, ':') : NULL;
    long expected = colon ? strtol(colon + 1, NULL, 10) : 0;
    char path[4096];
    int len = snprintf(path, sizeof(path), "%s%s", ready, suffix);
    if (len < 0 || (size_t)len >= sizeof(path))
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;
    long owner;
    unsigned long long stamp;
    int state;
    char extra;
    int fields = fscanf(f, "%ld %llu %d %c", &owner, &stamp, &state, &extra);
    fclose(f);
    return fields == 3 ? ascreen_fresh(owner, stamp, state, expected, now) : -1;
}

static inline int ascreen_read(const char *ready, uint64_t now) {
    return ascreen_read_state(ready, ".screen", now);
}
#endif
