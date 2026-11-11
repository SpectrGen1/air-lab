/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "air-spatial.h"
#include "air-math.h"
#include "air-event.h"
#include "air-ipd.h"
#include "air-auto.h"
#include "air-watchdog.h"
#include "air-screen.h"
#include "air-cursor.h"
#include "air-widgets.h"
#include "air-workspace.h"
#include "common/macros.h"
#include "common/scene-helpers.h"
#include "common/set.h"
#include "input/cursor.h"
#include "input/keyboard.h"
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

enum mode { DESKTOP, ARMING, SPATIAL, RESTORING };
struct entry {
    struct view *view;
    struct air_panel panel;
};
struct presenter_child {
    struct wl_list link;
    struct wl_event_source *exit_timer;
    pid_t pid;
    uint64_t generation;
    bool termination_requested;
};
struct desktop_window {
    uint64_t id;
    struct wlr_box geometry;
};

static struct {
    enum mode mode;
    int fd;
    pid_t pid, attempt_pid;
    struct wl_list presenter_children;
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
    struct wl_array desktop_windows;
    bool desktop_restore_pending;
    double flat_x, flat_y, theta, height, basis[9], aspect;
    struct air_vec last_grab_point;
    bool have_grab_point, scene_head_only;
    double widget_yaw, zoom;
    bool widget_hold;
    uint64_t widget_presented_stamp;
    bool ready, enabled, power, restore_pending, pointer_pending;
    uint64_t restore_retry, auto_poll;
    bool auto_start, stereo, stereo_enabled, calibration, reserved_display;
    int reported_display_mode, reported_oled;
    struct wlr_scene_tree *reserved_tree, *presenter_mirror;
    struct wlr_scene_rect *reserved_black;
    struct wlr_surface *mirrored_surface;
    struct wl_listener mirror_destroy;
    char reserved_output[128];
    double ipd_mm, ipd_center_mm, ipd_baseline_mm;
    uint64_t ipd_poll;
    struct air_ipd ipd_profile;
    struct air_auto automatic;
    struct air_watchdog watchdog;
    char auto_output[128];
    struct entry scene[AIR_MAX_PANELS];
    int count;
    struct {
        uint64_t id;
        struct workspace *workspace;
        double angle, height, distance, depth_offset;
        int reference_width, spatial_width, spatial_height;
        uint64_t sized_generation;
        double scale;
        bool pinned;
    } poses[512];
    int poses_count;
    struct air_workspace_state workspace_state;
    bool restored[AIR_LAYOUT_WINDOWS];
    bool overview;
    char overview_query[96];
    unsigned overview_selected;
    uint32_t overview_revision, overview_presented;
    uint64_t overview_stamp;
    double overview_basis[9];
    uint64_t overview_ids[AIR_LAYOUT_WINDOWS];
    unsigned overview_count;
    bool overview_in_view[AIR_LAYOUT_WINDOWS];
    uint64_t overview_polled;
    double game_angle, game_height, game_scale;
    struct workspace *game_workspace;
    uint64_t game_id, dock_until;
    double dock_offset;
    uint64_t offscreen_frame;
    int frame_interval_ms;
    uint64_t hover_id, widget_grab_id;
    uint32_t hover_region, cursor_region;
    bool native_left_down;
    struct {
        uint64_t view_id, root_id;
        struct workspace *workspace;
        uint32_t region;
        double x, y, snap_heading;
        struct air_panel plane;
        struct wlr_box initial, requested;
        uint64_t resize_sent_at;
    } manipulation;
    struct {
        char name[128];
        bool enabled;
        char identity[512];
    } outputs[32];
    int noutputs;
    char target_name[128];
} xr = {.fd = -1, .zoom = 1, .workspace_state.magic = AIR_LAYOUT_MAGIC,
        .reported_display_mode = -1, .reported_oled = -1};
static double panel_scale = 2.0 / 3.0;
static double pointer_speed = 1, configured_pointer_speed = 1, pointer_gain;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static long long age_ms(uint64_t now, uint64_t stamp) {
    uint64_t age = awatchdog_age(now, stamp);
    return age == UINT64_MAX ? -1 : (long long)(age / 1000000);
}

static void spatial_event(const char *event, const char *reason) {
    char output[128], description[256];
    aevent_quote(output, sizeof(output), xr.target_name);
    aevent_quote(description, sizeof(description), reason);
    uint64_t now = now_ns();
    aevent_write("compositor", event,
        ",\"generation\":%llu,\"phase\":\"%s\",\"reason\":%s,\"output\":%s,"
        "\"presenter_pid\":%ld,\"stereo\":%s,\"ready\":%s,\"scene_epoch\":%llu,\"scene_ack\":%llu,"
        "\"elapsed_ms\":%lld,\"pose_age_ms\":%lld,\"frame_age_ms\":%lld,"
        "\"input_mode\":%d,\"pointer_constraint\":%d,\"drag_active\":%s,"
        "\"last_display_mode\":%d,\"last_oled\":%d",
        (unsigned long long)xr.generation, xr.mode == ARMING ? "arming" :
        xr.mode == SPATIAL ? "spatial" : xr.mode == RESTORING ? "restoring" : "desktop",
        description, output, (long)xr.attempt_pid, aevent_bool(xr.stereo), aevent_bool(xr.ready),
        (unsigned long long)xr.epoch, (unsigned long long)xr.ack,
        age_ms(now, xr.started), age_ms(now, xr.pose_stamp), age_ms(now, xr.heartbeat),
        server.input_mode, server.seat.current_constraint ? (int)server.seat.current_constraint->type : -1,
        aevent_bool(server.seat.drag.active), xr.reported_display_mode, xr.reported_oled);
}

static struct presenter_child *presenter_child_find(pid_t pid) {
    struct presenter_child *child;
    wl_list_for_each(child, &xr.presenter_children, link) {
        if (child->pid == pid)
            return child;
    }
    return NULL;
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
    if (target && getenv("AIR_NATIVE_TEST_ID") &&
        (wlr_output_is_wl(target->wlr_output) || wlr_output_is_headless(target->wlr_output)))
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
static bool save_workspace_state(void) {
    const char *path = getenv("AIR_SPATIAL_WORKSPACE_STATE");
    if (!path || path[0] != '/')
        return false;
    char *temporary;
    if (asprintf(&temporary, "%s.XXXXXX", path) < 0)
        return false;
    int fd = mkstemp(temporary);
    bool saved = false;
    if (fd >= 0) {
        FILE *file = fdopen(fd, "wb");
        if (file) {
            bool written = fwrite(&xr.workspace_state, sizeof(xr.workspace_state), 1, file) == 1 &&
                           fflush(file) == 0 && fsync(fd) == 0;
            saved = fclose(file) == 0 && written && rename(temporary, path) == 0;
        } else {
            close(fd);
        }
        if (!saved)
            unlink(temporary);
    }
    free(temporary);
    return saved;
}

static void load_workspace_state(void) {
    const char *path = getenv("AIR_SPATIAL_WORKSPACE_STATE");
    FILE *file = path && path[0] == '/' ? fopen(path, "rb") : NULL;
    if (!file)
        return;
    bool valid = fread(&xr.workspace_state, sizeof(xr.workspace_state), 1, file) == 1 &&
                 fgetc(file) == EOF && !ferror(file) &&
                 xr.workspace_state.magic == AIR_LAYOUT_MAGIC &&
                 xr.workspace_state.count <= AIR_LAYOUT_WINDOWS &&
                 xr.workspace_state.reading <= 1;
    fclose(file);
    if (!valid) {
        xr.workspace_state = (struct air_workspace_state){.magic = AIR_LAYOUT_MAGIC};
        return;
    }
    for (unsigned i = 0; i < xr.workspace_state.count; i++) {
        struct air_saved_window *saved = &xr.workspace_state.windows[i];
        if (!memchr(saved->workspace, 0, sizeof(saved->workspace)) ||
            !memchr(saved->app, 0, sizeof(saved->app)) ||
            !memchr(saved->title, 0, sizeof(saved->title)) ||
            !aplacement_valid(saved->placement)) {
            xr.workspace_state = (struct air_workspace_state){.magic = AIR_LAYOUT_MAGIC};
            return;
        }
    }
}

static const char *window_workspace(struct view *view) {
    return view->workspace && view->workspace->name ? view->workspace->name : "";
}

static struct air_placement placement_at(int i) {
    return (struct air_placement){
        .angle = xr.poses[i].angle, .height = xr.poses[i].height,
        .distance = xr.poses[i].distance, .scale = xr.poses[i].scale, .pinned = xr.poses[i].pinned,
        .client_width = xr.poses[i].spatial_width, .client_height = xr.poses[i].spatial_height,
    };
}

static void place_at(int i, struct air_placement p) {
    xr.poses[i].angle = p.angle;
    xr.poses[i].height = p.height;
    xr.poses[i].distance = p.distance;
    xr.poses[i].scale = p.scale;
    xr.poses[i].pinned = p.pinned;
    xr.poses[i].spatial_width = p.client_width;
    xr.poses[i].spatial_height = p.client_height;
    xr.poses[i].sized_generation = 0;
}

static bool restore_window_placement(struct view *view, int pose) {
    if (!view->app_id || !view->app_id[0] || (view->impl->get_parent && view->impl->get_parent(view)))
        return false;
    const char *title = view->title ? view->title : "";
    int exact = -1, fallback = -1, matches = 0;
    for (unsigned i = 0; i < xr.workspace_state.count; i++) {
        struct air_saved_window *saved = &xr.workspace_state.windows[i];
        if (strcmp(saved->workspace, window_workspace(view)) || strcmp(saved->app, view->app_id))
            continue;
        matches++;
        if (xr.restored[i])
            continue;
        fallback = i;
        if (exact < 0 && !strcmp(saved->title, title))
            exact = i;
    }
    int slot = exact >= 0 ? exact : matches == 1 ? fallback : -1;
    if (slot < 0)
        return false;
    place_at(pose, xr.workspace_state.windows[slot].placement);
    xr.restored[slot] = true;
    return true;
}

static int existing_pose(struct view *view) {
    for (int i = 0; i < xr.poses_count; i++) {
        if (xr.poses[i].id == view->creation_id && xr.poses[i].workspace == view->workspace)
            return i;
    }
    return -1;
}

static int pose_for(struct view *v, int index) {
    int existing = existing_pose(v);
    if (existing >= 0)
        return existing;
    if (xr.poses_count == 512)
        return -1;
    int i = xr.poses_count++;
    xr.poses[i].id = v->creation_id;
    xr.poses[i].workspace = v->workspace;
    xr.poses[i].angle = awrap(index == 0 ? 0 : ((index + 1) / 2) * .98 * (index % 2 ? -1 : 1));
    xr.poses[i].height = 0;
    xr.poses[i].distance = AIR_RADIUS;
    xr.poses[i].depth_offset = 0;
    xr.poses[i].reference_width = v->current.width;
    xr.poses[i].scale = 1;
    restore_window_placement(v, i);
    return i;
}
/* Resolve root to leaf, so a nested dialog follows its immediate parent's
 * current placement and scale, including any independent dialog movement. */
static bool compose_panel_pose(struct view *view, int index, struct air_panel *panel,
                               struct air_panel *anchor) {
    struct view *chain[16];
    int count = 0;
    for (struct view *v = view; v; v = v->impl->get_parent ? v->impl->get_parent(v) : NULL) {
        if (count == (int)(sizeof(chain) / sizeof(chain[0])))
            return false;
        chain[count++] = v;
    }

    struct air_panel parent = {0};
    for (int level = count - 1; level >= 0; level--) {
        struct view *v = chain[level];
        bool attached = level != count - 1;
        int pose = pose_for(v, attached ? 0 : index);
        if (pose < 0)
            return false;
        struct air_panel placed = {.width = v->current.width, .height = v->current.height};
        if (attached) {
            struct view *parent_view = chain[level + 1];
            double x = v->current.x + placed.width * .5 -
                       parent_view->current.x - parent.width * .5;
            double y = v->current.y + placed.height * .5 -
                       parent_view->current.y - parent.height * .5;
            atransient_anchor(&placed, &parent, x, y);
        } else {
            placed.distance = xr.poses[pose].distance;
            if (xr.poses[pose].reference_width <= 0)
                xr.poses[pose].reference_width = placed.width;
            placed.metres_per_pixel = panel_scale * xr.poses[pose].scale *
                (xr.workspace_state.reading ? 1.35 : 1) *
                fmin(.0009, 1.3 / fmax(1, xr.poses[pose].reference_width));
            placed.metres_per_pixel_y = placed.metres_per_pixel;
        }
        if (level == 0 && anchor)
            *anchor = placed;
        placed.azimuth = awrap(placed.azimuth + xr.poses[pose].angle);
        placed.elevation += xr.poses[pose].height;
        if (attached)
            placed.distance = fmax(fmin(.30, placed.distance),
                                   placed.distance + xr.poses[pose].depth_offset);
        parent = placed;
    }
    panel->width = parent.width;
    panel->height = parent.height;
    panel->azimuth = parent.azimuth;
    panel->elevation = parent.elevation;
    panel->distance = parent.distance;
    panel->tangent = 0;
    panel->metres_per_pixel = parent.metres_per_pixel;
    panel->metres_per_pixel_y = parent.metres_per_pixel_y;
    return true;
}
static struct view *root_view(struct view *view);

static bool scene_update(void) {
    struct entry next[AIR_MAX_PANELS] = {0};
    int n = 0;
    struct view *v;
    wl_list_for_each(v, &server.views, link) {
        if (!eligible(v) || (xr.game_id && root_view(v) != find_view(xr.game_id)))
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
        snprintf(p->title, sizeof(p->title), "%s", v->title ? v->title : "Window");
        p->width = v->current.width;
        p->height = v->current.height;
        if (p->width <= 0 || p->height <= 0)
            continue;
        struct wlr_box box = {0};
        wlr_scene_node_for_each_buffer(&v->capture.scene->tree.node, bbox, &box);
        if (box.width <= 0 || box.height <= 0)
            continue;
        p->bx = box.x;
        p->by = box.y;
        p->bw = box.width;
        p->bh = box.height;
        if (!compose_panel_pose(v, n, p, NULL)) {
            air_spatial_stop("spatial pose capacity or transient nesting exceeded");
            return false;
        }
        struct view *root = root_view(v);
        int pose = root ? pose_for(root, n) : -1;
        if (v == server.active_view)
            p->flags |= AIR_PANEL_FOCUSED;
        if (pose >= 0 && xr.poses[pose].pinned)
            p->flags |= AIR_PANEL_PINNED;
        if (xr.manipulation.region == AIR_TITLE && xr.manipulation.view_id == p->id &&
            v == root &&
            asnap_target(p->azimuth, xr.manipulation.plane.azimuth,
                         xr.manipulation.snap_heading, &p->snap_azimuth))
            p->flags |= AIR_PANEL_SNAP;
        n++;
    }
    struct air_widget *widget;
    wl_list_for_each(widget, air_widgets_list(), link) {
        if ((xr.game_id && !xr.dock_until) || n == AIR_MAX_PANELS || !widget->layer->surface->mapped)
            continue;
        struct air_panel *p = &next[n].panel;
        p->id = widget->id;
        p->flags = AIR_PANEL_WIDGET | (air_widget_has_popup(widget) ? AIR_PANEL_POPUP : 0);
        snprintf(p->identifier, sizeof(p->identifier), "air-widget-%llu", (unsigned long long)p->id);
        snprintf(p->title, sizeof(p->title), "%s", widget->role == 0 ? "Applications" :
                 widget->role == 1 ? "Status" : "System");
        p->width = widget->layer->surface->current.width;
        p->height = widget->layer->surface->current.height;
        struct wlr_box box = {0};
        wlr_scene_node_for_each_buffer(&widget->capture->tree.node, bbox, &box);
        if (p->width <= 0 || p->height <= 0 || box.width <= 0 || box.height <= 0)
            continue;
        p->bx = box.x; p->by = box.y; p->bw = box.width; p->bh = box.height;
        awidget_pose(p, widget->role, xr.widget_yaw);
        p->elevation += xr.dock_offset;
        if (widget->reported_width != p->bw || widget->reported_height != p->bh) {
            widget->reported_width = p->bw; widget->reported_height = p->bh;
            wlr_log(WLR_INFO, "spatial widget geometry role=%d width=%d height=%d bounds=%d,%d,%d,%d",
                    widget->role, p->width, p->height, p->bx, p->by, p->bw, p->bh);
        }
        n++;
    }
    bool changed = n != xr.count;
    for (int i = 0; i < n && !changed; i++)
        changed = memcmp(&next[i].panel, &xr.scene[i].panel, sizeof(struct air_panel)) != 0;
    if (!changed)
        return false;
    xr.scene_head_only = n == xr.count;
    for (int i = 0; i < n && xr.scene_head_only; i++) {
        struct air_panel old = xr.scene[i].panel;
        if (old.flags & AIR_PANEL_WIDGET)
            old.azimuth = next[i].panel.azimuth;
        xr.scene_head_only = !memcmp(&old, &next[i].panel, sizeof(old));
    }
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
/* Wayland constraints are expressed in the target surface's logical coordinates. */
static struct entry *constraint_panel(struct wlr_pointer_constraint_v1 *constraint) {
    for (int i = 0; constraint && i < xr.count; i++) {
        struct entry *entry = &xr.scene[i];
        if (eligible(entry->view) && entry->view->surface == constraint->surface)
            return entry;
    }
    return NULL;
}

bool air_spatial_can_constrain(struct wlr_pointer_constraint_v1 *constraint) {
    if (!constraint || xr.overview || server.seat.drag.active ||
        server.seat.wlr_seat->keyboard_state.focused_surface != constraint->surface)
        return false;
    struct view *view;
    wl_list_for_each(view, &server.views, link) {
        if (eligible(view) && view->surface == constraint->surface)
            return !xr.game_id || root_view(view) == find_view(xr.game_id);
    }
    return false;
}

bool air_spatial_relative_allowed(void) {
    if (!air_spatial_active())
        return true;
    struct wlr_pointer_constraint_v1 *constraint = server.seat.current_constraint;
    return !xr.overview && !xr.watchdog.paused && !xr.manipulation.region &&
           !server.seat.drag.active && xr.ack == xr.epoch &&
           now_ns() - xr.pose_stamp < 250000000 &&
           (!constraint || constraint->surface ==
                           server.seat.wlr_seat->pointer_state.focused_surface);
}

static void point_pointer_at(struct air_vec point) {
    xr.theta = atan2(point.x, point.z);
    xr.height = aclamp(AIR_RADIUS * point.y / fmax(.001, hypot(point.x, point.z)), -1.2, 1.2);
}

struct constraint_origin {
    struct wlr_surface *surface;
    struct wlr_scene_node *node;
    int x, y;
};

static void constraint_surface_origin(struct wlr_scene_buffer *buffer, int x, int y, void *data) {
    struct constraint_origin *origin = data;
    if (lab_wlr_surface_from_node(&buffer->node) == origin->surface) {
        origin->node = &buffer->node;
        origin->x = x;
        origin->y = y;
    }
}

void air_spatial_constraint_hint(struct wlr_pointer_constraint_v1 *constraint,
                                 double sx, double sy) {
    struct entry *entry = constraint_panel(constraint);
    if (!entry || !isfinite(sx) || !isfinite(sy))
        return;
    struct constraint_origin origin = {.surface = constraint->surface};
    wlr_scene_node_for_each_buffer(&entry->view->capture.scene->tree.node,
                                   constraint_surface_origin, &origin);
    if (!origin.node)
        return;
    point_pointer_at(apoint(&entry->panel, sx + origin.x, sy + origin.y));
    if (server.seat.wlr_seat->pointer_state.focused_surface == constraint->surface)
        wlr_seat_pointer_warp(server.seat.wlr_seat, sx, sy);
}

static bool constraint_context(struct cursor_context *ctx, struct air_vec ray, struct air_vec *point) {
    struct wlr_pointer_constraint_v1 *constraint = server.seat.current_constraint;
    struct entry *entry = constraint_panel(constraint);
    if (!entry)
        return false;
    struct constraint_origin origin = {.surface = constraint->surface};
    wlr_scene_node_for_each_buffer(&entry->view->capture.scene->tree.node,
                                   constraint_surface_origin, &origin);
    if (!origin.node)
        return false;
    struct wlr_seat_pointer_state *pointer = &server.seat.wlr_seat->pointer_state;
    double x = entry->panel.width * .5 - origin.x, y = entry->panel.height * .5 - origin.y;
    if (pointer->focused_surface == constraint->surface) {
        x = pointer->sx;
        y = pointer->sy;
    }
    if (constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
        if (!pixman_region32_contains_point(&constraint->region, floor(x), floor(y), NULL)) {
            int count;
            const pixman_box32_t *boxes = pixman_region32_rectangles(&constraint->region, &count);
            double best = HUGE_VAL;
            double nearest_x = x, nearest_y = y;
            for (int i = 0; i < count; i++) {
                double cx = aclamp(x, boxes[i].x1, boxes[i].x2 - .001);
                double cy = aclamp(y, boxes[i].y1, boxes[i].y2 - .001);
                double distance = hypot(cx - x, cy - y);
                if (distance < best) {
                    best = distance;
                    nearest_x = cx;
                    nearest_y = cy;
                }
            }
            x = nearest_x;
            y = nearest_y;
        }
        double sx, sy, distance, cx, cy;
        if (aplane(&entry->panel, ray, &sx, &sy, &distance) &&
            wlr_region_confine(&constraint->region, x, y, sx - origin.x, sy - origin.y, &cx, &cy)) {
            x = cx;
            y = cy;
        }
    }
    *point = apoint(&entry->panel, x + origin.x, y + origin.y);
    *ctx = (struct cursor_context){.type = LAB_NODE_CLIENT, .view = entry->view,
        .node = origin.node, .surface = constraint->surface, .sx = x, .sy = y};
    return true;
}

static bool hit_context(struct cursor_context *ctx, struct air_vec *point) {
    struct air_vec d = aray(xr.theta, xr.height);
    double best = 1e30;
    int best_priority = -1;
    bool hit = false;
    xr.hover_id = 0;
    xr.hover_region = 0;
    *ctx = (struct cursor_context){.type = LAB_NODE_ROOT};
    *point = acylinder_point(xr.theta, xr.height);
    if (constraint_context(ctx, d, point))
        return true;
    bool grab = server.seat.pressed.ctx.surface &&
                !wlr_seat_pointer_has_grab(server.seat.wlr_seat) && !server.seat.drag.active;
    for (int i = 0; i < xr.count; i++) {
        struct entry *e = &xr.scene[i];
        struct air_widget *widget = (e->panel.flags & AIR_PANEL_WIDGET) ? air_widget_find(e->panel.id) : NULL;
        if (!eligible(e->view) && !widget)
            continue;
        double x, y, t;
        if (!aplane(&e->panel, d, &x, &y, &t))
            continue;
        if (grab) {
            struct cursor_context *pressed = &server.seat.pressed.ctx;
            struct view *root = pressed->view;
            if (widget ? air_widget_for_surface(pressed->surface) != widget : root != e->view)
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
        int priority = (e->panel.flags & AIR_PANEL_POPUP) ? 1 : 0;
        if (priority < best_priority || (priority == best_priority && t >= best - 1e-6))
            continue;
        double sx, sy;
        struct wlr_scene_node *node = wlr_scene_node_at(
            widget ? &widget->capture->tree.node : &e->view->capture.scene->tree.node,
            x, y, &sx, &sy);
        struct wlr_surface *surface = node ? lab_wlr_surface_from_node(node) : NULL;
        uint32_t region = widget ? 0 : aframe_region(&e->panel, x, y);
        if (region && !surface) {
            /* Popups extending over the spatial frame retain their own input.
             * The frame itself has no client surface and never receives seat buttons. */
            *ctx = (struct cursor_context){.type = LAB_NODE_ROOT};
            xr.hover_id = e->panel.id;
            xr.hover_region = region;
            best = t;
            best_priority = priority;
            *point = ascale(d, t);
            hit = true;
            continue;
        }
        if (!surface)
            continue; /* Input holes allow a farther panel to win. */
        if (server.seat.drag.active &&
            !(xr.stereo ? astereo_visible_center_zoom(ascale(d, t), xr.basis, xr.aspect,
                                                     xr.ipd_mm, xr.ipd_center_mm, xr.zoom)
                        : avisible_zoom(ascale(d, t), xr.basis, xr.aspect, xr.zoom)))
            continue;
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
                                       .type = widget ? LAB_NODE_LAYER_SURFACE : LAB_NODE_CLIENT,
                                       .sx = sx,
                                       .sy = sy};
        best = t;
        best_priority = priority;
        xr.hover_id = e->panel.id;
        xr.hover_region = 0;
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
static bool widget_follow_allowed(void) {
    return xr.mode==SPATIAL && !xr.widget_hold && !xr.native_left_down &&
        !server.seat.wlr_seat->pointer_state.button_count &&
        !wlr_seat_pointer_has_grab(server.seat.wlr_seat);
}
static void pointer_packet(void) {
    if(xr.watchdog.paused)return;
    struct cursor_context ctx;
    struct air_vec point;
    bool hit = hit_context(&ctx, &point);
    if (xr.overview) {
        point = acylinder_point(xr.theta, xr.height);
        xr.hover_id = 0;
        xr.hover_region = 0;
        hit = false;
    }
    if (xr.manipulation.region) {
        for (int i = 0; i < xr.count; i++) {
            if (xr.scene[i].panel.id != xr.manipulation.view_id)
                continue;
            double x, y, t;
            struct air_vec ray = aray(xr.theta, xr.height);
            if (aplane(&xr.scene[i].panel, ray, &x, &y, &t)) {
                point = ascale(ray, t);
                hit = true;
            }
            break;
        }
    }
    struct air_packet m = {.type = AIR_POINTER};
    m.data.pointer.xyz[0] = point.x;
    m.data.pointer.xyz[1] = point.y;
    m.data.pointer.xyz[2] = point.z;
    m.data.pointer.hit = hit;
    m.data.pointer.active = xr.mode == SPATIAL;
    m.data.pointer.hide_cursor = server.seat.current_constraint &&
        server.seat.current_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED;
    m.data.pointer.widget_follow=widget_follow_allowed();
    m.data.pointer.widget_yaw=xr.widget_yaw;
    m.data.pointer.view_id = xr.manipulation.region ? xr.manipulation.view_id : xr.hover_id;
    m.data.pointer.region = xr.manipulation.region ? xr.manipulation.region : xr.hover_region;
    if (xr.mode == SPATIAL && xr.cursor_region != m.data.pointer.region) {
        xr.cursor_region = m.data.pointer.region;
        air_spatial_cursor_name("default");
    }
    if (!send_packet(&m, -1))
        air_spatial_stop("presentation pointer channel stalled");
}
bool air_spatial_context(struct cursor_context *ctx) {
    if (!air_spatial_active())
        return false;
    if (xr.overview || xr.manipulation.region) {
        *ctx = (struct cursor_context){.type = LAB_NODE_ROOT};
        return true;
    }
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
bool air_spatial_manipulating(void) { return air_spatial_active() && xr.manipulation.region != 0; }
static struct view *root_view(struct view *view) {
    for (int depth = 0; view && depth < 16; depth++) {
        struct view *parent = view->impl->get_parent ? view->impl->get_parent(view) : NULL;
        if (!parent)
            return view;
        view = parent;
    }
    return NULL;
}
static bool workspace_controls_allowed(void) {
    return air_spatial_active() && !xr.watchdog.paused && !xr.calibration &&
           !xr.native_left_down && !server.seat.drag.active && !server.seat.current_constraint &&
           !server.seat.wlr_seat->pointer_state.button_count &&
           !wlr_seat_pointer_has_grab(server.seat.wlr_seat) &&
           server.input_mode == LAB_INPUT_STATE_PASSTHROUGH;
}

static double forward_yaw(void) {
    return atan2(xr.basis[6], xr.basis[8]);
}

static double forward_height(double distance) {
    return aclamp(distance * xr.basis[7] / fmax(.001, hypot(xr.basis[6], xr.basis[8])), -1.2, 1.2);
}

static void refresh_workspace_scene(void) {
    scene_update();
    if (air_spatial_active()) {
        cursor_update_focus();
        pointer_packet();
    }
}

static void clear_game_focus(void) {
    if (!xr.game_id)
        return;
    for (int i = 0; i < xr.poses_count; i++) {
        if (xr.poses[i].id == xr.game_id && xr.poses[i].workspace == xr.game_workspace) {
            xr.poses[i].angle = xr.game_angle;
            xr.poses[i].height = xr.game_height;
            xr.poses[i].scale = xr.game_scale;
            break;
        }
    }
    xr.game_id = 0;
    xr.game_workspace = NULL;
}

static void center_window(struct view *view) {
    if (!view || (!view->mapped && !view->minimized))
        return;
    if (!view->visible_on_all_workspaces && view->workspace != server.workspaces.current)
        workspaces_switch_to(view->workspace, false);
    struct view *root = root_view(view);
    if (!root)
        return;
    if (xr.game_id && xr.game_id != root->creation_id)
        clear_game_focus();
    int i = pose_for(root, 0);
    if (i < 0)
        return;
    xr.poses[i].angle = forward_yaw();
    xr.poses[i].height = forward_height(xr.poses[i].distance);
    if (view->minimized)
        view_minimize(view, false);
    if (view->mapped)
        desktop_focus_view(view, true);
    refresh_workspace_scene();
}

static void apply_workspace_layout(enum air_layout layout) {
    clear_game_focus();
    struct view *focused = root_view(server.active_view);
    int count = 0;
    struct view *view;
    wl_list_for_each(view, &server.views, link) {
        if (eligible(view) && view == root_view(view))
            count++;
    }
    int rank = eligible(focused) ? 1 : 0;
    wl_list_for_each(view, &server.views, link) {
        if (!eligible(view) || view != root_view(view))
            continue;
        int i = pose_for(view, rank);
        if (i < 0 || xr.poses[i].pinned)
            continue;
        int slot = view == focused ? 0 : rank++;
        struct air_placement placement = alayout_slot(layout, slot, count,
                                                       forward_yaw(), forward_height(AIR_RADIUS));
        if (layout != AIR_LAYOUT_FOCUS && count > 1 && slot < 2) {
            int reference = xr.poses[i].reference_width > 0 ? xr.poses[i].reference_width : view->current.width;
            double width = fmax(1, view->current.width) *
                fmin(.0009, 1.3 / fmax(1, reference)) * panel_scale *
                (xr.workspace_state.reading ? 1.35 : 1);
            double available = layout == AIR_LAYOUT_PAIR ? .42 : slot == 0 ? .45 : .30;
            placement.scale = aclamp(available / width, .35, placement.scale);
        }
        placement.client_width = xr.poses[i].spatial_width;
        placement.client_height = xr.poses[i].spatial_height;
        place_at(i, placement);
    }
    refresh_workspace_scene();
}

static void save_layout(void) {
    struct air_workspace_state saved = {.magic = AIR_LAYOUT_MAGIC,
                                        .reading = xr.workspace_state.reading};
    struct view *view;
    wl_list_for_each(view, &server.views, link) {
        if (!view->mapped || view != root_view(view) || !view->app_id || !view->app_id[0] ||
            saved.count == AIR_LAYOUT_WINDOWS)
            continue;
        int i = existing_pose(view);
        if (i < 0)
            continue;
        struct air_saved_window *window = &saved.windows[saved.count++];
        snprintf(window->workspace, sizeof(window->workspace), "%s", window_workspace(view));
        snprintf(window->app, sizeof(window->app), "%s", view->app_id);
        snprintf(window->title, sizeof(window->title), "%s", view->title ? view->title : "");
        window->placement = placement_at(i);
        if (view->creation_id == xr.game_id && view->workspace == xr.game_workspace) {
            window->placement.angle = xr.game_angle;
            window->placement.height = xr.game_height;
            window->placement.scale = xr.game_scale;
        }
    }
    xr.workspace_state = saved;
    memset(xr.restored, 1, sizeof(xr.restored));
    status(save_workspace_state() ? "Saved spatial window layout" : "Could not save spatial window layout");
}

static void recall_layout(void) {
    clear_game_focus();
    memset(xr.restored, 0, sizeof(xr.restored));
    /* Match the current scene first, then reserve entries for the other open workspaces. */
    for (int pass = 0; pass < 2; pass++) {
        struct view *view;
        wl_list_for_each(view, &server.views, link) {
            if (!view->mapped || (view->workspace == server.workspaces.current) != (pass == 0))
                continue;
            int i = existing_pose(view);
            if (i < 0)
                pose_for(view, 0);
            else
                restore_window_placement(view, i);
        }
    }
    refresh_workspace_scene();
}

static bool overview_matches(struct view *view) {
    const char *query = xr.overview_query;
    return !query[0] || (view->title && strcasestr(view->title, query)) ||
           (view->app_id && strcasestr(view->app_id, query)) ||
           strcasestr(window_workspace(view), query);
}

static struct view *overview_view(uint64_t id) {
    struct view *view;
    wl_list_for_each(view, &server.views, link) {
        if ((view->mapped || view->minimized) && view->creation_id == id)
            return view;
    }
    return NULL;
}

static void overview_send(void) {
    unsigned start = xr.overview_selected / AIR_OVERVIEW_ROWS * AIR_OVERVIEW_ROWS;
    struct air_packet packet = {.type = AIR_OVERVIEW};
    packet.data.overview.visible = xr.overview;
    packet.data.overview.revision = ++xr.overview_revision;
    packet.data.overview.selected = xr.overview_selected;
    packet.data.overview.start = start;
    packet.data.overview.count = xr.overview_count;
    snprintf(packet.data.overview.query, sizeof(packet.data.overview.query), "%s", xr.overview_query);
    if (!send_packet(&packet, -1))
        goto failed;
    for (unsigned i = start; xr.overview && i < xr.overview_count && i < start + AIR_OVERVIEW_ROWS; i++) {
        struct view *view = overview_view(xr.overview_ids[i]);
        packet = (struct air_packet){.type = AIR_OVERVIEW_ROW};
        packet.data.row.index = i - start;
        snprintf(packet.data.row.title, sizeof(packet.data.row.title), "%s",
                 view && view->title && *view->title ? view->title : "Window");
        if (view) {
            snprintf(packet.data.row.workspace, sizeof(packet.data.row.workspace), "%s",
                     view->minimized ? "Minimized" : window_workspace(view));
            packet.data.row.visible = xr.overview_in_view[i];
        }
        if (!send_packet(&packet, -1))
            goto failed;
    }
    packet = (struct air_packet){.type = AIR_OVERVIEW_COMMIT};
    if (send_packet(&packet, -1))
        return;
failed:
    air_spatial_stop("window overview presentation channel stalled");
}

static void overview_refresh(bool force) {
    uint64_t ids[AIR_LAYOUT_WINDOWS];
    bool visible[AIR_LAYOUT_WINDOWS] = {0};
    unsigned count = 0;
    struct view *view;
    wl_list_for_each(view, &server.views, link) {
        if ((!view->mapped && !view->minimized) || !view->surface || !overview_matches(view) ||
            count == AIR_LAYOUT_WINDOWS)
            continue;
        ids[count] = view->creation_id;
        for (int i = 0; i < xr.count; i++) {
            const struct air_panel *p = &xr.scene[i].panel;
            if (p->id == view->creation_id) {
                visible[count] = apanel_intersects_view(p, xr.basis, xr.aspect, xr.zoom, 0);
                break;
            }
        }
        count++;
    }
    if (!force && count == xr.overview_count &&
        !memcmp(ids, xr.overview_ids, count * sizeof(ids[0])) &&
        !memcmp(visible, xr.overview_in_view, count * sizeof(visible[0])))
        return;
    memcpy(xr.overview_ids, ids, count * sizeof(ids[0]));
    memcpy(xr.overview_in_view, visible, count * sizeof(visible[0]));
    xr.overview_count = count;
    xr.overview_selected = count ? MIN(count - 1, xr.overview_selected) : 0;
    overview_send();
}

static void overview_close(void) {
    xr.overview = false;
    overview_send();
    cursor_update_focus();
}

static void overview_activate(unsigned index) {
    struct view *view = index < xr.overview_count ? overview_view(xr.overview_ids[index]) : NULL;
    overview_close();
    if (air_spatial_active())
        center_window(view);
}

static void overview_move(int delta) {
    if (!xr.overview_count)
        return;
    xr.overview_selected = ((int)xr.overview_selected + delta + (int)xr.overview_count) %
                           (int)xr.overview_count;
    overview_send();
}

static void overview_click(void) {
    uint64_t now = now_ns();
    if (xr.overview_presented != xr.overview_revision || now - xr.overview_stamp >= 250000000 ||
        now - xr.pose_stamp >= 250000000)
        return;
    struct air_vec ray = aray(xr.theta, xr.height);
    double z = adot(ray, abaxis(xr.overview_basis, 2));
    if (z <= 0)
        return;
    double x = adot(ray, abaxis(xr.overview_basis, 0)) / z;
    double y = adot(ray, abaxis(xr.overview_basis, 1)) / z;
    unsigned start = xr.overview_selected / AIR_OVERVIEW_ROWS * AIR_OVERVIEW_ROWS;
    unsigned rows = MIN(AIR_OVERVIEW_ROWS, xr.overview_count - start);
    int row = aoverview_row(x, y, rows);
    if (row >= 0) {
        unsigned index = start + row;
        if (index < xr.overview_count)
            overview_activate(index);
    }
}

static void overview_key(struct wlr_keyboard *kb, uint32_t key) {
    if (key == KEY_ESC) {
        overview_close();
        return;
    }
    if (key == KEY_ENTER || key == KEY_KPENTER) {
        if (xr.overview_count)
            overview_activate(xr.overview_selected);
        return;
    }
    if (key == KEY_UP || key == KEY_DOWN || key == KEY_TAB) {
        overview_move(key == KEY_UP ? -1 : 1);
        return;
    }
    size_t length = strlen(xr.overview_query);
    if (key == KEY_BACKSPACE && length) {
        do {
            length--;
        } while (length && ((unsigned char)xr.overview_query[length] & 0xc0) == 0x80);
        xr.overview_query[length] = 0;
    } else {
        uint32_t modifiers = wlr_keyboard_get_modifiers(kb);
        if (!kb->xkb_state || modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO))
            return;
        char text[16];
        int size = xkb_state_key_get_utf8(kb->xkb_state, key + 8, text, sizeof(text));
        if (size <= 0 || size >= (int)sizeof(text) || (unsigned char)text[0] < 32 ||
            length + size >= sizeof(xr.overview_query))
            return;
        memcpy(xr.overview_query + length, text, size + 1);
    }
    xr.overview_selected = 0;
    overview_refresh(true);
}

static void summon_dock(void) {
    xr.dock_offset = forward_height(.6096) + .20;
    xr.dock_until = now_ns() + 8000000000ULL;
    refresh_workspace_scene();
}

static void toggle_game_focus(void) {
    if (xr.game_id) {
        clear_game_focus();
        refresh_workspace_scene();
        return;
    }
    struct view *view = root_view(server.active_view);
    if (!eligible(view))
        return;
    int i = pose_for(view, 0);
    if (i >= 0) {
        xr.game_angle = xr.poses[i].angle;
        xr.game_height = xr.poses[i].height;
        xr.game_scale = xr.poses[i].scale;
        double reference = fmax(1, xr.poses[i].reference_width);
        double mpp = panel_scale * (xr.workspace_state.reading ? 1.35 : 1) *
                     fmin(.0009, 1.3 / reference);
        double width = fmax(1, view->current.width + 2 * AIR_FRAME_BORDER) * mpp;
        double height = fmax(1, view->current.height + AIR_FRAME_TITLE + 2 * AIR_FRAME_BORDER) * mpp;
        double span = 1.70 * xr.poses[i].distance * tan(40 * AIR_PI / 360);
        xr.poses[i].scale = aclamp(fmin(span / width, span / (height * fmax(.1, xr.aspect))), .35, 2.5);
        xr.game_workspace = view->workspace;
        xr.game_id = view->creation_id;
        center_window(view);
    }
}

static bool spatial_can_resize(struct view *view) {
    return view_is_floating(view) && !view->shaded && view->impl->configure;
}

static bool spatial_resize_window(struct view *view) {
    if (!spatial_can_resize(view))
        return false;
    uint64_t now = now_ns();
    /* Keep only one live configure in flight. Release can supersede it with
     * the final pointer position, and the timer retries coalesced motion. */
    if (xr.native_left_down &&
        (xr.ack != xr.epoch || view->pending_configure_serial ||
         now - xr.manipulation.resize_sent_at < 33000000))
        return false;

    const struct air_panel *plane = &xr.manipulation.plane;
    double x, y, t;
    if (!aplane(plane, aray(xr.theta, xr.height), &x, &y, &t))
        return false;
    if (fabs(x - xr.manipulation.x) < 1e-6 && fabs(y - xr.manipulation.y) < 1e-6 &&
        xr.manipulation.requested.width == xr.manipulation.initial.width &&
        xr.manipulation.requested.height == xr.manipulation.initial.height)
        return false;
    struct view_size_hints hints = view_get_size_hints(view);
    int capture_width = AIR_MAX_CAPTURE_DIMENSION - MAX(0, plane->bw - plane->width);
    int capture_height = AIR_MAX_CAPTURE_DIMENSION - MAX(0, plane->bh - plane->height);
    struct air_resize_limits limits = {
        .min_width = hints.min_width > 0 ? hints.min_width :
            hints.base_width > 0 ? hints.base_width : LAB_MIN_VIEW_WIDTH,
        .min_height = hints.min_height > 0 ? hints.min_height :
            hints.base_height > 0 ? hints.base_height : LAB_MIN_VIEW_HEIGHT,
        .max_width = hints.max_width > 0 ? MIN(hints.max_width, capture_width) : capture_width,
        .max_height = hints.max_height > 0 ? MIN(hints.max_height, capture_height) : capture_height,
        .base_width = hints.base_width > 0 ? hints.base_width : MAX(0, hints.min_width),
        .base_height = hints.base_height > 0 ? hints.base_height : MAX(0, hints.min_height),
        .width_inc = hints.width_inc,
        .height_inc = hints.height_inc,
    };
    struct wlr_box initial = xr.manipulation.initial;
    struct wlr_box geometry = initial;
    uint32_t region = xr.manipulation.region;
    bool horizontal = region & (AIR_EDGE_LEFT | AIR_EDGE_RIGHT);
    bool vertical = region & (AIR_EDGE_TOP | AIR_EDGE_BOTTOM);
    double dx = x - xr.manipulation.x, dy = y - xr.manipulation.y;
    if (horizontal && vertical) {
        double scale = aresize_scale(plane->width * plane->metres_per_pixel,
                                     plane->height * apixel_y(plane),
                                     dx * plane->metres_per_pixel, dy * apixel_y(plane), region);
        if (!aresize_client_size(initial.width, initial.height, scale, limits,
                                 &geometry.width, &geometry.height))
            return false;
    } else if (horizontal) {
        double width = initial.width + 2 * (region & AIR_EDGE_RIGHT ? dx : -dx);
        if (!aresize_client_dimension(width, plane->metres_per_pixel, limits.min_width,
                                      limits.max_width, limits.base_width, limits.width_inc,
                                      &geometry.width))
            return false;
    } else if (vertical) {
        double height = initial.height + 2 * (region & AIR_EDGE_BOTTOM ? dy : -dy);
        if (!aresize_client_dimension(height, apixel_y(plane), limits.min_height,
                                      limits.max_height, limits.base_height, limits.height_inc,
                                      &geometry.height))
            return false;
    } else {
        return false;
    }
    struct wlr_box requested = xr.manipulation.requested;
    if (geometry.width == requested.width && geometry.height == requested.height)
        return false;
    geometry.x = initial.x + (initial.width - geometry.width) / 2;
    geometry.y = initial.y + (initial.height - geometry.height) / 2;
    xr.manipulation.requested = geometry;
    xr.manipulation.resize_sent_at = now;
    view_move_resize(view, geometry);
    return true;
}

static bool remember_desktop_window(struct view *view);

static void apply_spatial_sizes(void) {
    for (int i = 0; i < xr.poses_count; i++) {
        if (!xr.poses[i].spatial_width || xr.poses[i].sized_generation == xr.generation)
            continue;
        struct view *view = find_view(xr.poses[i].id);
        if (!eligible(view) || (xr.game_id && root_view(view) != find_view(xr.game_id)) ||
            view->workspace != xr.poses[i].workspace ||
            !spatial_can_resize(view) || view->pending_configure_serial)
            continue;
        xr.poses[i].sized_generation = xr.generation;
        struct wlr_box geometry = wlr_box_empty(&view->pending) ? view->current : view->pending;
        struct view_size_hints hints = view_get_size_hints(view);
        int w = MAX(xr.poses[i].spatial_width, hints.min_width);
        int h = MAX(xr.poses[i].spatial_height, hints.min_height);
        if (hints.max_width > 0) w = MIN(w, hints.max_width);
        if (hints.max_height > 0) h = MIN(h, hints.max_height);
        if ((w == geometry.width && h == geometry.height) || !remember_desktop_window(view))
            continue;
        geometry.x += (geometry.width - w) / 2;
        geometry.y += (geometry.height - h) / 2;
        geometry.width = w;
        geometry.height = h;
        view_move_resize(view, geometry);
    }
}

static void manipulation_update(void) {
    struct view *view = find_view(xr.manipulation.view_id);
    struct view *root = find_view(xr.manipulation.root_id);
    if (!eligible(view) || !eligible(root) || view->workspace != xr.manipulation.workspace) {
        xr.manipulation.region = 0;
        return;
    }
    if (now_ns() - xr.pose_stamp > 250000000)
        return;
    int i = pose_for(view, 0);
    if (i < 0 || root_view(view) != root)
        return;
    if (xr.manipulation.region == AIR_TITLE) {
        struct air_panel current = {0}, anchor;
        if (!compose_panel_pose(view, 0, &current, &anchor))
            return;
        double angle = current.azimuth, height = current.elevation;
        if (!agrab_position((xr.manipulation.x - current.width * .5) * current.metres_per_pixel,
                            (current.height * .5 - xr.manipulation.y) * apixel_y(&current),
                            current.distance, xr.theta, xr.height, &angle, &height))
            return;
        xr.poses[i].angle = awrap(angle - anchor.azimuth);
        xr.poses[i].height = aclamp(height - anchor.elevation, -1.2, 1.2);
    } else {
        if (!spatial_resize_window(view))
            return;
        xr.poses[i].spatial_width = xr.manipulation.requested.width;
        xr.poses[i].spatial_height = xr.manipulation.requested.height;
        xr.poses[i].sized_generation = xr.generation;
    }
    scene_update();
}
static void manipulation_log(const char *event) {
    struct view *view = find_view(xr.manipulation.view_id);
    struct view *root = find_view(xr.manipulation.root_id);
    if (!view || !root)
        return;
    struct air_panel current = {0};
    if (!compose_panel_pose(view, 0, &current, NULL))
        return;
    wlr_log(WLR_INFO,
            "spatial control %s id=%llu region=%u angle=%.6f height=%.6f "
            "distance=%.6f mpp=%.9f,%.9f desktop=%d,%d,%d,%d requested=%d,%d",
            event, (unsigned long long)view->creation_id, xr.manipulation.region,
            current.azimuth, current.elevation, current.distance,
            current.metres_per_pixel, apixel_y(&current), view->current.x, view->current.y,
            view->current.width, view->current.height, xr.manipulation.requested.width,
            xr.manipulation.requested.height);
}
void air_spatial_widget_removed(uint64_t id) {
    if (xr.widget_grab_id != id)
        return;
    xr.widget_grab_id = 0;
    struct wlr_seat *seat = server.seat.wlr_seat;
    /* The owning layer has gone. Clear pointer focus before releasing seat
     * state, so no unrelated client can receive its unmatched button-up. */
    wlr_seat_pointer_notify_clear_focus(seat);
    uint32_t buttons[WLR_POINTER_BUTTONS_CAP];
    size_t n = seat->pointer_state.button_count;
    if (n > WLR_POINTER_BUTTONS_CAP) n = WLR_POINTER_BUTTONS_CAP;
    for (size_t i = 0; i < n; i++) buttons[i] = seat->pointer_state.buttons[i].button;
    for (size_t i = 0; i < n; i++) {
        wlr_seat_pointer_notify_button(seat, now_ns() / 1000000, buttons[i], WL_POINTER_BUTTON_STATE_RELEASED);
        cursor_finish_button_release(&server.seat, buttons[i]);
        lab_set_add(&server.seat.bound_buttons, buttons[i]);
    }
    wlr_seat_pointer_notify_frame(seat);
    cursor_context_save(&server.seat.pressed, NULL);
}
bool air_spatial_button(uint32_t button, bool pressed) {
    if(air_spatial_active()){
        if(pressed)xr.widget_hold=true;
        else if(server.seat.wlr_seat->pointer_state.button_count<=1)xr.widget_hold=false;
        pointer_packet(); /* Freeze at the last displayed pose before routing a press. */
    }
    if (!pressed && air_spatial_active() && server.seat.drag.active) {
        struct wlr_seat *seat = server.seat.wlr_seat;
        struct cursor_context ctx;
        struct air_vec point;
        if (!air_spatial_press_allowed() || !hit_context(&ctx, &point) ||
            !seat->drag || seat->drag->focus != ctx.surface)
            wlr_seat_pointer_notify_clear_focus(seat);
    }
    if (!pressed && server.seat.wlr_seat->pointer_state.button_count <= 1)
        xr.widget_grab_id = 0;
    if (pressed && air_spatial_active() && (xr.hover_id >> 63) &&
        !xr.hover_region && air_spatial_press_allowed())
        xr.widget_grab_id = xr.hover_id;
    if (button == BTN_LEFT && !pressed && xr.native_left_down) {
        xr.native_left_down = false;
        if (xr.manipulation.region) {
            manipulation_update();
            struct view *view = find_view(xr.manipulation.view_id);
            if (xr.manipulation.region == AIR_TITLE && view && view == root_view(view)) {
                int i = pose_for(view, 0);
                double target;
                if (i >= 0 && asnap_target(xr.poses[i].angle, xr.manipulation.plane.azimuth,
                                          xr.manipulation.snap_heading, &target))
                    xr.poses[i].angle = target;
            }
            manipulation_log("end");
            xr.manipulation.region = 0;
            xr.cursor_region = UINT32_MAX;
            if (air_spatial_active()) {
                refresh_workspace_scene();
            }
        }
        return true; /* Also consumes the matching release after forced desktop. */
    }
    if (!air_spatial_active())
        return false;
    if (xr.overview) {
        if (button == BTN_LEFT && pressed) {
            xr.native_left_down = true;
            overview_click();
        } else if (pressed) {
            lab_set_add(&server.seat.bound_buttons, button);
        }
        return pressed;
    }
    if (xr.native_left_down) {
        if (pressed)
            lab_set_add(&server.seat.bound_buttons, button);
        return pressed;
    }
    if (!pressed || !air_spatial_press_allowed() || !xr.hover_region)
        return false;
    if (server.input_mode != LAB_INPUT_STATE_PASSTHROUGH)
        return false;
    if (button != BTN_LEFT) {
        lab_set_add(&server.seat.bound_buttons, button);
        return true;
    }
    if (server.seat.wlr_seat->pointer_state.button_count ||
        wlr_seat_pointer_has_grab(server.seat.wlr_seat))
        return false;
    if (xr.hover_region == AIR_FRAME) {
        xr.native_left_down = true;
        return true; /* Inert chrome must not start a flat desktop resize. */
    }
    struct view *view = find_view(xr.hover_id), *root = root_view(view);
    if (!view || !root)
        return false;
    if (xr.hover_region == AIR_CENTER || xr.hover_region == AIR_PIN || xr.hover_region == AIR_MINIMIZE) {
        xr.native_left_down = true;
        if (xr.hover_region == AIR_CENTER)
            center_window(view);
        else if (xr.hover_region == AIR_MINIMIZE) {
            view_minimize(view, true);
            refresh_workspace_scene();
        } else {
            int pose = pose_for(root, 0);
            if (pose >= 0)
                xr.poses[pose].pinned = !xr.poses[pose].pinned;
            refresh_workspace_scene();
        }
        return true;
    }
    for (int j = 0; j < xr.count; j++) {
        if (xr.scene[j].panel.id != xr.hover_id)
            continue;
        int i = pose_for(view, 0);
        if (i < 0)
            return false;
        xr.manipulation.view_id = view->creation_id;
        xr.manipulation.root_id = root->creation_id;
        xr.manipulation.workspace = view->workspace;
        xr.manipulation.region = xr.hover_region;
        if (xr.hover_region != AIR_TITLE && !spatial_can_resize(view)) {
            xr.manipulation.region = 0;
            xr.native_left_down = true;
            return true;
        }
        xr.manipulation.initial = view->current;
        xr.manipulation.requested = view->pending;
        if (xr.manipulation.requested.width <= 0 || xr.manipulation.requested.height <= 0)
            xr.manipulation.requested = view->current;
        xr.manipulation.resize_sent_at = 0;
        xr.manipulation.plane = xr.scene[j].panel;
        xr.manipulation.snap_heading = forward_yaw();
        double t;
        aplane(&xr.manipulation.plane, aray(xr.theta, xr.height), &xr.manipulation.x,
               &xr.manipulation.y, &t);
        xr.native_left_down = true;
        xr.cursor_region = UINT32_MAX;
        cursor_context_save(&server.seat.pressed, NULL);
        desktop_focus_view_or_surface(&server.seat, view, view->surface, true);
        manipulation_log("begin");
        scene_update();
        cursor_update_focus();
        pointer_packet();
        return true;
    }
    return false;
}
static void zoom_set(double factor) {
    if (factor == xr.zoom) return;
    xr.zoom = factor;
    if (xr.mode != SPATIAL) return;
    struct air_packet m = {.type = AIR_ZOOM};
    m.data.zoom.factor = factor;
    send_packet(&m, -1);
    xr.count = -1;
    scene_update(); /* Old-frustum presentations cannot authorize new clicks. */
}
void air_spatial_modifiers(uint32_t modifiers) {
    if ((modifiers & (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT)) !=
        (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT)) zoom_set(1);
}
static bool zoom_axis(uint32_t orientation, double delta) {
    uint32_t mods = keyboard_get_all_modifiers(&server.seat);
    if ((mods & (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT)) ==
        (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT)) {
        if (orientation == WL_POINTER_AXIS_VERTICAL_SCROLL && !xr.watchdog.paused &&
            !xr.calibration && !xr.manipulation.region && !server.seat.current_constraint &&
            !server.seat.wlr_seat->pointer_state.button_count &&
            now_ns() - xr.pose_stamp < 250000000)
            zoom_set(azoom_step(xr.zoom, delta));
        return true;
    }
    return false;
}
bool air_spatial_axis(uint32_t orientation, double delta) {
    if (!air_spatial_active())
        return false;
    if (xr.overview) {
        if (orientation == WL_POINTER_AXIS_VERTICAL_SCROLL && fabs(delta) >= 1)
            overview_move(delta > 0 ? 1 : -1);
        return true;
    }
    if (zoom_axis(orientation, delta)) return true;
    if (xr.manipulation.region) {
        if (xr.manipulation.region == AIR_TITLE && orientation == WL_POINTER_AXIS_VERTICAL_SCROLL &&
            isfinite(delta) && now_ns() - xr.pose_stamp < 250000000) {
            struct view *view = find_view(xr.manipulation.view_id);
            struct view *root = find_view(xr.manipulation.root_id);
            if (eligible(view) && eligible(root) && root_view(view) == root &&
                view->workspace == xr.manipulation.workspace) {
                int i = pose_for(view, 0);
                struct air_panel current = {0}, anchor;
                if (i >= 0 && compose_panel_pose(view, 0, &current, &anchor)) {
                    double distance = current.distance * exp(aclamp(delta, -100, 100) * .008);
                    if (view == root) {
                        xr.poses[i].distance = aclamp(distance, .45, 4);
                    } else {
                        double depth = aclamp(distance, fmin(.30, anchor.distance), anchor.distance);
                        xr.poses[i].depth_offset = depth - anchor.distance;
                    }
                    manipulation_update();
                    pointer_packet();
                }
            }
        }
        return true;
    }
    struct cursor_context ctx;
    struct air_vec point;
    hit_context(&ctx, &point);
    return xr.hover_region != 0;
}
bool air_spatial_motion(double dx, double dy) {
    if (!air_spatial_active())
        return false;
    if(xr.watchdog.paused)return true; /* Consume motion without hidden desktop input. */
    if (server.seat.current_constraint &&
        server.seat.current_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
        return true;
    scene_update();
    if (!air_spatial_active())
        return false;
    xr.pointer_pending = xr.ack != xr.epoch;
    amouse_move(&xr.theta, &xr.height, dx, dy, pointer_gain * pointer_speed / xr.zoom);
    if (server.seat.current_constraint) {
        struct air_vec point;
        struct cursor_context ctx;
        if (constraint_context(&ctx, aray(xr.theta, xr.height), &point))
            point_pointer_at(point);
    }
    if (xr.manipulation.region)
        manipulation_update();
    pointer_packet();
    return true;
}
bool air_spatial_press_allowed(void) {
    if (!air_spatial_active())
        return true;
    if(xr.watchdog.paused)return false;
    scene_update();
    if (!air_spatial_active())
        return false;
    struct cursor_context ctx;
    struct air_vec point;
    hit_context(&ctx, &point);
    bool locked_pointer = server.seat.current_constraint &&
                          server.seat.current_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED &&
                          ctx.surface == server.seat.current_constraint->surface;
    return xr.ready && xr.ack == xr.epoch && now_ns() - xr.heartbeat < 250000000 &&
           now_ns() - xr.pose_stamp < 250000000 && !xr.calibration &&
           (locked_pointer || (xr.stereo ? astereo_visible_center_zoom(point,xr.basis,xr.aspect,xr.ipd_mm,xr.ipd_center_mm,xr.zoom) :
                        avisible_zoom(point, xr.basis, xr.aspect, xr.zoom)));
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
    char output[128];
    aevent_quote(output, sizeof(output), name);
    aevent_write("compositor", "output_power",
        ",\"generation\":%llu,\"output\":%s,\"requested_on\":%s,\"actual_on\":%s,\"ok\":%s",
        (unsigned long long)xr.generation, output, aevent_bool(enabled),
        aevent_bool(o->wlr_output->enabled), aevent_bool(ok));
    if (!ok)
        wlr_log(WLR_ERROR, "spatial: output %s power %s deferred", name, enabled ? "on" : "off");
    return ok;
}
static bool remember_desktop_window(struct view *view) {
    struct desktop_window *saved;
    wl_array_for_each(saved, &xr.desktop_windows) {
        if (saved->id == view->creation_id)
            return true;
    }
    struct wlr_box geometry = wlr_box_empty(&view->pending) ? view->current : view->pending;
    if (wlr_box_empty(&geometry))
        return false;
    saved = wl_array_add(&xr.desktop_windows, sizeof(*saved));
    if (!saved)
        return false;
    *saved = (struct desktop_window){.id = view->creation_id, .geometry = geometry};
    return true;
}

static bool snapshot_desktop_windows(void) {
    wl_array_release(&xr.desktop_windows);
    wl_array_init(&xr.desktop_windows);
    struct view *view;
    wl_list_for_each(view, &server.views, link) {
        if (!view->mapped || !view_is_floating(view) || !view->impl->configure ||
            (wlr_box_empty(&view->pending) && wlr_box_empty(&view->current)))
            continue;
        if (!remember_desktop_window(view)) {
            wl_array_release(&xr.desktop_windows);
            wl_array_init(&xr.desktop_windows);
            return false;
        }
    }
    return true;
}

static void restore_desktop_windows(void) {
    struct output *target = find_output(xr.target_name);
    struct output *desktop = NULL;
    struct output *output;
    wl_list_for_each(output, &server.outputs, link) {
        if (output != target && !original_air(output) && output_is_usable(output)) {
            desktop = output;
            break;
        }
    }

    struct view *view, *next;
    wl_list_for_each_safe(view, next, &server.views, link) {
        if (!view->mapped || !view->impl->configure)
            continue;
        struct wlr_box geometry = wlr_box_empty(&view->pending) ? view->current : view->pending;
        bool restore = false;
        bool floating = view_is_floating(view);
        if (floating) {
            const struct desktop_window *saved;
            wl_array_for_each(saved, &xr.desktop_windows) {
                if (saved->id == view->creation_id) {
                    geometry = saved->geometry;
                    restore = true;
                    break;
                }
            }
        }

        struct output *destination = output_from_wlr_output(wlr_output_layout_output_at(
            server.output_layout, geometry.x + geometry.width * .5,
            geometry.y + geometry.height * .5));
        /* Floating windows follow their rectangle. Managed windows also
         * depend on their assigned output. */
        bool assigned_glasses = view->output && (view->output == target || original_air(view->output));
        bool on_glasses = (destination && (destination == target || original_air(destination))) ||
            ((!destination || !floating) && assigned_glasses);
        if (desktop && (on_glasses || (restore && !output_is_usable(destination)))) {
            if (!floating) {
                view_move_to_output(view, desktop);
                continue;
            }
            struct wlr_box area = output_usable_area_in_layout_coords(desktop);
            geometry.x = area.x + (area.width - geometry.width) / 2;
            geometry.y = area.y + (area.height - geometry.height) / 2;
            destination = desktop;
            restore = true;
        } else if (desktop && assigned_glasses && output_is_usable(destination)) {
            restore = true;
        }
        if (!restore)
            continue;
        bool reassign = output_is_usable(destination) && destination != view->output;
        if (reassign)
            view_set_output(view, destination);
        if (!wlr_box_equal(&geometry, &view->pending))
            view_move_resize(view, geometry);
        else if (reassign)
            view_save_last_placement(view);
    }
    wl_array_release(&xr.desktop_windows);
    wl_array_init(&xr.desktop_windows);
    xr.desktop_restore_pending = false;
    spatial_event("desktop_restored", "window placement recovery complete");
}

static int glasses_screen_state(void);
static void enter(void) {
    if (xr.mode == ARMING)
        scene_update();
    if (xr.mode != ARMING || !xr.ready || xr.ack != xr.epoch ||
        now_ns() - xr.pose_stamp > 250000000 || glasses_screen_state() != 1)
        return;
    if ((server.seat.current_constraint && !air_spatial_can_constrain(server.seat.current_constraint)) ||
        server.seat.drag.active ||
        server.seat.wlr_seat->pointer_state.button_count || xr.native_left_down) {
        status("Release buttons and pointer constraints before entry");
        return;
    }
    if (!snapshot_desktop_windows()) {
        air_spatial_stop("desktop window snapshot allocation failed");
        return;
    }
    xr.mode = SPATIAL;
    desktop_cancel_pending_auto_raise();
    wlr_cursor_unset_image(server.seat.cursor);
    status("SPATIAL | Super+Alt+X turns auto mode off");
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
    spatial_event("spatial_enter", "healthy tracking and scene");
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
static void cancel_spatial_drag(void) {
    struct wlr_seat *seat = server.seat.wlr_seat;
    if (seat->drag && seat->drag->source)
        wlr_data_source_destroy(seat->drag->source);
    else
        wlr_seat_pointer_end_grab(seat);
}

static void terminate_presenter(pid_t pid, struct presenter_child *child) {
    siginfo_t info = {0};
    int result;
    do {
        result = waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT);
    } while (result < 0 && errno == EINTR);
    bool running = result == 0 ? info.si_pid == 0 : errno != ECHILD;
    if (!running || kill(pid, SIGKILL) < 0)
        return;
    if (child)
        child->termination_requested = true;
    aevent_write("presenter", "stop_requested",
        ",\"generation\":%llu,\"child_pid\":%ld,\"signal\":%d",
        (unsigned long long)(child ? child->generation : xr.generation), (long)pid, SIGKILL);
}

static int presenter_exit_timeout(void *data) {
    struct presenter_child *child = data;
    wl_event_source_remove(child->exit_timer);
    child->exit_timer = NULL;
    terminate_presenter(child->pid, child);
    return 0;
}

static void stop_spatial(const char *reason, bool transport_closed) {
    xr.automatic.pending = false;
    xr.automatic.since = now_ns();
    if (xr.mode == DESKTOP || xr.mode == RESTORING)
        return;
    spatial_event("spatial_exit", reason);
    xr.desktop_restore_pending = xr.mode == SPATIAL;
    xr.mode = RESTORING;
    xr.zoom = 1;
    xr.calibration = false;
    xr.overview = false;
    clear_game_focus();
    xr.dock_until = 0;
    xr.dock_offset = 0;
    xr.manipulation.region = 0;
    xr.cursor_region = 0;
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
    /* Cancel a pending drop before synthesizing release during recovery. */
    if (server.seat.drag.active)
        cancel_spatial_drag();
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
        struct presenter_child *child = presenter_child_find(xr.pid);
        /* A dying presenter may close its sockets before waitid can see it.
         * Recover the desktop now and leave a short grace period for its status. */
        if (transport_closed && child)
            child->exit_timer = wl_event_loop_add_timer(server.wl_event_loop,
                                                       presenter_exit_timeout, child);
        if (child && child->exit_timer && wl_event_source_timer_update(child->exit_timer, 250) < 0) {
            wl_event_source_remove(child->exit_timer);
            child->exit_timer = NULL;
        }
        if (!child || !child->exit_timer)
            terminate_presenter(xr.pid, child);
        xr.pid = 0;
    }
    xr.widget_grab_id = 0;
    air_widgets_stop();
    xr.count = 0;
    xr.ready = false;
    xr.ack = 0;
    xr.pose_stamp = 0;
    xr.watchdog=(struct air_watchdog){0};
    xr.pointer_pending = false;
    xr.mode = DESKTOP;
    if (!xr.restore_pending)
        spatial_event("outputs_restored", reason);
    wlr_cursor_warp_closest(server.seat.cursor, NULL, xr.flat_x, xr.flat_y);
    if (strcmp(reason, "session locked")) {
        struct view *v = find_view(xr.saved_focus);
        if (v)
            desktop_focus_view(v, false);
    }
    cursor_update_image(&server.seat);
    cursor_update_focus();
}
void air_spatial_stop(const char *reason) {
    stop_spatial(reason, false);
}

static void client_destroyed(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
    wl_list_remove(&xr.client_destroy.link);
    xr.client = NULL;
    stop_spatial("presenter disconnected", true);
}
static void layer_destroyed(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
    wl_list_remove(&xr.layer_destroy.link);
    xr.layer = NULL;
    stop_spatial("presentation surface removed", true);
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
/* Adopt the widget pose from this exact displayed frame, without creating a
 * new scene epoch or synthesizing pointer/focus events from head movement. */
static void widget_presented_yaw(double yaw) {
    if(!isfinite(yaw) || fabs(yaw)>AIR_PI)return;
    xr.widget_yaw=yaw;
    for(int i=0;i<xr.count;i++)
        if(xr.scene[i].panel.flags & AIR_PANEL_WIDGET)xr.scene[i].panel.azimuth=yaw;
}
static int socket_read(int fd, uint32_t mask, void *data) {
    (void)data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        stop_spatial("helper pipe closed", true);
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
            double gain = amouse_gain(m.data.ready.width ? m.data.ready.width : 1920);
            double aspect =
                m.data.ready.height ? (double)m.data.ready.width / m.data.ready.height : 16. / 9;
            bool resized = xr.ready && (gain != pointer_gain || aspect != xr.aspect);
            pointer_gain = gain;
            xr.aspect = aspect;
            if (resized) {
                xr.count = -1;
                scene_update(); /* Require a newly presented scene for the new viewport. */
            }
            if (!xr.ready) {
                xr.heartbeat = now_ns();
                if (!xr.pose_stamp)
                    xr.pose_stamp = xr.heartbeat;
            }
            xr.ready = true;
        } else if (m.type == AIR_FRAME_PRESENTED && m.stamp_ns <= now_ns() &&
                   now_ns() - m.stamp_ns < 250000000) {
            xr.heartbeat = m.stamp_ns;
            if (xr.overview && m.stamp_ns > xr.overview_stamp) {
                xr.overview_stamp = m.stamp_ns;
                xr.overview_presented = m.data.frame.overview_revision;
                memcpy(xr.overview_basis, m.data.frame.overview_basis, sizeof(xr.overview_basis));
            }
            if(m.epoch==xr.epoch && m.data.frame.complete && m.stamp_ns>xr.widget_presented_stamp){
                xr.widget_presented_stamp=m.stamp_ns;
                widget_presented_yaw(m.data.frame.widget_yaw);
            }
            /* A live scanout may contain a window still being recaptured. Keep
             * recovery liveness separate from permission to click that scene. */
            if (m.epoch == xr.epoch && !m.data.frame.complete)
                xr.ack = 0;
        } else if (m.type == AIR_PRESENTED && m.epoch == xr.epoch && m.stamp_ns <= now_ns() &&
                   now_ns() - m.stamp_ns < 250000000) {
            bool new_revision = xr.ack != m.epoch;
            xr.ack = m.epoch;
            xr.heartbeat = m.stamp_ns;
            if (new_revision && xr.mode == SPATIAL && !xr.manipulation.region) {
                if (xr.pointer_pending) {
                    double sx, sy;
                    if (cursor_process_motion(now_ns() / 1000000, &sx, &sy))
                        wlr_seat_pointer_notify_motion(server.seat.wlr_seat, now_ns() / 1000000, sx,
                                                       sy);
                    xr.pointer_pending = false;
                } else if (!xr.scene_head_only)
                    cursor_update_focus();
            }
        } else if (m.type == AIR_POSE) {
            uint64_t sample=0;
            bool valid=awatchdog_pose_time(now_ns(),m.stamp_ns,m.data.pose.age,&sample);
            for (int i = 0; i < 9; i++)
                valid =
                    valid && isfinite(m.data.pose.basis[i]) && fabs(m.data.pose.basis[i]) <= 1.001;
            for (int i = 0; i < 3; i++)
                valid = valid &&
                        fabs(adot(abaxis(m.data.pose.basis, i), abaxis(m.data.pose.basis, i)) - 1) <
                            .02;
            if (valid && sample>=xr.pose_stamp) {
                memcpy(xr.basis, m.data.pose.basis, sizeof(xr.basis));
                xr.pose_stamp=sample;
            }
        } else if (m.type == AIR_SHUTDOWN) {
            stop_spatial("renderer requested shutdown", true);
            return 0;
        }
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
        stop_spatial("renderer exited", true);
    return 0;
}
static void stereo_packet(void) {
    struct air_packet m = {.type = AIR_STEREO};
    m.data.stereo.enabled = xr.stereo;
    m.data.stereo.ipd_mm = xr.ipd_mm;
    m.data.stereo.center_mm = xr.ipd_center_mm;
    m.data.stereo.baseline_mm = xr.ipd_baseline_mm;
    m.data.stereo.calibration = xr.calibration;
    send_packet(&m, -1);
}
static void publish_controls(void) {
    const char *ready = getenv("AIR_TRACKER_READY_FILE"), *display = getenv("WAYLAND_DISPLAY");
    if (!ready || !display || getenv("AIR_NATIVE_TEST_ID")) return;
    char path[4096], temp[4096];
    int a = snprintf(path,sizeof(path),"%s.controls",ready);
    int b = snprintf(temp,sizeof(temp),"%s.controls.tmp",ready);
    if (a<0 || (size_t)a>=sizeof(path) || b<0 || (size_t)b>=sizeof(temp)) return;
    FILE *f=fopen(temp,"w");
    if (!f) return;
    int n=fprintf(f,"%ld %d %.6f %s\n",(long)getpid(),0,xr.ipd_mm,display);
    int closed=fclose(f);
    if (n>0 && !closed) rename(temp,path); else unlink(temp);
}
/* Require acknowledgment of our latest control before accepting a changed
 * profile. An older publication must never undo a newer key adjustment. */
static void refresh_ipd(void) {
    uint64_t now=now_ns();
    if (getenv("AIR_NATIVE_TEST_ID") || now-xr.ipd_poll<250000000ULL) return;
    xr.ipd_poll=now;
    struct air_ipd p;double control;
    if(!aipd_read(getenv("AIR_TRACKER_READY_FILE"),(long)getpid(),now,&p,&control) ||
       fabs(control-xr.ipd_mm)>.000001 ||
       aipd_equal(p,xr.ipd_profile))return;
    xr.ipd_profile=p;
    xr.ipd_mm=p.total;xr.ipd_center_mm=p.center;xr.ipd_baseline_mm=p.baseline;
    publish_controls();
    stereo_packet();
    if(xr.mode==ARMING || xr.mode==SPATIAL){xr.count=-1;scene_update();}
    wlr_log(WLR_INFO,"spatial: software eye profile total %.2f left %.2f right %.2f mm",
            p.total,p.total/2-p.center,p.total/2+p.center);
}
static void start(void) {
    xr.automatic.pending = false;
    const char *helper = getenv("AIR_SPATIAL_HELPER"), *configured = getenv("AIR_SPATIAL_OUTPUT");
    struct output *selected = select_output(configured);
    const char *target = selected ? selected->wlr_output->name : NULL;
    if (!helper || helper[0] != '/' || !target || locked() || xr.restore_pending || xr.desktop_restore_pending ||
        !output_is_usable(selected)) {
        wlr_log(WLR_ERROR, "spatial: require one extended original Air with USB, a valid helper, "
                           "and completed output recovery");
        return;
    }
    if (server.input_mode != LAB_INPUT_STATE_PASSTHROUGH || server.seat.drag.active ||
        (server.seat.current_constraint && !air_spatial_can_constrain(server.seat.current_constraint)))
        return;
    xr.reported_oled = glasses_screen_state();
    if (getenv("AIR_NATIVE_TEST_ID")) {
        xr.reported_display_mode = 0;
        xr.stereo = false; /* Disposable virtual outputs have no hardware mode. */
    } else {
        int hardware = ascreen_read_state(getenv("AIR_TRACKER_READY_FILE"), ".stereo", now_ns());
        xr.reported_display_mode = hardware;
        int selected_mode = aselected_display_mode(xr.stereo_enabled, hardware,
            selected->wlr_output->width, selected->wlr_output->height,
            selected->wlr_output->scale, selected->wlr_output->transform);
        if (selected_mode < 0) return; /* Wait for the physical mode/layout to settle. */
        xr.stereo = selected_mode == 1;
    }
    int refresh = selected->wlr_output->refresh;
    xr.frame_interval_ms = refresh > 65000 ? MAX(5, 1000000 / refresh) : 16;
    xr.offscreen_frame = 0;
    if (!getenv("AIR_NATIVE_TEST_ID")) {
        struct wlr_output_mode *mode;
        wl_list_for_each(mode, &selected->wlr_output->modes, link) {
            if (mode->width == selected->wlr_output->width &&
                mode->height == selected->wlr_output->height)
                wlr_log(WLR_INFO, "spatial: advertised %dx%d @ %.3f Hz%s",
                        mode->width, mode->height, mode->refresh / 1000.,
                        mode == selected->wlr_output->current_mode ? " (current)" : "");
        }
    }
    publish_controls(); /* Preference persistence only: hardware-mode request stays zero. */
    wlr_log(WLR_INFO, "spatial: using glasses-selected %s mode", xr.stereo ? "stereo" : "mono");
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
    xr.widget_yaw = 0;
    xr.widget_hold = false;
    xr.widget_presented_stamp = 0;
    xr.started = now_ns();
    xr.heartbeat = xr.started;
    xr.pose_stamp = 0;
    xr.watchdog=(struct air_watchdog){0};
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
        aevent_write("compositor", "presenter_fork_failed", ",\"errno\":%d", errno);
        close(pair[0]);
        close(wayland[0]);
        xr.mode = DESKTOP;
        return;
    }
    xr.pid = xr.attempt_pid = pid;
    struct presenter_child *child = calloc(1, sizeof(*child));
    if (child) {
        child->pid = pid;
        child->generation = xr.generation;
        wl_list_insert(&xr.presenter_children, &child->link);
    }
    spatial_event("spatial_arming", "presenter spawned");
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
    stereo_packet();
    air_widgets_start(target);
    scene_update();
    status("ARMING | waiting for tracking and a healthy scene; Super+Alt+X turns auto mode off");
    pointer_packet();
}
static int glasses_screen_state(void) {
    /* Disposable virtual outputs have no physical OLED; never bypass real Air. */
    struct output *o = find_output(getenv("AIR_SPATIAL_OUTPUT"));
    if (o && getenv("AIR_NATIVE_TEST_ID") &&
        (wlr_output_is_wl(o->wlr_output) || wlr_output_is_headless(o->wlr_output)))
        return 1;
    static uint64_t checked;
    static int state = -1;
    uint64_t now = now_ns();
    if (now - checked >= 100000000) {
        state = ascreen_read(getenv("AIR_TRACKER_READY_FILE"), now);
        checked = now;
    }
    return state;
}
/* Run on the compositor event loop, after output creation/layout has settled.
 * This uses the private entry path, never synthesizes input, and
 * lets the existing private launcher wait for the session-owned tracker. */
static void automatic_update(void) {
    uint64_t now = now_ns();
    if (!xr.auto_start || now - xr.auto_poll < 250000000)
        return;
    xr.auto_poll = now;
    struct output *output = select_output(getenv("AIR_SPATIAL_OUTPUT"));
    if (output)
        snprintf(xr.auto_output, sizeof(xr.auto_output), "%s", output->wlr_output->name);
    bool idle = xr.mode == DESKTOP && !xr.restore_pending && !xr.desktop_restore_pending &&
                output_is_usable(output) &&
                server.input_mode == LAB_INPUT_STATE_PASSTHROUGH &&
                !server.seat.drag.active &&
                (!server.seat.current_constraint || air_spatial_can_constrain(server.seat.current_constraint)) &&
                !server.seat.wlr_seat->pointer_state.button_count && !xr.native_left_down;
    bool present = output != NULL && glasses_screen_state() == 1;
    if (aauto_update(&xr.automatic, present,
                     locked(), idle, now)) {
        wlr_log(WLR_INFO, "spatial: automatic preview for connected glasses %s",
                xr.auto_output);
        start();
    }
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
    wl_event_source_timer_update(xr.timer, xr.mode == SPATIAL ? xr.frame_interval_ms : 16);
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
        if (!pending) {
            wlr_log(WLR_INFO, "spatial: physical output restoration complete");
            spatial_event("outputs_restored", "power retry completed");
        }
    }
    if (xr.mode == DESKTOP && xr.desktop_restore_pending && !xr.restore_pending)
        restore_desktop_windows();
    air_widgets_refresh();
    refresh_ipd();
    automatic_update();
    if (xr.mode != ARMING && xr.mode != SPATIAL)
        return 0;
    xr.reported_oled = glasses_screen_state();
    if (!getenv("AIR_NATIVE_TEST_ID")) {
        xr.reported_display_mode = ascreen_read_state(getenv("AIR_TRACKER_READY_FILE"), ".stereo", now_ns());
        if (xr.reported_display_mode != (xr.stereo ? 1 : 0)) {
            air_spatial_stop(xr.reported_display_mode < 0 ? "glasses display mode status unavailable" :
                                                         "glasses reported a different display mode");
            return 0;
        }
    }
    if (xr.reported_oled != 1) {
        air_spatial_stop(xr.reported_oled < 0 ? "glasses OLED status unavailable" : "glasses reported OLED off");
        return 0;
    }
    if (locked()) {
        air_spatial_stop("session locked");
        return 0;
    }
    if (!output_is_usable(find_output(xr.target_name))) {
        air_spatial_stop("glasses output disconnected");
        return 0;
    }
    if (xr.mode == SPATIAL && (server.input_mode != LAB_INPUT_STATE_PASSTHROUGH &&
                               server.input_mode != LAB_INPUT_STATE_CYCLE)) {
        air_spatial_stop("unsupported desktop grab; use desktop fallback");
        return 0;
    }
    uint64_t now = now_ns();
    if(xr.mode==SPATIAL || (xr.mode==ARMING && xr.ready)){
        bool was_paused=xr.watchdog.paused;
        enum air_watchdog_result health=awatchdog_update(&xr.watchdog,now,xr.pose_stamp,xr.heartbeat);
        if(health==AIR_WATCHDOG_TRACKING_LOST || health==AIR_WATCHDOG_PRESENTATION_LOST){
            air_spatial_stop(health==AIR_WATCHDOG_TRACKING_LOST ? "tracking watchdog expired"
                                                              : "presentation watchdog expired");
            return 0;
        }
        if(health==AIR_WATCHDOG_PAUSED){
            if (!was_paused) {
                wlr_log(WLR_INFO,"spatial: graphics pause; retaining scene, new pointer actions blocked (maximum 10 seconds)");
                spatial_event("watchdog_pause", "stale tracking or scanout");
            }
            return 0; /* No per-tick scene/pointer traffic into a stalled helper. */
        }
        if (was_paused) {
            wlr_log(WLR_INFO,"spatial: graphics resumed with fresh tracking and presentation");
            spatial_event("watchdog_resume", "fresh tracking and scanout");
        }
    }
    if (xr.mode == ARMING && !xr.ready && now - xr.started > 120000000000ULL) {
        air_spatial_stop("preview startup timed out");
        return 0;
    }
    if (xr.mode == SPATIAL && !xr.native_left_down && !server.seat.drag.active &&
        !server.seat.wlr_seat->pointer_state.button_count)
        apply_spatial_sizes();
    if (xr.mode == SPATIAL && xr.native_left_down && xr.manipulation.region &&
        xr.manipulation.region != AIR_TITLE)
        manipulation_update();
    if (xr.game_id && (!eligible(find_view(xr.game_id)) ||
                       root_view(server.active_view) != find_view(xr.game_id)))
        clear_game_focus();
    if (xr.dock_until && now >= xr.dock_until && widget_follow_allowed()) {
        xr.dock_until = 0;
        xr.dock_offset = 0;
    }
    if (xr.overview && now - xr.overview_polled >= 120000000ULL) {
        xr.overview_polled = now;
        overview_refresh(false);
    }
    scene_update();
    if (xr.mode == DESKTOP)
        return 0;
    if (xr.mode == SPATIAL) {
        wlr_cursor_unset_image(server.seat.cursor);
        struct timespec when;
        clock_gettime(CLOCK_MONOTONIC, &when);
        bool background_frame = now >= xr.offscreen_frame;
        if (background_frame)
            xr.offscreen_frame = now + 100000000ULL;
        for (int i = 0; i < xr.count; i++) {
            struct air_widget *w = air_widget_find(xr.scene[i].panel.id);
            if (w)
                wlr_scene_node_for_each_buffer(&w->capture->tree.node, frame_callback, &when);
            else if (eligible(xr.scene[i].view) &&
                     (background_frame || apanel_maybe_visible(&xr.scene[i].panel, xr.basis,
                                                              xr.aspect, xr.zoom)))
                wlr_scene_node_for_each_buffer(&xr.scene[i].view->capture.scene->tree.node,
                                               frame_callback, &when);
        }
        pointer_packet();
    }
    if (xr.mode == ARMING)
        enter();
    return 0;
}
static void notify_auto_state(bool enabled) {
    if (getenv("AIR_NATIVE_TEST_ID")) return;
    pid_t pid=fork();
    if (pid==0) {
        execlp("notify-send","notify-send","--app-name=Air Spatial Controls",
               "--expire-time=2500","--hint=string:x-dunst-stack-tag:air-spatial-auto",
               "Auto spatial mode",enabled?"Enabled":"Disabled",(char *)NULL);
        _exit(127);
    }
}
/* The shortcut controls the policy, not a competing one-shot mode request. */
static void toggle_automatic(void) {
    xr.auto_start = !xr.auto_start;
    xr.automatic = (struct air_auto){0};
    xr.auto_poll = 0;
    wlr_log(WLR_INFO, "spatial: automatic spatial mode %s", xr.auto_start ? "enabled" : "disabled");
    aevent_write("compositor", "automatic_mode", ",\"enabled\":%s", aevent_bool(xr.auto_start));
    if (!xr.auto_start)
        air_spatial_stop("automatic spatial mode disabled");
    notify_auto_state(xr.auto_start);
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
    if (ev->keycode == KEY_ESC && ev->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
        air_spatial_active() && server.seat.drag.active) {
        cancel_spatial_drag();
        swallowed[ev->keycode] = true;
        return true;
    }
    uint32_t mods = wlr_keyboard_get_modifiers(kb);
    bool chord = (mods & (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT)) ==
                 (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT);
    if (xr.overview && !locked() && ev->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
        !(chord && (ev->keycode == KEY_X || ev->keycode == KEY_L))) {
        if (ev->keycode == KEY_LEFTMETA || ev->keycode == KEY_RIGHTMETA ||
            ev->keycode == KEY_LEFTALT || ev->keycode == KEY_RIGHTALT ||
            ev->keycode == KEY_LEFTSHIFT || ev->keycode == KEY_RIGHTSHIFT ||
            ev->keycode == KEY_LEFTCTRL || ev->keycode == KEY_RIGHTCTRL)
            return false;
        overview_key(kb, ev->keycode);
        swallowed[ev->keycode] = true;
        return true;
    }
    if (ev->state != WL_KEYBOARD_KEY_STATE_PRESSED ||
        (mods & (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT)) != (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT))
        return false;
    uint32_t key = ev->keycode;
    bool handled = true;
    if (locked())
        return false;
    if (key == KEY_X) {
        if (!swallowed[key])
            toggle_automatic();
    } else if (key == KEY_L && (xr.overview || workspace_controls_allowed())) {
        if (!swallowed[key]) {
            if (xr.overview) {
                overview_close();
            } else {
                zoom_set(1);
                xr.overview = true;
                xr.overview_selected = 0;
                xr.overview_query[0] = 0;
                overview_refresh(true);
                cursor_update_focus();
            }
        }
    } else if (key == KEY_G && air_spatial_active() &&
               (xr.game_id || workspace_controls_allowed() ||
                air_spatial_can_constrain(server.seat.current_constraint))) {
        if (!swallowed[key])
            toggle_game_focus();
    } else if (workspace_controls_allowed() && (key == KEY_C || key == KEY_B ||
               key == KEY_S || key == KEY_D || (key >= KEY_1 && key <= KEY_4))) {
        if (!swallowed[key]) {
            if (key == KEY_C)
                center_window(server.active_view);
            else if (key == KEY_S)
                save_layout();
            else if (key == KEY_D)
                summon_dock();
            else if (key == KEY_4)
                recall_layout();
            else if (key == KEY_B) {
                xr.workspace_state.reading ^= 1;
                status(save_workspace_state() ? "Saved spatial reading preference" :
                                                "Reading preference applied for this session");
                refresh_workspace_scene();
            } else {
                apply_workspace_layout(key - KEY_1 + AIR_LAYOUT_FOCUS);
            }
        }
    } else if (key == KEY_I && xr.stereo &&
               (workspace_controls_allowed() || (xr.calibration && air_spatial_active()))) {
        if (!swallowed[key]) {
            zoom_set(1);
            xr.calibration = !xr.calibration;
            stereo_packet();
            xr.count=-1;
            scene_update();
        }
    } else if ((key == KEY_COMMA || key == KEY_DOT || key == KEY_BACKSLASH) && xr.stereo) {
        xr.ipd_mm = key == KEY_BACKSLASH ? xr.ipd_baseline_mm :
                    aclamp(xr.ipd_mm + (key == KEY_DOT ? .5 : -.5), 45, 80);
        publish_controls();
        stereo_packet();
        if (xr.mode == ARMING || xr.mode == SPATIAL) { xr.count=-1; scene_update(); }
        wlr_log(WLR_INFO, "spatial: IPD %.1f mm", xr.ipd_mm);
    } else if ((key == KEY_MINUS || key == KEY_EQUAL || key == KEY_0) && xr.mode == SPATIAL) {
        pointer_speed = key == KEY_0
                            ? configured_pointer_speed
                            : aclamp(pointer_speed * (key == KEY_MINUS ? 1 / 1.2 : 1.2), .25, 3);
        char message[192];
        snprintf(message, sizeof(message),
                 "SPATIAL | pointer %.2fx | Super+Alt+X turns auto mode off", pointer_speed);
        status(message);
    } else if (key == KEY_R && xr.mode != DESKTOP) {
        struct air_packet m = {.type = AIR_RECENTER};
        send_packet(&m, -1);
    } else if (key == KEY_F && xr.mode == SPATIAL) {
        struct air_vec f = abaxis(xr.basis, 2);
        xr.theta = atan2(f.x, f.z);
        xr.height = aclamp(AIR_RADIUS * f.y / fmax(.001, hypot(f.x, f.z)), -1.2, 1.2);
        pointer_packet();
        double sx, sy;
        if (cursor_process_motion(now_ns() / 1000000, &sx, &sy))
            wlr_seat_pointer_notify_motion(server.seat.wlr_seat, now_ns() / 1000000, sx, sy);
        wlr_seat_pointer_notify_frame(server.seat.wlr_seat);
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
    if (!acursor_visible(pixels, width, height, stride,
                         format == 1 || format == DRM_FORMAT_XBGR8888))
        return false;
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
/* Keep a pending client surface's commit listener while showing a fallback. */
static bool cursor_named(const char *name) {
    if (!air_spatial_active())
        return false;
    if (!name) name = "default";
    uint32_t region = xr.manipulation.region ? xr.manipulation.region : xr.hover_region;
    if (server.seat.drag.active) {
        name = "grabbing";
    } else if (region == AIR_FRAME)
        name = "default";
    else if (region == AIR_CENTER || region == AIR_PIN || region == AIR_MINIMIZE)
        name = "pointer";
    else if (region == AIR_TITLE)
        name = xr.native_left_down ? "grabbing" : "grab";
    else if ((region & (AIR_EDGE_LEFT | AIR_EDGE_RIGHT)) &&
             (region & (AIR_EDGE_TOP | AIR_EDGE_BOTTOM)))
        name = (region == (AIR_EDGE_TOP | AIR_EDGE_LEFT) ||
                region == (AIR_EDGE_BOTTOM | AIR_EDGE_RIGHT))
                   ? "nwse-resize"
                   : "nesw-resize";
    else if (region & (AIR_EDGE_LEFT | AIR_EDGE_RIGHT))
        name = "ew-resize";
    else if (region & (AIR_EDGE_TOP | AIR_EDGE_BOTTOM))
        name = "ns-resize";
    struct wlr_xcursor *cursor =
        wlr_xcursor_manager_get_xcursor(server.seat.xcursor_manager, name, 1);
    if (!cursor || !cursor->image_count)
        cursor = wlr_xcursor_manager_get_xcursor(server.seat.xcursor_manager, "left_ptr", 1);
    if (cursor && cursor->image_count) {
        struct wlr_xcursor_image *i = cursor->images[0];
        if (cursor_pixels(i->buffer, i->width, i->height, i->width * 4, i->hotspot_x,
                          i->hotspot_y, 0, 1))
            return true;
    }
    /* A missing/blank theme must not make the world pointer vanish either. */
    uint32_t arrow[24 * 16] = {0};
    for (int y = 0; y < 20; y++)
        for (int x = 0; x <= y / 2; x++)
            arrow[y * 16 + x] = !x || x == y / 2 || y == 19 ? 0xff101010 : 0xffffffff;
    for (int y = 15; y < 24; y++)
        for (int x = 5; x <= 8; x++)
            arrow[y * 16 + x] = x == 5 || x == 8 || y == 23 ? 0xff101010 : 0xffffffff;
    return cursor_pixels(arrow, 16, 24, 16 * 4, 0, 0, 0, 1);
}
bool air_spatial_cursor_name(const char *name) {
    if (!air_spatial_active())
        return false;
    cursor_detach();
    return cursor_named(name);
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
    if (air_spatial_active())
        cursor_named("default");
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
    if (!surface || !surface->buffer)
        return cursor_named("default");
    struct wlr_buffer *buffer = surface->buffer->source;
    void *pixels;
    uint32_t format;
    size_t stride;
    bool shown = false;
    if (buffer && wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &pixels,
                                                   &format, &stride)) {
        if (format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888 ||
            format == DRM_FORMAT_ABGR8888 || format == DRM_FORMAT_XBGR8888) {
            /* wl_shm uses 0/1 for ARGB/XRGB, FOURCC for ABGR/XBGR. */
            uint32_t wire = format == DRM_FORMAT_ARGB8888 ? 0 :
                            format == DRM_FORMAT_XRGB8888 ? 1 : format;
            shown = cursor_pixels(pixels, buffer->width, buffer->height, stride,
                                  hx * surface->current.scale, hy * surface->current.scale,
                                  wire, surface->current.scale);
        }
        wlr_buffer_end_data_ptr_access(buffer);
    }
    if (!shown)
        cursor_named("default");
    return true;
}
bool air_spatial_private_client(struct wl_client *client) { return client && client == xr.client; }
void air_spatial_init(void) {
    wl_list_init(&xr.presenter_children);
    air_widgets_init();
    xr.enabled = getenv("AIR_SPATIAL_HELPER") != NULL;
    xr.auto_start = getenv("AIR_SPATIAL_AUTO_START") &&
                    !strcmp(getenv("AIR_SPATIAL_AUTO_START"), "1");
    xr.stereo_enabled = getenv("AIR_SPATIAL_STEREO") && !strcmp(getenv("AIR_SPATIAL_STEREO"),"1");
    xr.reserved_display = getenv("AIR_SPATIAL_RESERVED_DISPLAY") &&
                          !strcmp(getenv("AIR_SPATIAL_RESERVED_DISPLAY"),"1");
    xr.ipd_mm = 64;
    const char *ipd = getenv("AIR_SPATIAL_IPD_MM");
    if (ipd) { char *end; double value=strtod(ipd,&end);
        if (!*end && isfinite(value) && value>=45 && value<=80) xr.ipd_mm=value;
    }
    xr.ipd_baseline_mm=xr.ipd_mm;
    const char *center=getenv("AIR_SPATIAL_IPD_CENTER_MM");
    const char *baseline=getenv("AIR_SPATIAL_IPD_BASELINE_MM");
    if(center){char *end;double value=strtod(center,&end);
        if(!*end && isfinite(value) && fabs(value)<=5)xr.ipd_center_mm=value;}
    if(baseline){char *end;double value=strtod(baseline,&end);
        if(!*end && isfinite(value) && value>=45 && value<=80)xr.ipd_baseline_mm=value;}
    xr.ipd_profile=(struct air_ipd){xr.ipd_mm,xr.ipd_center_mm,xr.ipd_baseline_mm};
    xr.power = getenv("AIR_SPATIAL_POWER") && !strcmp(getenv("AIR_SPATIAL_POWER"), "1");
    pointer_gain = amouse_gain(1920);
    const char *speed = getenv("AIR_SPATIAL_POINTER_SPEED");
    if (speed) {
        char *end;
        double value = strtod(speed, &end);
        if (*end == '\0' && isfinite(value) && value >= .25 && value <= 3)
            configured_pointer_speed = pointer_speed = value;
    }
    const char *scale = getenv("AIR_SPATIAL_PANEL_SCALE");
    if (scale) {
        char *end;
        double value = strtod(scale, &end);
        if (*end == '\0' && isfinite(value) && value >= .25 && value <= 2)
            panel_scale = value;
    }
    if (xr.enabled) {
        load_workspace_state();
        xr.timer = wl_event_loop_add_timer(server.wl_event_loop, tick, NULL);
        wl_event_source_timer_update(xr.timer, 16);
        wlr_log(WLR_INFO, "native spatial feature ready (power transitions %s)",
                xr.power ? "enabled" : "disabled");
    }
}
static void reserved_destroy(void) {
    if (xr.reserved_tree) wlr_scene_node_destroy(&xr.reserved_tree->node);
    xr.reserved_tree=NULL;xr.reserved_black=NULL;xr.reserved_output[0]=0;
}
static void mirror_destroyed(struct wl_listener *listener, void *data) {
    (void)listener;(void)data;
    wl_list_remove(&xr.mirror_destroy.link);
    xr.presenter_mirror=NULL;xr.mirrored_surface=NULL;
}
struct output *air_spatial_desktop_output(struct output *proposed) {
    if (!xr.reserved_display || !original_air(proposed)) return proposed;
    struct output *o;
    wl_list_for_each(o,&server.outputs,link)
        if (!original_air(o) && output_is_usable(o)) return o;
    return proposed; /* Keep a desktop when the glasses are the only usable output. */
}
void air_spatial_output_frame(struct output *output) {
    if (!xr.reserved_display || getenv("AIR_NATIVE_TEST_ID")) return;
    struct output *target=select_output(getenv("AIR_SPATIAL_OUTPUT"));
    if (target!=output) return;
    struct output *desktop=air_spatial_desktop_output(target);
    if (xr.mode==DESKTOP && desktop==target) { reserved_destroy();return; }
    struct wlr_box box;
    wlr_output_layout_get_box(server.output_layout,target->wlr_output,&box);
    if (box.width<=0 || box.height<=0) return;
    if (!xr.reserved_tree) {
        xr.reserved_tree=wlr_scene_tree_create(&server.scene->tree);
        if (!xr.reserved_tree) return;
        const float black[4]={0,0,0,1};
        xr.reserved_black=wlr_scene_rect_create(xr.reserved_tree,box.width,box.height,black);
        if (!xr.reserved_black) { reserved_destroy();return; }
        snprintf(xr.reserved_output,sizeof(xr.reserved_output),"%s",output->wlr_output->name);
    }
    wlr_scene_node_set_position(&xr.reserved_tree->node,box.x,box.y);
    wlr_scene_rect_set_size(xr.reserved_black,box.width,box.height);
    wlr_scene_node_raise_to_top(&xr.reserved_tree->node);
    if (xr.layer && !locked() && xr.layer->surface!=xr.mirrored_surface) {
        if (xr.presenter_mirror) wlr_scene_node_destroy(&xr.presenter_mirror->node);
        xr.presenter_mirror=wlr_scene_subsurface_tree_create(xr.reserved_tree,xr.layer->surface);
        if (xr.presenter_mirror) {
            xr.mirrored_surface=xr.layer->surface;
            xr.mirror_destroy.notify=mirror_destroyed;
            wl_signal_add(&xr.presenter_mirror->node.events.destroy,&xr.mirror_destroy);
        }
    }
    if (xr.presenter_mirror) wlr_scene_node_set_enabled(&xr.presenter_mirror->node,!locked());
    /* Authentication remains above the black mask and the private presenter. */
    struct output *o;
    wl_list_for_each(o,&server.outputs,link)
        if (o->session_lock_tree) wlr_scene_node_raise_to_top(&o->session_lock_tree->node);
    if (xr.mode==DESKTOP && !locked() && desktop!=target &&
        server.input_mode==LAB_INPUT_STATE_PASSTHROUGH && !server.seat.drag.active &&
        !server.seat.wlr_seat->pointer_state.button_count) {
        /* Don't leave ordinary windows or the flat pointer in an invisible region. */
        struct view *v;
        wl_list_for_each(v,&server.views,link)
            if (v->output==target && v->mapped) view_move_to_output(v,desktop);
        double x=server.seat.cursor->x,y=server.seat.cursor->y;
        if (wlr_box_contains_point(&box,x,y)) {
            wlr_output_layout_closest_point(server.output_layout,desktop->wlr_output,x,y,&x,&y);
            wlr_cursor_warp_closest(server.seat.cursor,NULL,x,y);
            cursor_update_focus();
        }
    }
}
void air_spatial_finish(void) {
    reserved_destroy();
    air_spatial_stop("compositor shutdown");
    struct presenter_child *child, *next;
    wl_list_for_each_safe(child, next, &xr.presenter_children, link) {
        if (child->exit_timer) {
            wl_event_source_remove(child->exit_timer);
            terminate_presenter(child->pid, child);
        }
        wl_list_remove(&child->link);
        free(child);
    }
    air_widgets_finish();
    wl_array_release(&xr.desktop_windows);
    wl_array_init(&xr.desktop_windows);
    if (xr.timer) {
        wl_event_source_remove(xr.timer);
        xr.timer = NULL;
    }
}

void air_spatial_forget_view(struct view *v) {
    if (xr.manipulation.view_id == v->creation_id || xr.manipulation.root_id == v->creation_id)
        xr.manipulation.region = 0;
    for (int i = xr.poses_count - 1; i >= 0; i--) {
        if (xr.poses[i].id == v->creation_id)
            xr.poses[i] = xr.poses[--xr.poses_count];
    }
    for (int i = 0; i < xr.count; i++)
        if (xr.scene[i].view == v) {
            xr.scene[i].view = NULL;
            xr.ack = 0;
        }
}

void air_spatial_helper_exited(pid_t pid, int code, int status) {
    air_widgets_child_exited(pid);
    struct presenter_child *child = presenter_child_find(pid);
    if (child) {
        aevent_write("presenter", "process_exit",
            ",\"generation\":%llu,\"child_pid\":%ld,\"exit_code\":%d,\"signal\":%d,"
            "\"core_dumped\":%s,\"termination_requested\":%s",
            (unsigned long long)child->generation, (long)pid,
            code == CLD_EXITED ? status : -1, code == CLD_EXITED ? 0 : status,
            aevent_bool(code == CLD_DUMPED), aevent_bool(child->termination_requested));
        if (child->exit_timer)
            wl_event_source_remove(child->exit_timer);
        wl_list_remove(&child->link);
        free(child);
    }
    if (pid == xr.pid) {
        xr.pid = 0;
        air_spatial_stop("presenter exited");
    }
}
void air_spatial_output_lost(struct output *output) {
    if (!strcmp(xr.reserved_output,output->wlr_output->name)) reserved_destroy();
    if (!strcmp(xr.auto_output, output->wlr_output->name))
        xr.automatic = (struct air_auto){0};
    if (xr.mode == DESKTOP || xr.mode == RESTORING)
        return;
    for (int i = 0; i < xr.noutputs; i++)
        if (!strcmp(xr.outputs[i].name, output->wlr_output->name))
            xr.outputs[i].name[0] = 0;
    char connector[128];
    aevent_quote(connector, sizeof(connector), output->wlr_output->name);
    aevent_write("compositor", "output_removed", ",\"generation\":%llu,\"output\":%s",
                 (unsigned long long)xr.generation, connector);
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
