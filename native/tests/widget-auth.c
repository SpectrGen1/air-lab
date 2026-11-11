/* A normal client must never acquire the private widget capture manager. */
#include "ext-image-capture-source-v1-protocol.h"
#include "air-widget-v1-protocol.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
static struct air_widget_manager_v1 *manager;
static void global(void *data, struct wl_registry *r, uint32_t id, const char *name, uint32_t v) {
    if (!strcmp(name, "air_widget_manager_v1"))
        manager = wl_registry_bind(r, id, &air_widget_manager_v1_interface, 1);
}
static void removed(void *d, struct wl_registry *r, uint32_t id) {}
int main(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;
    struct wl_registry *r = wl_display_get_registry(display);
    static const struct wl_registry_listener l = {.global=global,.global_remove=removed};
    wl_registry_add_listener(r, &l, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    int result = manager && wl_display_get_error(display) == EPROTO ? 0 : 1;
    wl_display_disconnect(display);
    if (!result) puts("PASS: ordinary clients cannot bind private widget capture");
    return result;
}
