/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_AIR_SPATIAL_H
#define LABWC_AIR_SPATIAL_H
#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
struct output;
struct view;
struct cursor_context;
struct wlr_layer_surface_v1;
struct wl_client;
struct wlr_surface;
struct wlr_input_device;
struct wlr_keyboard;
struct wlr_keyboard_key_event;
#if HAVE_AIR_SPATIAL
void air_spatial_forget_view(struct view *view);
void air_spatial_helper_exited(pid_t pid);
void air_spatial_output_lost(struct output *output);
bool air_spatial_presenter(struct wlr_surface *surface);
bool air_spatial_ignore_host_test_input(struct wlr_input_device *device);
void air_spatial_init(void);
void air_spatial_finish(void);
void air_spatial_stop(const char *reason);
bool air_spatial_active(void);
bool air_spatial_in_use(void);
bool air_spatial_context(struct cursor_context *ctx);
bool air_spatial_motion(double dx, double dy);
bool air_spatial_key(struct wlr_keyboard *kb, struct wlr_keyboard_key_event *ev);
bool air_spatial_press_allowed(void);
bool air_spatial_layer(struct wlr_layer_surface_v1 *layer);
bool air_spatial_cursor(struct wlr_surface *surface, int hx, int hy);
bool air_spatial_cursor_name(const char *name);
bool air_spatial_capture_allowed(struct wl_client *client);
#else
static inline void air_spatial_helper_exited(pid_t pid) { (void)pid; }
static inline void air_spatial_output_lost(struct output *o) { (void)o; }
static inline bool air_spatial_presenter(struct wlr_surface *s) {
    (void)s;
    return false;
}
static inline void air_spatial_forget_view(struct view *v) { (void)v; }
static inline bool air_spatial_ignore_host_test_input(struct wlr_input_device *d) {
    (void)d;
    return false;
}
static inline void air_spatial_init(void) {}
static inline void air_spatial_finish(void) {}
static inline void air_spatial_stop(const char *s) { (void)s; }
static inline bool air_spatial_in_use(void) { return false; }
static inline bool air_spatial_active(void) { return false; }
static inline bool air_spatial_context(struct cursor_context *p) {
    (void)p;
    return false;
}
static inline bool air_spatial_motion(double x, double y) {
    (void)x;
    (void)y;
    return false;
}
static inline bool air_spatial_key(struct wlr_keyboard *k, struct wlr_keyboard_key_event *e) {
    (void)k;
    (void)e;
    return false;
}
static inline bool air_spatial_press_allowed(void) { return true; }
static inline bool air_spatial_layer(struct wlr_layer_surface_v1 *l) {
    (void)l;
    return true;
}
static inline bool air_spatial_cursor(struct wlr_surface *s, int x, int y) {
    (void)s;
    (void)x;
    (void)y;
    return false;
}
static inline bool air_spatial_cursor_name(const char *s) {
    (void)s;
    return false;
}
static inline bool air_spatial_capture_allowed(struct wl_client *c) {
    (void)c;
    return true;
}
#endif
#endif
