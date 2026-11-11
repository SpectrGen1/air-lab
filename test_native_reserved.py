"""Quiet routing regression: preserve desktop fallback without touching outputs."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parent


class ReservedOutputTests(unittest.TestCase):
    def test_ordinary_outputs_win_but_glasses_remain_last_resort(self):
        source=(ROOT/'native/labwc/air-spatial.c').read_text()
        a=source.index('struct output *air_spatial_desktop_output(')
        b=source.index('\n}\n',a)+3
        code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
struct output {struct output *next;bool air,usable;};
static struct {bool reserved_display;} xr;
static struct {struct {struct output *first;} outputs;} server;
#define wl_list_for_each(var,list,link) for(var=(list)->first;var;var=var->next)
static bool original_air(struct output *o){return o&&o->air;}
static bool output_is_usable(struct output *o){return o&&o->usable;}
'''+source[a:b]+r'''
int main(void){
 struct output lg={NULL,false,true},dell={&lg,false,true},air={&dell,true,true};
 server.outputs.first=&air;xr.reserved_display=true;
 assert(air_spatial_desktop_output(&air)==&dell);
 dell.usable=false;assert(air_spatial_desktop_output(&air)==&lg);
 lg.usable=false;assert(air_spatial_desktop_output(&air)==&air);
 assert(air_spatial_desktop_output(NULL)==NULL);
 xr.reserved_display=false;dell.usable=true;assert(air_spatial_desktop_output(&air)==&air);
 assert(air_spatial_desktop_output(&dell)==&dell);
}
'''
        with tempfile.TemporaryDirectory(prefix='air-reserved-check-') as d:
            file=Path(d)/'reserved.c';file.write_text(code);binary=Path(d)/'reserved'
            subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror',str(file),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
