/* Synthetic lock/unlock fixture: disposable test sockets only, no authentication. */
#define _GNU_SOURCE
#include "ext-session-lock-v1-protocol.h"
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct ext_session_lock_manager_v1 *manager;
static struct wl_output *outputs[8];
static int noutputs;
static int locked;
static void pointer_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                          wl_fixed_t x, wl_fixed_t y) {
    (void)d;
    (void)s;
    (void)x;
    (void)y;
    wl_pointer_set_cursor(p, serial, NULL, 0, 0);
}
static void pointer_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
    (void)d;
    (void)p;
    (void)serial;
    (void)s;
}
static void pointer_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d;
    (void)p;
    (void)t;
    (void)x;
    (void)y;
}
static void pointer_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b,
                           uint32_t state) {
    (void)d;
    (void)p;
    (void)s;
    (void)t;
    (void)b;
    (void)state;
}
static void pointer_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis,
                         wl_fixed_t value) {
    (void)d;
    (void)p;
    (void)t;
    (void)axis;
    (void)value;
}
static const struct wl_pointer_listener pointer_listener = {.enter = pointer_enter,
                                                            .leave = pointer_leave,
                                                            .motion = pointer_motion,
                                                            .button = pointer_button,
                                                            .axis = pointer_axis};
static void global(void *d, struct wl_registry *r, uint32_t id, const char *name, uint32_t v) {
    (void)d;
    (void)v;
    if (!strcmp(name, "wl_compositor"))
        compositor = wl_registry_bind(r, id, &wl_compositor_interface, 4);
    else if (!strcmp(name, "wl_shm"))
        shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    else if (!strcmp(name, "ext_session_lock_manager_v1"))
        manager = wl_registry_bind(r, id, &ext_session_lock_manager_v1_interface, 1);
    else if (!strcmp(name, "wl_seat")) {
        struct wl_seat *seat = wl_registry_bind(r, id, &wl_seat_interface, 1);
        struct wl_pointer *pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    } else if (!strcmp(name, "wl_output") && noutputs < 8)
        outputs[noutputs++] = wl_registry_bind(r, id, &wl_output_interface, 1);
}
static void removed(void *d, struct wl_registry *r, uint32_t id) {
    (void)d;
    (void)r;
    (void)id;
}
static const struct wl_registry_listener registry_listener = {.global = global,
                                                              .global_remove = removed};
static void lock_locked(void *d, struct ext_session_lock_v1 *l) {
    (void)d;
    (void)l;
    locked = 1;
    puts("LOCKED");
    fflush(stdout);
}
static void lock_finished(void *d, struct ext_session_lock_v1 *l) {
    (void)d;
    (void)l;
    locked = -1;
}
static const struct ext_session_lock_v1_listener lock_listener = {.locked = lock_locked,
                                                                  .finished = lock_finished};
static void configure(void *d, struct ext_session_lock_surface_v1 *l, uint32_t serial, uint32_t w,
                      uint32_t h) {
    struct wl_surface *s = d;
    ext_session_lock_surface_v1_ack_configure(l, serial);
    size_t bytes = (size_t)w * h * 4;
    int fd = memfd_create("air-test-black-lock", 0);
    ftruncate(fd, bytes);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, bytes);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_surface_attach(s, buffer, 0, 0);
    wl_surface_damage_buffer(s, 0, 0, w, h);
    wl_surface_commit(s);
}
static const struct ext_session_lock_surface_v1_listener surface_listener = {.configure =
                                                                                 configure};
int main(void) {
    if (!getenv("AIR_TEST_CONFIRM_DISPOSABLE") ||
        strcmp(getenv("AIR_TEST_CONFIRM_DISPOSABLE"), "1"))
        return 2;
    struct wl_display *d = wl_display_connect(NULL);
    if (!d)
        return 1;
    struct wl_registry *r = wl_display_get_registry(d);
    wl_registry_add_listener(r, &registry_listener, NULL);
    wl_display_roundtrip(d);
    if (!compositor || !shm || !manager)
        return 1;
    struct ext_session_lock_v1 *l = ext_session_lock_manager_v1_lock(manager);
    ext_session_lock_v1_add_listener(l, &lock_listener, NULL);
    for (int i = 0; i < noutputs; i++) {
        struct wl_surface *s = wl_compositor_create_surface(compositor);
        struct ext_session_lock_surface_v1 *ls =
            ext_session_lock_v1_get_lock_surface(l, s, outputs[i]);
        ext_session_lock_surface_v1_add_listener(ls, &surface_listener, s);
    }
    wl_display_roundtrip(d);
    time_t deadline = time(NULL) + 4;
    while (locked >= 0 && time(NULL) < deadline) {
        wl_display_dispatch_pending(d);
        while (wl_display_prepare_read(d) != 0)
            wl_display_dispatch_pending(d);
        wl_display_flush(d);
        struct pollfd p = {wl_display_get_fd(d), POLLIN, 0};
        int n = poll(&p, 1, 50);
        if (n > 0 && p.revents & POLLIN) {
            if (wl_display_read_events(d) < 0)
                return 1;
        } else
            wl_display_cancel_read(d);
    }
    if (locked == 1) {
        ext_session_lock_v1_unlock_and_destroy(l);
        wl_display_roundtrip(d);
        puts("UNLOCKED");
    }
    wl_display_disconnect(d);
    return locked == 1 ? 0 : 1;
}
