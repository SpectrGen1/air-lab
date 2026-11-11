"""Exercise the compositor's transient placement and title drag without Wayland."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_native_cursor import function

ROOT = Path(__file__).resolve().parent


class TransientDragTests(unittest.TestCase):
    def test_file_picker_moves_independently_and_follows_current_parent(self):
        source = (ROOT / 'native/labwc/air-spatial.c').read_text()
        prefix = r'''
#include "air-math.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

struct workspace { int unused; };
struct view;
struct view_impl { struct view *(*get_parent)(struct view *); };
struct view {
    uint64_t creation_id;
    struct workspace *workspace;
    const struct view_impl *impl;
    struct view *parent;
    struct { int x, y, width, height; } current;
};

static struct view *parent_of(struct view *view) { return view->parent; }
static const struct view_impl impl = {.get_parent = parent_of};
static struct workspace workspace;
static struct view browser = {1, &workspace, &impl, NULL, {3100, 120, 900, 600}};
static struct view picker = {2, &workspace, &impl, &browser, {3400, 210, 420, 300}};
static struct view nested = {3, &workspace, &impl, &picker, {3520, 280, 180, 130}};
static double panel_scale = 2. / 3;
static struct {
    struct {
        uint64_t id;
        struct workspace *workspace;
        double angle, height, distance, depth_offset;
        int reference_width, spatial_width, spatial_height;
        uint64_t sized_generation;
        double scale;
    } poses[512];
    int poses_count;
    uint64_t generation;
    struct {bool reading;} workspace_state;
    bool overview;
    uint32_t hover_region;
    uint64_t pose_stamp;
    double theta, height;
    struct {
        uint64_t view_id, root_id;
        struct workspace *workspace;
        uint32_t region;
        double x, y;
        struct air_panel plane;
        struct {int width,height;} requested;
    } manipulation;
} xr;

static void restore_window_placement(struct view *view, int pose) {(void)view;(void)pose;}
static void overview_move(int direction) {(void)direction;}
static struct view *find_view(uint64_t id) {
    if (id == 1) return &browser;
    if (id == 2) return &picker;
    return NULL;
}
static bool eligible(struct view *view) { return view != NULL; }
static uint64_t now_ns(void) { return 1000000000; }
static bool scene_update(void) { return true; }
static bool spatial_resize_window(struct view *view) { (void)view; return false; }
static bool air_spatial_active(void) { return true; }
static bool zoom_axis(uint32_t orientation, double delta) {
    (void)orientation;
    (void)delta;
    return false;
}
static void pointer_packet(void) {}
#define WL_POINTER_AXIS_VERTICAL_SCROLL 0
struct cursor_context { int unused; };
static void hit_context(struct cursor_context *ctx, struct air_vec *point) {
    (void)ctx;
    (void)point;
}
static void near(double a, double b) {
    if (fabs(a - b) >= 1e-6)
        fprintf(stderr, "actual %.9f, expected %.9f\n", a, b);
    assert(fabs(a - b) < 1e-6);
}
'''
        suffix = r'''
int main(void) {
    int root_pose = pose_for(&browser, 0);
    int picker_pose = pose_for(&picker, 0);
    xr.poses[root_pose].angle = .8;
    xr.poses[root_pose].height = .25;
    xr.poses[root_pose].distance = 1.2;

    struct air_panel parent = {.width = 900, .height = 600};
    struct air_panel dialog = {.width = 420, .height = 300}, anchor;
    assert(compose_panel_pose(&browser, 0, &parent, NULL));
    assert(compose_panel_pose(&picker, 0, &dialog, &anchor));
    struct air_vec base = apoint(&parent, 510, 240);
    struct air_vec delta = aadd(base, ascale(acentre(&dialog), -1));
    near(adot(delta, anormal(&parent)), .015);
    near(adot(delta, aright(&parent)), 0);
    near(adot(delta, aup(&parent)), 0);

    xr.pose_stamp = now_ns();
    xr.manipulation.view_id = picker.creation_id;
    xr.manipulation.root_id = browser.creation_id;
    xr.manipulation.workspace = &workspace;
    xr.manipulation.region = AIR_TITLE;
    xr.manipulation.plane = dialog;
    xr.manipulation.x = 260;
    xr.manipulation.y = -20;
    struct air_vec held = apoint(&dialog, xr.manipulation.x, xr.manipulation.y);
    xr.theta = atan2(held.x, held.z);
    xr.height = AIR_RADIUS * held.y / hypot(held.x, held.z);
    manipulation_update();
    near(xr.poses[picker_pose].angle, 0);
    near(xr.poses[picker_pose].height, 0);

    xr.theta += .22;
    xr.height += .15;
    manipulation_update();
    near(xr.poses[root_pose].angle, .8);
    near(xr.poses[root_pose].height, .25);
    near(xr.poses[root_pose].distance, 1.2);
    assert(fabs(xr.poses[picker_pose].angle) > .1);
    assert(fabs(xr.poses[picker_pose].height) > .05);
    assert(compose_panel_pose(&picker, 0, &dialog, &anchor));
    double x, y, t;
    assert(aplane(&dialog, aray(xr.theta, xr.height), &x, &y, &t));
    assert(fabs(x - xr.manipulation.x) < .001);
    assert(fabs(y - xr.manipulation.y) < .001);

    double picker_angle = xr.poses[picker_pose].angle;
    double picker_height = xr.poses[picker_pose].height;
    double old_angle = dialog.azimuth;
    xr.poses[root_pose].angle += .4;
    assert(compose_panel_pose(&picker, 0, &dialog, &anchor));
    near(awrap(dialog.azimuth - old_angle), .4);
    xr.poses[root_pose].height += .1;
    browser.current.x -= 450;
    browser.current.width += 900;
    assert(compose_panel_pose(&picker, 0, &dialog, &anchor));
    near(dialog.azimuth, awrap(anchor.azimuth + picker_angle));
    near(dialog.elevation, anchor.elevation + picker_height);
    near(dialog.metres_per_pixel, .0009 * panel_scale);

    struct air_panel child = {.width = 180, .height = 130};
    assert(compose_panel_pose(&nested, 0, &child, NULL));
    base = apoint(&dialog, 210, 135);
    delta = aadd(base, ascale(acentre(&child), -1));
    near(adot(delta, anormal(&dialog)), .015);
    near(adot(delta, aright(&dialog)), 0);
    near(adot(delta, aup(&dialog)), 0);
    double old_distance = dialog.distance;
    assert(air_spatial_axis(WL_POINTER_AXIS_VERTICAL_SCROLL, -15));
    assert(compose_panel_pose(&picker, 0, &dialog, &anchor));
    assert(dialog.distance < old_distance);
    near(xr.poses[root_pose].distance, 1.2);
    assert(aplane(&dialog, aray(xr.theta, xr.height), &x, &y, &t));
    assert(fabs(x - xr.manipulation.x) < .001);
    assert(fabs(y - xr.manipulation.y) < .001);
    assert(air_spatial_axis(WL_POINTER_AXIS_VERTICAL_SCROLL, 100));
    assert(compose_panel_pose(&picker, 0, &dialog, &anchor));
    near(dialog.distance, anchor.distance);
    assert(browser.current.x == 2650 && picker.current.x == 3400);
}
'''
        def extract(signature):
            start = source.index(signature + ('struct view *view) {' if signature.endswith('root_view(') else 'struct view *v, int index) {'))
            end = source.index('\n}\n', start) + 3
            return source[start:end]

        implementation = function(source, 'existing_pose') + extract('static int pose_for(') + extract('static struct view *root_view(')
        implementation += '\n'.join(function(source, name) for name in (
            'compose_panel_pose', 'manipulation_update', 'air_spatial_axis',
        ))
        with tempfile.TemporaryDirectory(prefix='air-transient-') as directory:
            path = Path(directory) / 'check.c'
            path.write_text(prefix + implementation + suffix)
            binary = path.with_name('check')
            subprocess.run([
                'cc', '-std=c17', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'native/common'), str(path), '-lm', '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
