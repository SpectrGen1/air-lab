"""Exercise the real spatial configure path with a client that commits asynchronously."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_native_cursor import function

ROOT = Path(__file__).resolve().parent


class SpatialResizeTests(unittest.TestCase):
    def test_drag_configures_client_and_coalesces_pending_requests(self):
        source = (ROOT / 'native/labwc/air-spatial.c').read_text()
        prefix = r'''
#include "air-math.h"
#include <assert.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define LAB_MIN_VIEW_WIDTH 100
#define LAB_MIN_VIEW_HEIGHT 60
struct wlr_box { int x, y, width, height; };
struct view_size_hints {
    int min_width, min_height, max_width, max_height;
    int base_width, base_height, width_inc, height_inc;
};
struct view;
struct view_impl { void (*configure)(struct view *, struct wlr_box); };
struct view {
    bool floating, shaded;
    const struct view_impl *impl;
    unsigned pending_configure_serial;
    struct wlr_box current, pending;
    struct view_size_hints hints;
};
static bool view_is_floating(struct view *view) { return view->floating; }
static struct view_size_hints view_get_size_hints(struct view *view) { return view->hints; }
static uint64_t clock_ns = 1000000000;
static uint64_t now_ns(void) { return clock_ns; }
static unsigned configures;
static void view_move_resize(struct view *view, struct wlr_box geometry) {
    configures++;
    view->pending = geometry;
    view->pending_configure_serial = configures;
}
static const struct view_impl impl = {.configure = view_move_resize};
static struct {
    bool native_left_down;
    uint64_t epoch, ack;
    double theta, height;
    struct {
        uint32_t region;
        double x, y;
        struct air_panel plane;
        struct wlr_box initial, requested;
        uint64_t resize_sent_at;
    } manipulation;
} xr;

static void aim(double x, double y) {
    struct air_vec point = apoint(&xr.manipulation.plane, x, y);
    xr.theta = atan2(point.x, point.z);
    xr.height = AIR_RADIUS * point.y / hypot(point.x, point.z);
}
'''
        suffix = r'''
int main(void) {
    struct view view = {
        .floating = true, .impl = &impl, .current = {2000, 100, 900, 600},
        .hints = {.min_width = 600, .min_height = 400, .max_width = 1350, .max_height = 800},
    };
    xr.epoch = xr.ack = 1;
    xr.native_left_down = true;
    xr.manipulation.initial = xr.manipulation.requested = view.current;
    xr.manipulation.plane = (struct air_panel){
        .width = 900, .height = 600, .bw = 900, .bh = 600,
        .distance = AIR_RADIUS, .metres_per_pixel = .0006,
    };
    xr.manipulation.region = AIR_EDGE_RIGHT | AIR_EDGE_BOTTOM;
    xr.manipulation.x = 906;
    xr.manipulation.y = 606;

    aim(906, 606);
    assert(!spatial_resize_window(&view) && configures == 0);
    aim(996, 666);
    assert(spatial_resize_window(&view) && configures == 1);
    assert(view.pending.width == 1080 && view.pending.height == 720);
    assert(view.pending.x == 1910 && view.pending.y == 40);
    assert(view.current.width == 900 && view.current.height == 600);

    aim(1100, 750);
    clock_ns += 40000000;
    assert(!spatial_resize_window(&view) && configures == 1);

    view.current = view.pending;
    view.pending_configure_serial = 0;
    xr.epoch++;
    assert(!spatial_resize_window(&view) && configures == 1);
    xr.ack = xr.epoch;
    assert(spatial_resize_window(&view) && configures == 2);
    /* Height limit constrains both axes. */
    assert(view.pending.width == 1200 && view.pending.height == 800);

    /* A quick release still sends the final size over an in-flight configure. */
    aim(951, 636);
    xr.native_left_down = false;
    assert(spatial_resize_window(&view) && configures == 3);
    assert(view.pending.width == 990 && view.pending.height == 660);
    assert(view.pending.x == 1955 && view.pending.y == 70);
    assert(!spatial_resize_window(&view) && configures == 3);

    view.floating = false;
    aim(996, 666);
    assert(!spatial_resize_window(&view) && configures == 3);
    view.floating = true;
    view.shaded = true;
    assert(!spatial_resize_window(&view) && configures == 3);
    view.shaded = false;

    const struct {
        uint32_t edge;
        double x, y, dx, dy;
        struct wlr_box expected;
    } edges[] = {
        {AIR_EDGE_RIGHT, 906, 300, 50, 200, {1950, 100, 1000, 600}},
        {AIR_EDGE_LEFT, -6, 300, -50, -200, {1950, 100, 1000, 600}},
        {AIR_EDGE_TOP, 450, -46, 200, -45, {2000, 55, 900, 690}},
        {AIR_EDGE_BOTTOM, 450, 606, -200, 45, {2000, 55, 900, 690}},
    };
    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        xr.manipulation.region = edges[i].edge;
        xr.manipulation.x = edges[i].x;
        xr.manipulation.y = edges[i].y;
        xr.manipulation.requested = xr.manipulation.initial;
        aim(edges[i].x + edges[i].dx, edges[i].y + edges[i].dy);
        assert(spatial_resize_window(&view));
        assert(view.pending.x == edges[i].expected.x && view.pending.y == edges[i].expected.y);
        assert(view.pending.width == edges[i].expected.width);
        assert(view.pending.height == edges[i].expected.height);
    }

    int width, height;
    struct air_resize_limits cells = {
        .min_width = 600, .min_height = 400, .max_width = 1350, .max_height = 800,
        .width_inc = 9, .height_inc = 6,
    };
    assert(aresize_client_size(900, 600, 1.23, cells, &width, &height));
    assert(width % 9 == 0 && height % 6 == 0 && width * 2 == height * 3);
    assert(aresize_client_dimension(1400, .0006, 600, 1350, 4, 9, &width));
    assert(width == 1345);
}
'''
        with tempfile.TemporaryDirectory(prefix='air-client-resize-') as directory:
            path = Path(directory) / 'check.c'
            path.write_text(prefix + function(source, 'spatial_can_resize') +
                            function(source, 'spatial_resize_window') + suffix)
            binary = path.with_name('check')
            subprocess.run([
                'cc', '-std=c17', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'native/common'), str(path), '-lm', '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
