"""Quiet regression for persistent spatial layouts and overview geometry."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_native_cursor import function

ROOT = Path(__file__).resolve().parent


class WorkspaceTests(unittest.TestCase):
    def test_state_round_trip_and_corruption(self):
        source = (ROOT / 'native/labwc/air-spatial.c').read_text()
        prefix = r'''
#define _GNU_SOURCE
#include "air-workspace.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static struct {
    struct air_workspace_state workspace_state;
} xr;
'''
        suffix = r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!setenv("AIR_SPATIAL_WORKSPACE_STATE", argv[1], 1));
    xr.workspace_state = (struct air_workspace_state){
        .magic = AIR_LAYOUT_MAGIC, .count = 1, .reading = 1,
        .windows = {{
            .workspace = "Work", .app = "org.example.Editor", .title = "Main",
            .placement = {.angle = -.15, .height = .2, .distance = 1.7,
                          .scale = 1.25, .pinned = 1, .client_width = 1100, .client_height = 680},
        }},
    };
    assert(save_workspace_state());
    struct stat sb;
    assert(!stat(argv[1], &sb) && (sb.st_mode & 0777) == 0600);
    xr.workspace_state = (struct air_workspace_state){0};
    load_workspace_state();
    const struct air_saved_window *restored = &xr.workspace_state.windows[0];
    assert(xr.workspace_state.reading == 1 && xr.workspace_state.count == 1);
    assert(!strcmp(restored->workspace, "Work") && !strcmp(restored->app, "org.example.Editor"));
    assert(restored->placement.angle == -.15 && restored->placement.height == .2);
    assert(restored->placement.distance == 1.7 && restored->placement.scale == 1.25);
    assert(restored->placement.pinned == 1 && restored->placement.client_width == 1100);
    assert(restored->placement.client_height == 680);
    xr.workspace_state.windows[0].placement.distance = NAN;
    assert(save_workspace_state());
    load_workspace_state();
    assert(xr.workspace_state.magic == AIR_LAYOUT_MAGIC && !xr.workspace_state.count);
    FILE *broken = fopen(argv[1], "wb");
    assert(broken && fputs("invalid", broken) >= 0 && fclose(broken) == 0);
    load_workspace_state();
    assert(xr.workspace_state.magic == AIR_LAYOUT_MAGIC && !xr.workspace_state.count);
}
'''
        with tempfile.TemporaryDirectory(prefix='air-layout-state-') as directory:
            path = Path(directory) / 'test.c'
            path.write_text(prefix + function(source, 'save_workspace_state') +
                            function(source, 'load_workspace_state') + suffix)
            binary = path.with_suffix('')
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'native/common'), path, '-lm', '-o', binary], check=True)
            subprocess.run([binary, Path(directory) / 'layout'], check=True)
