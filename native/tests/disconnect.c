/* Close the one disposable nested WL-2 window, simulating an output unplug.
 * This operates on the parent compositor, but refuses ambiguous targets. */
#define _GNU_SOURCE
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
struct top {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char *title, *app;
    int closed;
    struct top *next;
};
static struct top *tops;
static struct zwlr_foreign_toplevel_manager_v1 *manager;
static void title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *s) {
    (void)h;
    struct top *t = d;
    free(t->title);
    t->title = strdup(s);
}
static void app(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *s) {
    (void)h;
    struct top *t = d;
    free(t->app);
    t->app = strdup(s);
}
static void output(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {
    (void)d;
    (void)h;
    (void)o;
}
static void state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *s) {
    (void)d;
    (void)h;
    (void)s;
}
static void done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)d;
    (void)h;
}
static void closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)h;
    ((struct top *)d)->closed = 1;
}
static void parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                   struct zwlr_foreign_toplevel_handle_v1 *p) {
    (void)d;
    (void)h;
    (void)p;
}
static const struct zwlr_foreign_toplevel_handle_v1_listener top_listener = {.title = title,
                                                                             .app_id = app,
                                                                             .output_enter = output,
                                                                             .output_leave = output,
                                                                             .state = state,
                                                                             .done = done,
                                                                             .closed = closed,
                                                                             .parent = parent};
static void toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m,
                     struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)d;
    (void)m;
    struct top *t = calloc(1, sizeof(*t));
    t->handle = h;
    t->next = tops;
    tops = t;
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &top_listener, t);
}
static void finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m) {
    (void)d;
    (void)m;
}
static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = toplevel, .finished = finished};
static void global(void *d, struct wl_registry *r, uint32_t id, const char *name, uint32_t v) {
    (void)d;
    if (!strcmp(name, "zwlr_foreign_toplevel_manager_v1")) {
        manager =
            wl_registry_bind(r, id, &zwlr_foreign_toplevel_manager_v1_interface, v < 3 ? v : 3);
        zwlr_foreign_toplevel_manager_v1_add_listener(manager, &manager_listener, NULL);
    }
}
static void removed(void *d, struct wl_registry *r, uint32_t id) {
    (void)d;
    (void)r;
    (void)id;
}
static const struct wl_registry_listener registry_listener = {.global = global,
                                                              .global_remove = removed};
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
    wl_display_roundtrip(d);
    struct top *target = NULL;
    int count = 0;
    for (struct top *t = tops; t; t = t->next)
        if (!t->closed && t->title && t->app && !strcmp(t->app, "labwc") &&
            !strcmp(t->title, "labwc - WL-2")) {
            target = t;
            count++;
        }
    if (count != 1) {
        fprintf(stderr,
                "Refusing output-disconnect test: expected exactly one disposable labwc WL-2 "
                "window; found %d\n",
                count);
        return 2;
    }
    zwlr_foreign_toplevel_handle_v1_close(target->handle);
    wl_display_roundtrip(d);
    wl_display_disconnect(d);
    return 0;
}
