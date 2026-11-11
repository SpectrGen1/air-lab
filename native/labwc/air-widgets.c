/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "air-widgets.h"
#include "air-widget-v1-protocol.h"
#include "air-protocol.h"
#include "labwc.h"
#include "layers.h"
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
static struct wl_list widgets;
static uint64_t next_id;
static pid_t children[3];
static struct wl_global *manager;
static bool running;
static char target[128];
static uint64_t retry_at;
struct widget_popup {
    struct wl_list link;
    struct wlr_surface *surface;
    struct wlr_scene_tree *tree;
    struct wl_listener destroy, commit;
    struct wlr_xdg_popup *popup;
    struct air_widget *widget;
    int width, height;
};
bool air_widget_has_popup(struct air_widget *widget) {
    struct widget_popup *p;
    wl_list_for_each(p, &widget->popups, link)
        if (p->surface->mapped) return true;
    return false;
}
struct wl_list *air_widgets_list(void) { return &widgets; }
struct air_widget *air_widget_find(uint64_t id) {
    struct air_widget *w;
    wl_list_for_each(w, &widgets, link)
        if (w->id == id)
            return w;
    return NULL;
}
struct air_widget *air_widget_for_surface(struct wlr_surface *surface) {
    for (int depth = 0; surface && depth < 32; depth++) {
        surface = wlr_surface_get_root_surface(surface);
        struct air_widget *w;
        wl_list_for_each(w, &widgets, link)
            if (w->layer->surface == surface)
                return w;
        struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(surface);
        if (!xdg || xdg->role != WLR_XDG_SURFACE_ROLE_POPUP)
            break;
        surface = xdg->popup->parent;
    }
    return NULL;
}
static void popup_destroyed(struct wl_listener *listener, void *data) {
    struct widget_popup *p = wl_container_of(listener, p, destroy);
    wl_list_remove(&p->link);
    wl_list_remove(&p->destroy.link);
    wl_list_remove(&p->commit.link);
    free(p);
}
static void popup_committed(struct wl_listener *listener, void *data) {
    struct widget_popup *p = wl_container_of(listener, p, commit);
    struct wlr_box g = p->popup->current.geometry;
    if (p->surface->mapped && g.width > 0 && (g.width != p->width || g.height != p->height)) {
        p->width = g.width; p->height = g.height;
        wlr_log(WLR_INFO, "spatial widget popup role=%d geometry=%d,%d,%d,%d",
                p->widget->role, g.x, g.y, g.width, g.height);
    }
}
void air_widgets_popup(struct wlr_xdg_popup *popup) {
    struct air_widget *w = air_widget_for_surface(popup->parent);
    if (!w)
        return;
    struct wlr_scene_tree *parent = &w->capture->tree;
    struct widget_popup *p;
    wl_list_for_each(p, &w->popups, link)
        if (p->surface == popup->parent)
            parent = p->tree;
    p = calloc(1, sizeof(*p));
    if (!p)
        return;
    p->popup = popup; p->widget = w;
    p->surface = popup->base->surface;
    p->commit.notify = popup_committed;
    wl_signal_add(&p->surface->events.commit, &p->commit);
    p->tree = wlr_scene_xdg_surface_create(parent, popup->base);
    if (!p->tree) { wl_list_remove(&p->commit.link); free(p); return; }
    p->destroy.notify = popup_destroyed;
    wl_signal_add(&p->tree->node.events.destroy, &p->destroy);
    wl_list_insert(&w->popups, &p->link);
}
static void source_destroyed(struct wl_listener *listener, void *data) {
    struct air_widget *w = wl_container_of(listener, w, source_destroy);
    wl_list_remove(&w->source_destroy.link);
    w->source = NULL;
}
static void widget_destroyed(struct wl_listener *listener, void *data) {
    struct air_widget *w = wl_container_of(listener, w, destroy);
    air_spatial_widget_removed(w->id);
    wl_list_remove(&w->destroy.link);
    wl_list_remove(&w->link);
    wlr_scene_node_destroy(&w->capture->tree.node);
    if (w->pidfd >= 0) close(w->pidfd);
    free(w);
}
void air_widgets_hide(struct lab_layer_surface *layer) {
    struct air_widget *w;
    wl_list_for_each(w, &widgets, link)
        if (w->layer == layer->layer_surface)
            wlr_scene_node_set_enabled(&layer->scene_layer_surface->tree->node, false);
}
void air_widgets_layer_created(struct lab_layer_surface *layer) {
    struct wlr_layer_surface_v1 *l = layer->layer_surface;
    pid_t pid; uid_t uid;
    wl_client_get_credentials(wl_resource_get_client(l->resource), &pid, &uid, NULL);
    int role = -1;
    for (int i = 0; i < 3; i++)
        if (children[i] > 0 && children[i] == pid)
            role = i;
    /* Wofi is an explicit same-user OS launcher allowlist entry. Command
     * launchers may double-fork, so process ancestry isn't stable evidence. */
    if (role < 0 && running && air_spatial_active() && uid == getuid() && !strcmp(l->namespace, "wofi"))
        role = 3;
    if (role < 0)
        return;
    struct air_widget *w = calloc(1, sizeof(*w));
    if (!w)
        return;
    w->pidfd = role == 3 ? syscall(SYS_pidfd_open, pid, 0) : -1;
    if (role == 3 && w->pidfd < 0) { free(w); return; }
    w->id = (UINT64_C(1) << 63) | ++next_id;
    w->role = role;
    w->layer = l;
    w->capture = wlr_scene_create();
    wl_list_init(&w->popups);
    if (!w->capture || !wlr_scene_subsurface_tree_create(&w->capture->tree, l->surface)) {
        if (w->capture) wlr_scene_node_destroy(&w->capture->tree.node);
        if (w->pidfd >= 0) close(w->pidfd);
        free(w); return;
    }
    w->destroy.notify = widget_destroyed;
    wl_signal_add(&l->events.destroy, &w->destroy);
    wl_list_insert(widgets.prev, &w->link);
    air_widgets_hide(layer);
    wlr_log(WLR_INFO, "spatial widget mapped role=%d id=%llu pid=%d", role, (unsigned long long)w->id, pid);
}
static void manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}
static void get_source(struct wl_client *client, struct wl_resource *resource,
                       uint32_t new_id, uint32_t hi, uint32_t lo) {
    if (!air_spatial_private_client(client) || !air_spatial_in_use()) {
        wl_resource_post_error(resource, 0, "private presenter required");
        return;
    }
    struct air_widget *w = air_widget_find(((uint64_t)hi << 32) | lo);
    if (w && !w->source) {
        w->source = wlr_ext_image_capture_source_v1_create_with_scene_node(
            &w->capture->tree.node, server.wl_event_loop, server.allocator, server.renderer);
        if (w->source) {
            w->source_destroy.notify = source_destroyed;
            wl_signal_add(&w->source->events.destroy, &w->source_destroy);
        }
    }
    /* NULL makes an inert source if a layer disappeared between commit/request. */
    wlr_ext_image_capture_source_v1_create_resource(w ? w->source : NULL, client, new_id);
}
static const struct air_widget_manager_v1_interface implementation = {
    .destroy = manager_destroy, .get_source = get_source};
static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &air_widget_manager_v1_interface, 1, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &implementation, NULL, NULL);
    if (!air_spatial_private_client(client))
        wl_resource_post_error(resource, 0, "private presenter required");
}
void air_widgets_init(void) {
    wl_list_init(&widgets);
    manager = wl_global_create(server.wl_display, &air_widget_manager_v1_interface, 1, NULL, bind_manager);
}
void air_widgets_start(const char *output) {
    snprintf(target, sizeof(target), "%s", output);
    running = true;
    retry_at = 0;
    air_widgets_refresh();
}
void air_widgets_child_exited(pid_t pid) {
    for (int i = 0; i < 3; i++)
        if (children[i] == pid) children[i] = 0;
}
void air_widgets_refresh(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    if (!running || now < retry_at) return;
    retry_at = now + 2000000000;
    pid_t parent = getpid();
    const char *helper = getenv("AIR_SPATIAL_WIDGET_HELPER");
    if (!helper || helper[0] != '/')
        return;
    for (int i = 0; i < 3; i++) {
        if (children[i] > 0) continue;
        pid_t pid = fork();
        if (pid == 0) {
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() != parent) _exit(1);
            char role[8];
            snprintf(role, sizeof(role), "%d", i);
            /* Waybar connects normally; only the presenter uses WAYLAND_SOCKET. */
            execl(helper, helper, role, target, (char *)NULL);
            _exit(127);
        }
        children[i] = pid > 0 ? pid : 0;
    }
}
void air_widgets_stop(void) {
    running = false;
    for (int i = 0; i < 3; i++) {
        if (children[i] > 0) kill(children[i], SIGKILL);
        children[i] = 0;
    }
    /* Detach mirrors now, without depending on when Waybar processes exit. */
    struct air_widget *w, *tmp;
    wl_list_for_each_safe(w, tmp, &widgets, link) {
        if (w->role == 3) {
            syscall(SYS_pidfd_send_signal, w->pidfd, SIGTERM, NULL, 0);
            wlr_layer_surface_v1_destroy(w->layer); /* Dismiss the transient launcher. */
        } else
            widget_destroyed(&w->destroy, NULL);
    }
}
void air_widgets_finish(void) {
    air_widgets_stop();
    if (manager) wl_global_destroy(manager);
}
