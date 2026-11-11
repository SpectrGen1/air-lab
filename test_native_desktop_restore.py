"""Check desktop placement across spatial entry and exit without a compositor."""
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from test_native_cursor import function

ROOT = Path(__file__).resolve().parent


class DesktopRestoreTests(unittest.TestCase):
    def test_saved_rectangles_and_glasses_evacuation(self):
        source = (ROOT / 'native/labwc/air-spatial.c').read_text()
        prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct wlr_box { int x, y, width, height; };
struct output {
    bool air, usable;
    struct wlr_box area;
    struct wl_list link;
};
struct view_impl { bool configure; };
struct view {
    bool mapped, floating;
    const struct view_impl *impl;
    uint64_t creation_id;
    struct wlr_box current, pending;
    struct output *output, *saved_output;
    struct wl_list link;
};
struct desktop_window { uint64_t id; struct wlr_box geometry; };
static struct {
    struct wl_list views, outputs;
    void *output_layout;
} server;
static struct {
    const char *target_name;
    struct wl_array desktop_windows;
    bool desktop_restore_pending;
} xr;
static struct output air = {.air = true, .usable = true, .area = {1920, 0, 1920, 1080}};
static struct output monitor = {.usable = true, .area = {0, 30, 1920, 1050}};

static struct output *find_output(const char *name) { (void)name; return &air; }
static bool original_air(struct output *output) { return output->air; }
static bool output_is_usable(struct output *output) { return output && output->usable; }
static struct wlr_box output_usable_area_in_layout_coords(struct output *output) {
    return output->area;
}
static bool wlr_box_empty(const struct wlr_box *box) {
    return box->width <= 0 || box->height <= 0;
}
static bool wlr_box_equal(const struct wlr_box *a, const struct wlr_box *b) {
    return a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height;
}
static struct output *output_from_wlr_output(struct output *output) { return output; }
static struct output *wlr_output_layout_output_at(void *layout, double x, double y) {
    (void)layout;
    if (y < 0 || y >= 1080 || x < 0 || x >= 3840)
        return NULL;
    struct output *output = x < 1920 ? &monitor : &air;
    return output->usable ? output : NULL;
}
static bool view_is_floating(struct view *view) { return view->floating; }
static void view_set_output(struct view *view, struct output *output) { view->output = output; }
#define spatial_event(...) ((void)0)
static void view_save_last_placement(struct view *view) { view->saved_output = view->output; }
static void view_move_resize(struct view *view, struct wlr_box box) { view->pending = box; }
static void view_move_to_output(struct view *view, struct output *output) {
    view->output = output;
    view->pending = output->area;
}

static void add_view(struct view *view, uint64_t id, struct output *output,
                     struct wlr_box box, bool floating) {
    static const struct view_impl impl = {.configure = true};
    *view = (struct view){.mapped = true, .floating = floating, .impl = &impl,
        .creation_id = id, .output = output, .current = box, .pending = box};
    wl_list_insert(&server.views, &view->link);
}
'''
        suffix = r'''
int main(void) {
    wl_list_init(&server.views);
    wl_list_init(&server.outputs);
    wl_list_insert(&server.outputs, &monitor.link);
    wl_list_insert(&server.outputs, &air.link);
    wl_array_init(&xr.desktop_windows);

    struct view existing, existing_air, closed, new_air, new_visible, fullscreen;
    struct wlr_box saved = {120, 130, 720, 480};
    struct wlr_box visible = {700, 500, 650, 340};
    add_view(&existing, 1, &monitor, (struct wlr_box){120, 130, 680, 460}, true);
    existing.pending = saved;
    add_view(&existing_air, 2, &air, (struct wlr_box){2300, 220, 680, 540}, true);
    add_view(&closed, 3, &air, (struct wlr_box){2400, 100, 800, 600}, true);
    assert(snapshot_desktop_windows());

    existing.pending = (struct wlr_box){2100, 180, 1000, 400};
    existing.output = &air;
    existing_air.pending = (struct wlr_box){2100, 180, 1200, 800};
    wl_list_remove(&closed.link);
    add_view(&new_air, 4, &air, (struct wlr_box){2700, 350, 400, 300}, true);
    add_view(&new_visible, 5, &monitor, visible, true);
    new_visible.output = &air;
    /* A fullscreen view follows its assigned output even with stale geometry. */
    add_view(&fullscreen, 6, &air, monitor.area, false);
    xr.desktop_restore_pending = true;
    restore_desktop_windows();
    assert(!xr.desktop_restore_pending && !xr.desktop_windows.size);
    assert(existing.output == &monitor && wlr_box_equal(&existing.pending, &saved));
    assert(existing_air.output == &monitor);
    assert(wlr_box_equal(&existing_air.pending, &(struct wlr_box){620, 285, 680, 540}));
    assert(new_air.output == &monitor);
    assert(wlr_box_equal(&new_air.pending, &(struct wlr_box){760, 405, 400, 300}));
    assert(new_visible.output == &monitor && wlr_box_equal(&new_visible.pending, &visible));
    assert(new_visible.saved_output == &monitor);
    assert(fullscreen.output == &monitor && wlr_box_equal(&fullscreen.pending, &monitor.area));

    monitor.usable = false;
    struct wlr_box air_only = {2150, 100, 500, 400};
    add_view(&closed, 7, &air, air_only, true);
    assert(snapshot_desktop_windows());
    closed.pending.width = 800;
    restore_desktop_windows();
    assert(closed.output == &air && wlr_box_equal(&closed.pending, &air_only));
}
'''
        with tempfile.TemporaryDirectory(prefix='air-desktop-restore-') as directory:
            path = Path(directory) / 'check.c'
            path.write_text(prefix + function(source, 'remember_desktop_window') +
                            function(source, 'snapshot_desktop_windows') +
                            function(source, 'restore_desktop_windows') + suffix)
            binary = path.with_name('check')
            flags = shlex.split(subprocess.check_output(
                ['pkg-config', '--cflags', '--libs', 'wayland-server'], text=True))
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', str(path),
                            '-o', str(binary), *flags], check=True)
            subprocess.run([str(binary)], check=True)
