/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "air-spatial.h"
#include "air-math.h"
#include "common/scene-helpers.h"
#include "common/set.h"
#include "input/cursor.h"
#include "labwc.h"
#include "node.h"
#include "output-state.h"
#include "output.h"
#include "session-lock.h"
#include "view.h"
#include "workspaces.h"
#include <dirent.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend/wayland.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>

enum mode { DESKTOP, ARMING, SPATIAL, RESTORING };
struct entry {
    struct view *view;
    struct air_panel panel;
};
static struct {
    enum mode mode;
    int fd;
    pid_t pid;
    struct wl_client *client;
    struct wl_event_source *io, *timer;
    struct wl_listener client_destroy;
    struct wlr_layer_surface_v1 *layer;
    struct wl_listener layer_destroy;
    struct wlr_surface *cursor_surface;
    struct wl_listener cursor_commit, cursor_destroy;
    int cursor_hot_x, cursor_hot_y;
    uint64_t generation, epoch, ack, heartbeat, pose_stamp, started;
    uint64_t saved_focus;
    double flat_x, flat_y, theta, height, basis[9], aspect;
    struct air_vec last_grab_point;
    bool have_grab_point;
    bool ready, confirmed, enabled, power, restore_pending, pointer_pending;
    uint64_t restore_retry;
    struct entry scene[AIR_MAX_PANELS];
    int count;
    struct {
        uint64_t id;
        struct workspace *workspace;
        double angle, height;
    } poses[512];
    int poses_count;
    struct {
        char name[128];
        bool enabled;
        char identity[512];
    } outputs[32];
    int noutputs;
    char target_name[128];
} xr = {.fd = -1};

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
/* labwc's output_from_name deliberately filters powered-off outputs. Recovery must not. */
static struct output *find_output(const char *name) {
    if (!name)
        return NULL;
    struct output *output;
    wl_list_for_each(output, &server.outputs,
                     link) if (output->wlr_output->name &&
                               !strcmp(name, output->wlr_output->name)) return output;
    return NULL;
}
static bool original_air(struct output *output) {
    return output && output->wlr_output->model &&
           (strcasestr(output->wlr_output->model, "nreal air") ||
            strcasestr(output->wlr_output->model, "xreal air"));
}
static bool usb_air_present(void) {
    DIR *dir = opendir("/sys/bus/usb/devices");
    if (!dir)
        return false;
    struct dirent *entry;
    bool found = false;
    while ((entry = readdir(dir))) {
        char path[512];
        unsigned vendor = 0, product = 0;
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idVendor", entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        int ok = fscanf(f, "%x", &vendor);
        fclose(f);
        if (ok != 1 || vendor != 0x3318)
            continue;
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idProduct", entry->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;
        ok = fscanf(f, "%x", &product);
        fclose(f);
        if (ok == 1 && product == 0x0424) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}
static struct output *select_output(const char *name) {
    struct output *target = find_output(name);
    /* Virtual outputs are allowed for the nested, non-production test harness. */
    if (target && wlr_output_is_wl(target->wlr_output))
        return target;
    if (!target || !original_air(target)) {
        target = NULL;
        struct output *candidate;
        wl_list_for_each(candidate, &server.outputs,
                         link) if (original_air(candidate) && output_is_usable(candidate)) {
            if (target)
                return NULL;
            target = candidate;
        }
    }
    if (!target || !usb_air_present())
        return NULL;
    struct wlr_box target_box;
    wlr_output_layout_get_box(server.output_layout, target->wlr_output, &target_box);
    struct output *other;
    wl_list_for_each(other, &server.outputs, link) if (other != target && output_is_usable(other)) {
        struct wlr_box box, intersection;
        wlr_output_layout_get_box(server.output_layout, other->wlr_output, &box);
        if (wlr_box_intersection(&intersection, &target_box, &box))
            return NULL;
    }
    return target;
}
static bool locked(void) {
    return server.session_lock_manager && server.session_lock_manager->locked;
}
bool air_spatial_active(void) { return xr.mode == SPATIAL && !locked(); }
static bool send_packet(struct air_packet *m, int fd) {
    if (xr.fd < 0)
        return false;
    m->magic = AIR_MAGIC;
    m->version = AIR_VERSION;
    m->size = sizeof(*m);
    m->generation = xr.generation;
    m->epoch = xr.epoch;
    m->stamp_ns = now_ns();
    struct iovec iov = {m, sizeof(*m)};
    char ancillary[CMSG_SPACE(sizeof(int))];
    struct msghdr hdr = {.msg_iov = &iov, .msg_iovlen = 1};
    if (fd >= 0) {
        memset(ancillary, 0, sizeof(ancillary));
        hdr.msg_control = ancillary;
        hdr.msg_controllen = sizeof(ancillary);
        struct cmsghdr *c = CMSG_FIRSTHDR(&hdr);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    return sendmsg(xr.fd, &hdr, MSG_DONTWAIT | MSG_NOSIGNAL) == sizeof(*m);
}
static void status(const char *text) {
    struct air_packet m = {.type = AIR_STATUS};
    snprintf(m.data.text, sizeof(m.data.text), "%s", text);
    send_packet(&m, -1);
    wlr_log(WLR_INFO, "spatial: %s", text);
}
static struct view *find_view(uint64_t id) {
    struct view *v;
    wl_list_for_each(v, &server.views,
                     link) if (v->creation_id == id && v->mapped && !v->minimized) return v;
    return NULL;
}
static bool eligible(struct view *v) {
    return v && v->mapped && !v->minimized && v->surface &&
           (v->visible_on_all_workspaces || v->workspace == server.workspaces.current);
}
static void bbox(struct wlr_scene_buffer *b, int x, int y, void *data) {
    struct wlr_box *box = data;
    if (!b->buffer)
        return;
    int w = b->dst_width ? b->dst_width : b->buffer->width,
        h = b->dst_height ? b->dst_height : b->buffer->height;
    int x1 = box->width ? box->x : x, y1 = box->height ? box->y : y;
    int x2 = box->width ? box->x + box->width : x + w,
        y2 = box->height ? box->y + box->height : y + h;
    box->x = x < x1 ? x : x1;
    box->y = y < y1 ? y : y1;
    box->width = (x + w > x2 ? x + w : x2) - box->x;
    box->height = (y + h > y2 ? y + h : y2) - box->y;
}
static int pose_for(struct view *v, int index) {
    for (int i = 0; i < xr.poses_count; i++)
        if (xr.poses[i].id == v->creation_id && xr.poses[i].workspace == v->workspace)
            return i;
    if (xr.poses_count == 512)
        return -1;
    int i = xr.poses_count++;
    xr.poses[i].id = v->creation_id;
    xr.poses[i].workspace = v->workspace;
    xr.poses[i].angle = awrap(index == 0 ? 0 : ((index + 1) / 2) * .98 * (index % 2 ? -1 : 1));
    xr.poses[i].height = 0;
    return i;
}
static bool scene_update(void) {
    struct entry next[AIR_MAX_PANELS] = {0};
    int n = 0;
    struct view *v;
    wl_list_for_each(v, &server.views, link) {
        if (!eligible(v))
            continue;
        if (n == AIR_MAX_PANELS) {
            air_spatial_stop("too many mapped windows for this build");
            return false;
        }
        struct air_panel *p = &next[n].panel;
        next[n].view = v;
        p->id = v->creation_id;
        struct wlr_ext_foreign_toplevel_handle_v1 *handle;
        bool found = false;
        wl_list_for_each(handle, &server.foreign_toplevel_list->toplevels, link) {
            if (handle->data == v) {
                snprintf(p->identifier, sizeof(p->identifier), "%s", handle->identifier);
                found = true;
                break;
            }
        }
        if (!found)
            continue;
        p->width = v->current.width;
        p->height = v->current.height;
        struct wlr_box box = {0};
        wlr_scene_node_for_each_buffer(&v->capture.scene->tree.node, bbox, &box);
        if (box.width <= 0 || box.height <= 0)
            continue;
        p->bx = box.x;
        p->by = box.y;
        p->bw = box.width;
        p->bh = box.height;
        p->metres_per_pixel = fmin(.0009, 1.3 / fmax(1, p->width));
        struct view *parent = v->impl->get_parent ? v->impl->get_parent(v) : NULL;
        struct view *root = v;
        int depth = 0;
        while (root->impl->get_parent && root->impl->get_parent(root) && depth < 16) {
            root = root->impl->get_parent(root);
            depth++;
        }
        if (depth == 16) {
            air_spatial_stop("cyclic or excessively deep transient group");
            return false;
        }
        int root_pose = pose_for(root, n);
        int i = pose_for(v, parent ? 0 : n);
        if (i < 0 || root_pose < 0) {
            air_spatial_stop("panel pose capacity exceeded");
            return false;
        }
        p->azimuth = xr.poses[root_pose].angle;
        p->elevation = xr.poses[root_pose].height;
        p->distance = AIR_RADIUS - depth * .005;
        if (parent) {
            p->metres_per_pixel = fmin(.0009, 1.3 / fmax(1, root->current.width));
            p->azimuth = awrap(p->azimuth + xr.poses[i].angle);
            p->elevation += xr.poses[i].height + (root->current.y + root->current.height * .5 -
                                                  v->current.y - v->current.height * .5) *
                                                     p->metres_per_pixel;
            p->tangent = (v->current.x + v->current.width * .5 - root->current.x -
                          root->current.width * .5) *
                         p->metres_per_pixel;
        }
        n++;
    }
    bool changed = n != xr.count;
    for (int i = 0; i < n && !changed; i++)
        changed = memcmp(&next[i].panel, &xr.scene[i].panel, sizeof(struct air_panel)) != 0;
    if (!changed)
        return false;
    memcpy(xr.scene, next, sizeof(next));
    xr.count = n;
    xr.epoch++;
    struct air_packet m = {.type = AIR_SCENE_BEGIN};
    if (!send_packet(&m, -1))
        goto failed;
    for (int i = n - 1; i >= 0; i--) {
        m = (struct air_packet){.type = AIR_PANEL};
        m.data.panel = next[i].panel;
        if (!send_packet(&m, -1))
            goto failed;
    }
    m = (struct air_packet){.type = AIR_SCENE_COMMIT};
    if (!send_packet(&m, -1))
        goto failed;
    return true;
failed:
    air_spatial_stop("presentation channel congested");
    return false;
}
static bool hit_context(struct cursor_context *ctx, struct air_vec *point) {
    struct air_vec d = aray(xr.theta, xr.height);
    double best = 1e30;
    bool hit = false;
    *ctx = (struct cursor_context){.type = LAB_NODE_ROOT};
    *point = ascale(d, AIR_RADIUS);
    bool grab = server.seat.pressed.ctx.surface &&
                !wlr_seat_pointer_has_grab(server.seat.wlr_seat) && !server.seat.drag.active;
    for (int i = 0; i < xr.count; i++) {
        struct entry *e = &xr.scene[i];
        if (!eligible(e->view))
            continue;
        double x, y, t;
        if (!aplane(&e->panel, d, &x, &y, &t))
            continue;
        if (grab) {
            struct cursor_context *pressed = &server.seat.pressed.ctx;
            struct view *root = pressed->view;
            if (root != e->view)
                continue;
            int nx, ny;
            wlr_scene_node_coords(pressed->node, &nx, &ny);
            *ctx = *pressed;
            ctx->sx = x - nx;
            ctx->sy = y - ny;
            *point = ascale(d, t);
            xr.last_grab_point = *point;
            xr.have_grab_point = true;
            return true;
        }
        if (t >= best - 1e-6)
            continue;
        double sx, sy;
        struct wlr_scene_node *node =
            wlr_scene_node_at(&e->view->capture.scene->tree.node, x, y, &sx, &sy);
        struct wlr_surface *surface = node ? lab_wlr_surface_from_node(node) : NULL;
        if (!surface)
            continue; /* Input holes allow a farther panel to win. */
        struct wlr_surface *root_surface = wlr_surface_get_root_surface(surface);
        struct view *target = NULL;
        struct wlr_scene_node *live_node = root_surface->data;
        while (live_node) {
            struct node_descriptor *desc = live_node->data;
            if (desc && desc->view) {
                target = desc->view;
                break;
            }
            live_node = live_node->parent ? &live_node->parent->node : NULL;
        }
        *ctx = (struct cursor_context){.view = target ? target : e->view,
                                       .node = node,
                                       .surface = surface,
                                       .type = LAB_NODE_CLIENT,
                                       .sx = sx,
                                       .sy = sy};
        best = t;
        *point = ascale(d, t);
        hit = true;
    }
    if (grab && server.seat.pressed.ctx.surface) {
        *ctx = server.seat.last_cursor_ctx.ctx.surface == server.seat.pressed.ctx.surface
                   ? server.seat.last_cursor_ctx.ctx
                   : server.seat.pressed.ctx;
        if (xr.have_grab_point)
            *point = xr.last_grab_point;
        return true;
    }
    xr.have_grab_point = hit;
    if (hit)
        xr.last_grab_point = *point;
    return hit;
}
static void pointer_packet(void) {
    struct cursor_context ctx;
    struct air_vec point;
    bool hit = hit_context(&ctx, &point);
    struct air_packet m = {.type = AIR_POINTER};
    m.data.pointer.xyz[0] = point.x;
    m.data.pointer.xyz[1] = point.y;
    m.data.pointer.xyz[2] = point.z;
    m.data.pointer.hit = hit;
    m.data.pointer.active = xr.mode == SPATIAL;
    if (!send_packet(&m, -1))
        air_spatial_stop("presentation pointer channel stalled");
}
bool air_spatial_context(struct cursor_context *ctx) {
    if (!air_spatial_active())
        return false;
    scene_update();
    if (!air_spatial_active())
        return false;
    if (xr.ack != xr.epoch) {
        *ctx = server.seat.last_cursor_ctx.ctx;
        if (!ctx->surface)
            *ctx = (struct cursor_context){.type = LAB_NODE_ROOT};
        return true;
    }
    struct air_vec point;
    hit_context(ctx, &point);
    return true;
}
bool air_spatial_motion(double dx, double dy) {
    if (!air_spatial_active())
        return false;
    if (server.seat.current_constraint || server.seat.drag.active) {
        air_spatial_stop("relative pointer constraint or drag requires desktop fallback");
        return false;
    }
    scene_update();
    if (!air_spatial_active())
        return false;
    xr.pointer_pending = xr.ack != xr.epoch;
    xr.theta = awrap(xr.theta + dx * .0012);
    xr.height = aclamp(xr.height - dy * .0012 * AIR_RADIUS, -1.2, 1.2);
    pointer_packet();
    return true;
}
bool air_spatial_press_allowed(void) {
    if (!air_spatial_active())
        return true;
    scene_update();
    if (!air_spatial_active())
        return false;
    struct cursor_context ctx;
    struct air_vec point;
    hit_context(&ctx, &point);
    return xr.ready && xr.ack == xr.epoch && now_ns() - xr.heartbeat < 250000000 &&
           now_ns() - xr.pose_stamp < 250000000 && avisible(point, xr.basis, xr.aspect);
}
static void output_identity(struct output *o, char out[512]) {
    snprintf(out, 512, "%s|%s|%s", o->wlr_output->make ? o->wlr_output->make : "",
             o->wlr_output->model ? o->wlr_output->model : "",
             o->wlr_output->serial ? o->wlr_output->serial : "");
}
static bool output_power(const char *name, bool enabled) {
    struct output *o = find_output(name);
    if (!o)
        return false;
    char identity[512];
    output_identity(o, identity);
    for (int i = 0; i < xr.noutputs; i++)
        if (!strcmp(name, xr.outputs[i].name) && strcmp(identity, xr.outputs[i].identity)) {
            wlr_log(WLR_INFO, "spatial: identity changed for %s (%s -> %s)", name,
                    xr.outputs[i].identity, identity);
            return false;
        }
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, enabled);
    bool ok;
    if (enabled && locked()) {
        /* Atomically enable scanout with an already composed opaque lock frame. */
        ok = o->scene_output && wlr_scene_output_build_state(o->scene_output, &state, NULL) &&
             wlr_output_commit_state(o->wlr_output, &state);
    } else
        ok = wlr_output_commit_state(o->wlr_output, &state);
    wlr_output_state_finish(&state);
    ok = ok && o->wlr_output->enabled == enabled;
    wlr_log(WLR_INFO, "spatial: power %s requested=%d actual=%d result=%d", name, enabled,
            o->wlr_output->enabled, ok);
    if (!ok)
        wlr_log(WLR_ERROR, "spatial: output %s power %s deferred", name, enabled ? "on" : "off");
    return ok;
}
static void enter(void) {
    if (xr.mode == ARMING)
        scene_update();
    if (xr.mode != ARMING || !xr.ready || xr.ack != xr.epoch ||
        now_ns() - xr.pose_stamp > 250000000)
        return;
    if (server.seat.current_constraint || server.seat.drag.active ||
        server.seat.wlr_seat->pointer_state.button_count) {
        status("Release buttons and pointer constraints before entry");
        return;
    }
    xr.mode = SPATIAL;
    xr.confirmed = true;
    desktop_cancel_pending_auto_raise();
    wlr_cursor_unset_image(server.seat.cursor);
    status("SPATIAL | Super+Alt+Escape restores desktop");
    if (xr.power) {
        const char *target = xr.target_name;
        for (int i = 0; i < xr.noutputs; i++)
            if (xr.outputs[i].enabled && strcmp(xr.outputs[i].name, target)) {
                if (!output_power(xr.outputs[i].name, false)) {
                    air_spatial_stop("monitor sleep failed");
                    return;
                }
            }
    }
    cursor_update_focus();
    pointer_packet();
}
static void cursor_detach(void) {
    if (xr.cursor_surface) {
        wl_list_remove(&xr.cursor_commit.link);
        wl_list_remove(&xr.cursor_destroy.link);
        xr.cursor_surface = NULL;
    }
}
void air_spatial_stop(const char *reason) {
    if (xr.mode == DESKTOP || xr.mode == RESTORING)
        return;
    xr.mode = RESTORING;
    cursor_detach();
    wlr_log(WLR_INFO, "spatial: restoring desktop (%s)", reason);
    if (xr.io) {
        wl_event_source_remove(xr.io);
        xr.io = NULL;
    }
    xr.restore_pending = false;
    /* Restore scanout before dropping the presenter. Keep intentionally-off outputs off. */
    for (int i = 0; i < xr.noutputs; i++)
        if (xr.outputs[i].enabled && xr.outputs[i].name[0])
            if (!output_power(xr.outputs[i].name, true))
                xr.restore_pending = true;
    if (xr.power)
        for (int i = 0; i < xr.noutputs; i++)
            if (!xr.outputs[i].enabled && xr.outputs[i].name[0])
                output_power(xr.outputs[i].name, false);
    struct wlr_seat *seat = server.seat.wlr_seat;
    uint32_t buttons[WLR_POINTER_BUTTONS_CAP];
    size_t n = seat->pointer_state.button_count;
    if (n > WLR_POINTER_BUTTONS_CAP)
        n = WLR_POINTER_BUTTONS_CAP;
    for (size_t i = 0; i < n; i++)
        buttons[i] = seat->pointer_state.buttons[i].button;
    for (size_t i = 0; i < n; i++) {
        wlr_seat_pointer_notify_button(seat, now_ns() / 1000000, buttons[i],
                                       WL_POINTER_BUTTON_STATE_RELEASED);
        cursor_finish_button_release(&server.seat, buttons[i]);
        lab_set_add(&server.seat.bound_buttons, buttons[i]);
    }
    wlr_seat_pointer_notify_frame(seat);
    cursor_context_save(&server.seat.pressed, NULL);
    if (xr.fd >= 0) {
        close(xr.fd);
        xr.fd = -1;
    }
    if (xr.layer) {
        wl_list_remove(&xr.layer_destroy.link);
        xr.layer = NULL;
    }
    if (xr.client) {
        struct wl_client *client = xr.client;
        xr.client = NULL;
        wl_list_remove(&xr.client_destroy.link);
        wl_client_destroy(client);
    }
    if (xr.pid > 0) {
        kill(xr.pid, SIGKILL);
        xr.pid = 0;
    }
    xr.count = 0;
    xr.ready = false;
    xr.ack = 0;
    xr.pose_stamp = 0;
    xr.pointer_pending = false;
    xr.mode = DESKTOP;
    wlr_cursor_warp_closest(server.seat.cursor, NULL, xr.flat_x, xr.flat_y);
    if (strcmp(reason, "session locked")) {
        struct view *v = find_view(xr.saved_focus);
        if (v)
            desktop_focus_view(v, false);
    }
    cursor_update_image(&server.seat);
    cursor_update_focus();
}
static void client_destroyed(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
    wl_list_remove(&xr.client_destroy.link);
    xr.client = NULL;
    air_spatial_stop("presenter disconnected");
}
static void layer_destroyed(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
    wl_list_remove(&xr.layer_destroy.link);
    xr.layer = NULL;
    air_spatial_stop("presentation surface removed");
}
bool air_spatial_layer(struct wlr_layer_surface_v1 *layer) {
    struct wl_client *client = wl_resource_get_client(layer->resource);
    if (strcmp(layer->namespace, AIR_NAMESPACE))
        return true;
    if (client != xr.client || xr.mode != ARMING || xr.layer || locked())
        return false;
    struct output *o = find_output(xr.target_name);
    if (!o)
        return false;
    layer->output = o->wlr_output;
    xr.layer = layer;
    xr.layer_destroy.notify = layer_destroyed;
    wl_signal_add(&layer->events.destroy, &xr.layer_destroy);
    return true;
}
bool air_spatial_capture_allowed(struct wl_client *client) {
    if (locked())
        return false;
    if (client != xr.client)
        return true;
    return xr.mode == ARMING || xr.mode == SPATIAL;
}
static int socket_read(int fd, uint32_t mask, void *data) {
    (void)data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        air_spatial_stop("helper pipe closed");
        return 0;
    }
    struct air_packet m;
    ssize_t n;
    while ((n = recv(fd, &m, sizeof(m), MSG_DONTWAIT)) > 0) {
        if (n != sizeof(m) || m.magic != AIR_MAGIC || m.version != AIR_VERSION ||
            m.size != sizeof(m) || m.generation != xr.generation) {
            air_spatial_stop("invalid private protocol packet");
            return 0;
        }
        if (m.type == AIR_READY) {
            xr.aspect =
                m.data.ready.height ? (double)m.data.ready.width / m.data.ready.height : 16. / 9;
            xr.ready = true;
        } else if (m.type == AIR_PRESENTED && m.epoch == xr.epoch && m.stamp_ns <= now_ns() &&
                   now_ns() - m.stamp_ns < 250000000) {
            bool new_revision = xr.ack != m.epoch;
            xr.ack = m.epoch;
            xr.heartbeat = m.stamp_ns;
            if (new_revision && xr.mode == SPATIAL) {
                if (xr.pointer_pending) {
                    double sx, sy;
                    if (cursor_process_motion(now_ns() / 1000000, &sx, &sy))
                        wlr_seat_pointer_notify_motion(server.seat.wlr_seat, now_ns() / 1000000, sx,
                                                       sy);
                    xr.pointer_pending = false;
                } else
                    cursor_update_focus();
            }
        } else if (m.type == AIR_POSE) {
            bool valid = m.data.pose.age >= 0 && m.data.pose.age < .25;
            for (int i = 0; i < 9; i++)
                valid =
                    valid && isfinite(m.data.pose.basis[i]) && fabs(m.data.pose.basis[i]) <= 1.001;
            for (int i = 0; i < 3; i++)
                valid = valid &&
                        fabs(adot(abaxis(m.data.pose.basis, i), abaxis(m.data.pose.basis, i)) - 1) <
                            .02;
            if (valid) {
                memcpy(xr.basis, m.data.pose.basis, sizeof(xr.basis));
                xr.pose_stamp = now_ns() - (uint64_t)(m.data.pose.age * 1e9);
            }
        } else if (m.type == AIR_SHUTDOWN) {
            air_spatial_stop("renderer requested shutdown");
            return 0;
        }
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
        air_spatial_stop("renderer exited");
    return 0;
}
static void start(void) {
    const char *helper = getenv("AIR_SPATIAL_HELPER"), *configured = getenv("AIR_SPATIAL_OUTPUT");
    struct output *selected = select_output(configured);
    const char *target = selected ? selected->wlr_output->name : NULL;
    if (!helper || helper[0] != '/' || !target || locked() || xr.restore_pending ||
        !output_is_usable(selected)) {
        wlr_log(WLR_ERROR, "spatial: require one extended original Air with USB, a valid helper, "
                           "and completed output recovery");
        return;
    }
    if (server.input_mode != LAB_INPUT_STATE_PASSTHROUGH || server.seat.drag.active ||
        server.seat.current_constraint)
        return;
    snprintf(xr.target_name, sizeof(xr.target_name), "%s", target);
    xr.flat_x = server.seat.cursor->x;
    xr.flat_y = server.seat.cursor->y;
    xr.saved_focus = server.active_view ? server.active_view->creation_id : 0;
    xr.noutputs = 0;
    struct output *o;
    wl_list_for_each(o, &server.outputs, link) {
        if (xr.noutputs == 32)
            return;
        snprintf(xr.outputs[xr.noutputs].name, 128, "%s", o->wlr_output->name);
        output_identity(o, xr.outputs[xr.noutputs].identity);
        xr.outputs[xr.noutputs++].enabled = o->wlr_output->enabled;
        wlr_log(WLR_INFO, "spatial snapshot: %s enabled=%d", o->wlr_output->name,
                o->wlr_output->enabled);
    }
    int pair[2], wayland[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0)
        return;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wayland) < 0) {
        close(pair[0]);
        close(pair[1]);
        return;
    }
    xr.generation++;
    xr.epoch = 0;
    xr.ack = 0;
    xr.count = -1;
    xr.ready = false;
    xr.mode = ARMING;
    xr.started = now_ns();
    xr.heartbeat = xr.started;
    xr.pose_stamp = 0;
    pid_t pid = fork();
    if (pid == 0) {
        close(pair[0]);
        close(wayland[0]);
        fcntl(pair[1], F_SETFD, 0);
        fcntl(wayland[1], F_SETFD, 0);
        char fdstr[32], wlstr[32], generation[32];
        snprintf(fdstr, sizeof(fdstr), "%d", pair[1]);
        snprintf(wlstr, sizeof(wlstr), "%d", wayland[1]);
        snprintf(generation, sizeof(generation), "%llu", (unsigned long long)xr.generation);
        setenv("WAYLAND_SOCKET", wlstr, 1);
        setenv("AIR_SPATIAL_FD", fdstr, 1);
        setenv("AIR_SPATIAL_GENERATION", generation, 1);
        execl(helper, helper, "--output", target, (char *)NULL);
        _exit(127);
    }
    close(pair[1]);
    close(wayland[1]);
    if (pid < 0) {
        close(pair[0]);
        close(wayland[0]);
        xr.mode = DESKTOP;
        return;
    }
    xr.pid = pid;
    xr.fd = pair[0];
    fcntl(xr.fd, F_SETFL, O_NONBLOCK);
    xr.client = wl_client_create(server.wl_display, wayland[0]);
    if (!xr.client) {
        close(wayland[0]);
        air_spatial_stop("private Wayland connection failed");
        return;
    }
    xr.client_destroy.notify = client_destroyed;
    wl_client_add_destroy_listener(xr.client, &xr.client_destroy);
    xr.io = wl_event_loop_add_fd(server.wl_event_loop, xr.fd, WL_EVENT_READABLE, socket_read, NULL);
    scene_update();
    status(xr.power
               ? "ARMING | Super+Alt+Return routes input + sleeps monitors; Super+Alt+X cancels"
               : "ARMING | Super+Alt+Return routes input; Super+Alt+X cancels");
    pointer_packet();
}
static void frame_callback(struct wlr_scene_buffer *buffer, int x, int y, void *data) {
    (void)x;
    (void)y;
    struct wlr_scene_surface *surface = wlr_scene_surface_try_from_buffer(buffer);
    if (surface)
        wlr_surface_send_frame_done(surface->surface, data);
}
static int tick(void *data) {
    (void)data;
    wl_event_source_timer_update(xr.timer, 16);
    if (xr.restore_pending && now_ns() - xr.restore_retry > 100000000) {
        xr.restore_retry = now_ns();
        bool pending = false;
        for (int i = 0; i < xr.noutputs; i++) {
            struct output *output = xr.outputs[i].name[0] ? find_output(xr.outputs[i].name) : NULL;
            if (output && xr.outputs[i].enabled && !output->wlr_output->enabled &&
                !output_power(xr.outputs[i].name, true))
                pending = true;
        }
        xr.restore_pending = pending;
        if (!pending)
            wlr_log(WLR_INFO, "spatial: physical output restoration complete");
    }
    if (xr.mode != ARMING && xr.mode != SPATIAL)
        return 0;
    if (locked()) {
        air_spatial_stop("session locked");
        return 0;
    }
    if (!output_is_usable(find_output(xr.target_name))) {
        air_spatial_stop("glasses output disconnected");
        return 0;
    }
    if (xr.mode == SPATIAL && (server.seat.drag.active || server.seat.current_constraint ||
                               (server.input_mode != LAB_INPUT_STATE_PASSTHROUGH &&
                                server.input_mode != LAB_INPUT_STATE_CYCLE))) {
        air_spatial_stop("unsupported desktop grab; use desktop fallback");
        return 0;
    }
    uint64_t now = now_ns();
    if ((xr.mode == SPATIAL &&
         (now - xr.pose_stamp > 700000000 || now - xr.heartbeat > 1000000000)) ||
        (xr.mode == ARMING && now - xr.started > 120000000000ULL)) {
        air_spatial_stop("tracking or presentation watchdog expired");
        return 0;
    }
    scene_update();
    if (xr.mode == DESKTOP)
        return 0;
    if (xr.mode == SPATIAL) {
        wlr_cursor_unset_image(server.seat.cursor);
        struct timespec when;
        clock_gettime(CLOCK_MONOTONIC, &when);
        for (int i = 0; i < xr.count; i++)
            if (eligible(xr.scene[i].view))
                wlr_scene_node_for_each_buffer(&xr.scene[i].view->capture.scene->tree.node,
                                               frame_callback, &when);
        pointer_packet();
    }
    if (xr.mode == ARMING && xr.confirmed)
        enter();
    return 0;
}
bool air_spatial_key(struct wlr_keyboard *kb, struct wlr_keyboard_key_event *ev) {
    if (!xr.enabled)
        return false;
    static bool swallowed[KEY_MAX + 1];
    if (ev->keycode > KEY_MAX)
        return false;
    if (ev->state == WL_KEYBOARD_KEY_STATE_RELEASED && swallowed[ev->keycode]) {
        swallowed[ev->keycode] = false;
        return true;
    }
    uint32_t mods = wlr_keyboard_get_modifiers(kb);
    if (ev->state != WL_KEYBOARD_KEY_STATE_PRESSED ||
        (mods & (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT)) != (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT))
        return false;
    uint32_t key = ev->keycode;
    bool handled = true;
    if (key == KEY_ESC)
        air_spatial_stop("force desktop hotkey");
    else if (locked())
        return false;
    else if (key == KEY_X) {
        if (xr.mode == DESKTOP)
            start();
        else
            air_spatial_stop("toggle cancelled");
    } else if (key == KEY_ENTER && xr.mode == ARMING)
        enter();
    else if (key == KEY_R && xr.mode != DESKTOP) {
        struct air_packet m = {.type = AIR_RECENTER};
        send_packet(&m, -1);
    } else if (key == KEY_F && xr.mode == SPATIAL) {
        struct air_vec f = abaxis(xr.basis, 2);
        xr.theta = atan2(f.x, f.z);
        xr.height = aclamp(AIR_RADIUS * f.y / fmax(.001, hypot(f.x, f.z)), -1.2, 1.2);
        pointer_packet();
        cursor_update_focus();
    } else if ((key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN) &&
               xr.mode == SPATIAL) {
        struct view *v = server.active_view;
        for (int i = 0; i < xr.poses_count; i++)
            if (v && xr.poses[i].id == v->creation_id && xr.poses[i].workspace == v->workspace) {
                xr.poses[i].angle = awrap(xr.poses[i].angle + (key == KEY_RIGHT  ? .08
                                                               : key == KEY_LEFT ? -.08
                                                                                 : 0));
                xr.poses[i].height = aclamp(xr.poses[i].height + (key == KEY_UP     ? .04
                                                                  : key == KEY_DOWN ? -.04
                                                                                    : 0),
                                            -1.2, 1.2);
                break;
            }
        scene_update();
        pointer_packet();
        cursor_update_focus();
    } else
        handled = false;
    if (handled)
        swallowed[key] = true;
    return handled;
}
/* Cursor images are transferred as an anonymous SHM FD, never embedded in window capture. */
static bool cursor_pixels(const void *pixels, int width, int height, int stride, int hx, int hy,
                          uint32_t format, float scale) {
    if (!air_spatial_active())
        return false;
    if (width <= 0 || height <= 0 || width > 256 || height > 256)
        return true;
    size_t size = (size_t)width * height * 4;
    int fd = memfd_create("air-cursor", MFD_CLOEXEC);
    if (fd < 0)
        return true;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return true;
    }
    void *dst = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (dst == MAP_FAILED) {
        close(fd);
        return true;
    }
    for (int y = 0; y < height; y++)
        memcpy((char *)dst + y * width * 4, (const char *)pixels + y * stride, width * 4);
    munmap(dst, size);
    struct air_packet m = {.type = AIR_CURSOR};
    m.data.cursor.width = width;
    m.data.cursor.height = height;
    m.data.cursor.hot_x = hx;
    m.data.cursor.hot_y = hy;
    m.data.cursor.format = format;
    m.data.cursor.scale = scale;
    send_packet(&m, fd);
    close(fd);
    return true;
}
bool air_spatial_cursor_name(const char *name) {
    if (!air_spatial_active())
        return false;
    cursor_detach();
    struct wlr_xcursor *cursor =
        wlr_xcursor_manager_get_xcursor(server.seat.xcursor_manager, name, 1);
    if (cursor && cursor->image_count) {
        struct wlr_xcursor_image *i = cursor->images[0];
        return cursor_pixels(i->buffer, i->width, i->height, i->width * 4, i->hotspot_x,
                             i->hotspot_y, 0, 1);
    }
    return true;
}
static void cursor_committed(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
    air_spatial_cursor(xr.cursor_surface, xr.cursor_hot_x, xr.cursor_hot_y);
}
static void cursor_destroyed(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
    cursor_detach();
    if (air_spatial_active()) {
        struct air_packet m = {.type = AIR_CURSOR};
        send_packet(&m, -1);
    }
}
bool air_spatial_cursor(struct wlr_surface *surface, int hx, int hy) {
    if (!air_spatial_active())
        return false;
    if (surface != xr.cursor_surface) {
        cursor_detach();
        xr.cursor_surface = surface;
        if (surface) {
            xr.cursor_commit.notify = cursor_committed;
            xr.cursor_destroy.notify = cursor_destroyed;
            wl_signal_add(&surface->events.commit, &xr.cursor_commit);
            wl_signal_add(&surface->events.destroy, &xr.cursor_destroy);
        }
    }
    xr.cursor_hot_x = hx;
    xr.cursor_hot_y = hy;
    if (!surface || !surface->buffer) {
        struct air_packet m = {.type = AIR_CURSOR};
        send_packet(&m, -1);
        return true;
    }
    struct wlr_buffer *buffer = surface->buffer->source;
    void *pixels;
    uint32_t format;
    size_t stride;
    if (buffer && wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &pixels,
                                                   &format, &stride)) {
        /* Only these four-byte formats are valid for the copy below. */
        if (format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888 ||
            format == DRM_FORMAT_ABGR8888 || format == DRM_FORMAT_XBGR8888)
            cursor_pixels(pixels, buffer->width, buffer->height, stride,
                          hx * surface->current.scale, hy * surface->current.scale,
                          format == DRM_FORMAT_ABGR8888 ? 0x34324241u : 0, surface->current.scale);
        wlr_buffer_end_data_ptr_access(buffer);
    } else
        air_spatial_cursor_name("default");
    return true;
}
void air_spatial_init(void) {
    xr.enabled = getenv("AIR_SPATIAL_HELPER") != NULL;
    xr.power = getenv("AIR_SPATIAL_POWER") && !strcmp(getenv("AIR_SPATIAL_POWER"), "1");
    if (xr.enabled) {
        xr.timer = wl_event_loop_add_timer(server.wl_event_loop, tick, NULL);
        wl_event_source_timer_update(xr.timer, 16);
        wlr_log(WLR_INFO, "native spatial feature ready (power transitions %s)",
                xr.power ? "enabled" : "disabled");
    }
}
void air_spatial_finish(void) {
    air_spatial_stop("compositor shutdown");
    if (xr.timer) {
        wl_event_source_remove(xr.timer);
        xr.timer = NULL;
    }
}

void air_spatial_forget_view(struct view *v) {
    for (int i = 0; i < xr.count; i++)
        if (xr.scene[i].view == v) {
            xr.scene[i].view = NULL;
            xr.ack = 0;
        }
}

void air_spatial_helper_exited(pid_t pid) {
    if (pid == xr.pid) {
        xr.pid = 0;
        air_spatial_stop("presenter exited");
    }
}
void air_spatial_output_lost(struct output *output) {
    if (xr.mode == DESKTOP || xr.mode == RESTORING)
        return;
    for (int i = 0; i < xr.noutputs; i++)
        if (!strcmp(xr.outputs[i].name, output->wlr_output->name))
            xr.outputs[i].name[0] = 0;
    air_spatial_stop("output topology changed");
}
bool air_spatial_presenter(struct wlr_surface *surface) {
    return surface && xr.client && wl_resource_get_client(surface->resource) == xr.client;
}

bool air_spatial_in_use(void) { return xr.mode == ARMING || xr.mode == SPATIAL; }

/* Nested automation owns only its virtual test devices. Host pointer-enter and
 * physical typing must not perturb a disposable test while the user works. */
bool air_spatial_ignore_host_test_input(struct wlr_input_device *device) {
    return getenv("AIR_NATIVE_TEST_ID") && wlr_input_device_is_wl(device);
}
