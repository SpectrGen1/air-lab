/* Disposable nested-session input test tool. Never part of the installed desktop. */
#define _GNU_SOURCE
#include "virtual-keyboard-unstable-v1-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-protocol.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
static struct zwlr_virtual_pointer_manager_v1 *pm;
static struct zwp_virtual_keyboard_manager_v1 *km;
static struct wl_seat *seat;
static void global(void *d, struct wl_registry *r, uint32_t id, const char *name,
                   uint32_t version) {
    (void)d;
    (void)version;
    if (!strcmp(name, "zwlr_virtual_pointer_manager_v1"))
        pm = wl_registry_bind(r, id, &zwlr_virtual_pointer_manager_v1_interface, 2);
    if (!strcmp(name, "zwp_virtual_keyboard_manager_v1"))
        km = wl_registry_bind(r, id, &zwp_virtual_keyboard_manager_v1_interface, 1);
    if (!strcmp(name, "wl_seat"))
        seat = wl_registry_bind(r, id, &wl_seat_interface, 1);
}
static void remove_global(void *d, struct wl_registry *r, uint32_t id) {
    (void)d;
    (void)r;
    (void)id;
}
static const struct wl_registry_listener listener = {.global = global,
                                                     .global_remove = remove_global};
int main(int argc, char **argv) {
    if (!getenv("AIR_TEST_CONFIRM_DISPOSABLE") ||
        strcmp(getenv("AIR_TEST_CONFIRM_DISPOSABLE"), "1")) {
        fprintf(stderr,
                "Only use with AIR_TEST_CONFIRM_DISPOSABLE=1 on a disposable nested socket.\n");
        return 2;
    }
    struct wl_display *display = wl_display_connect(NULL);
    if (!display)
        return 1;
    struct wl_registry *r = wl_display_get_registry(display);
    wl_registry_add_listener(r, &listener, NULL);
    wl_display_roundtrip(display);
    if (!pm || !km || !seat)
        return 1;
    struct zwlr_virtual_pointer_v1 *p =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pm, seat);
    struct zwp_virtual_keyboard_v1 *k =
        zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(km, seat);
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = {.layout = "us"};
    struct xkb_keymap *map = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char *text = xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1);
    int fd = memfd_create("test-keymap", 0);
    size_t len = strlen(text) + 1;
    ftruncate(fd, len);
    write(fd, text, len);
    zwp_virtual_keyboard_v1_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, len);
    close(fd);
    free(text);
    xkb_keymap_unref(map);
    xkb_context_unref(ctx);
    wl_display_roundtrip(display);
    struct timespec clock;
    clock_gettime(CLOCK_MONOTONIC, &clock);
    uint32_t t = (uint32_t)((uint64_t)clock.tv_sec * 1000 + clock.tv_nsec / 1000000);
    for (int i = 1; i < argc;) {
        if (!strcmp(argv[i], "motion") && i + 2 < argc) {
            double dx = atof(argv[i + 1]), dy = atof(argv[i + 2]);
            zwlr_virtual_pointer_v1_motion(p, t, wl_fixed_from_double(dx),
                                           wl_fixed_from_double(dy));
            zwlr_virtual_pointer_v1_frame(p);
            i += 3;
        } else if (!strcmp(argv[i], "axis") && i + 1 < argc) {
            zwlr_virtual_pointer_v1_axis_source(p, WL_POINTER_AXIS_SOURCE_WHEEL);
            zwlr_virtual_pointer_v1_axis(p, t, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                         wl_fixed_from_double(atof(argv[i + 1])));
            zwlr_virtual_pointer_v1_frame(p);
            i += 2;
        } else if (!strcmp(argv[i], "button") && i + 2 < argc) {
            zwlr_virtual_pointer_v1_button(p, t, atoi(argv[i + 1]), atoi(argv[i + 2]));
            zwlr_virtual_pointer_v1_frame(p);
            i += 3;
        } else if (!strcmp(argv[i], "key") && i + 1 < argc) {
            uint32_t key = atoi(argv[i + 1]);
            zwp_virtual_keyboard_v1_key(k, t, key, 1);
            zwp_virtual_keyboard_v1_key(k, t + 1, key, 0);
            i += 2;
        } else if (!strcmp(argv[i], "chord") && i + 1 < argc) {
            uint32_t key = atoi(argv[i + 1]);
            zwp_virtual_keyboard_v1_modifiers(k, (1 << 3) | (1 << 6), 0, 0, 0);
            zwp_virtual_keyboard_v1_key(k, t, key, 1);
            zwp_virtual_keyboard_v1_key(k, t + 1, key, 0);
            zwp_virtual_keyboard_v1_modifiers(k, 0, 0, 0, 0);
            i += 2;
        } else if (!strcmp(argv[i], "wait") && i + 1 < argc) {
            wl_display_roundtrip(display);
            usleep(atoi(argv[i + 1]) * 1000);
            i += 2;
        } else
            return 2;
        clock_gettime(CLOCK_MONOTONIC, &clock);
        uint32_t current = (uint32_t)((uint64_t)clock.tv_sec * 1000 + clock.tv_nsec / 1000000);
        t = (int32_t)(current - t) > 0 ? current : t + 1;
        wl_display_roundtrip(display);
    }
    wl_display_disconnect(display);
    return 0;
}
