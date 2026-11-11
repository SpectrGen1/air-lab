/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIR_WIDGETS_H
#define AIR_WIDGETS_H
#include "air-spatial.h"
#include <wayland-server-core.h>
struct lab_layer_surface;
struct wlr_xdg_popup;
struct wlr_scene;
struct air_widget {
    struct wl_list link;
    uint64_t id;
    int role, reported_width, reported_height, pidfd;
    struct wlr_layer_surface_v1 *layer;
    struct wlr_scene *capture;
    struct wlr_ext_image_capture_source_v1 *source;
    struct wl_listener destroy, source_destroy;
    struct wl_list popups;
};
#if HAVE_AIR_SPATIAL
void air_widgets_init(void);
void air_widgets_finish(void);
void air_widgets_start(const char *output);
void air_widgets_stop(void);
void air_widgets_child_exited(pid_t pid);
void air_widgets_refresh(void);
void air_widgets_layer_created(struct lab_layer_surface *layer);
void air_widgets_hide(struct lab_layer_surface *layer);
void air_widgets_popup(struct wlr_xdg_popup *popup);
bool air_widget_has_popup(struct air_widget *widget);
struct air_widget *air_widget_find(uint64_t id);
struct air_widget *air_widget_for_surface(struct wlr_surface *surface);
struct wl_list *air_widgets_list(void);
#else
static inline void air_widgets_layer_created(struct lab_layer_surface *l) { (void)l; }
static inline void air_widgets_hide(struct lab_layer_surface *l) { (void)l; }
static inline void air_widgets_popup(struct wlr_xdg_popup *p) { (void)p; }
#endif
#endif
