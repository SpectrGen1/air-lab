"""Quiet cursor regressions; compile actual compositor routing without Wayland."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


def function(source, name):
    start = re.search(r'^(?:static )?(?:bool|void|int) ' + name + r'\([^;{]*\)\s*\{',source,re.M).start()
    end = source.index('{',start)+1
    depth = 1
    while depth:
        depth += (source[end]=='{') - (source[end]=='}')
        end += 1
    return source[start:end]


class SpatialCursorTests(unittest.TestCase):
    def test_video_hide_blank_pending_commit_and_destroy_keep_visible_cursor(self):
        source = (ROOT/'native/labwc/air-spatial.c').read_text()
        prefix = r'''
#include "air-cursor.h"
#include "air-protocol.h"
#include <assert.h>
#include <drm_fourcc.h>
#define WLR_BUFFER_DATA_PTR_ACCESS_READ 1
struct wl_listener {void (*notify)(struct wl_listener *,void *);int link;};
struct wlr_buffer {int width,height;uint32_t format;size_t stride;void *pixels;bool readable;};
struct client_buffer {struct wlr_buffer *source;};
struct wlr_surface {struct client_buffer *buffer;struct {int scale;} current;struct {int commit,destroy;} events;};
struct wlr_xcursor_image {void *buffer;int width,height,hotspot_x,hotspot_y;};
struct wlr_xcursor {int image_count;struct wlr_xcursor_image **images;};
static struct {struct wlr_surface *cursor_surface;int cursor_hot_x,cursor_hot_y;struct wl_listener cursor_commit,cursor_destroy;struct {uint32_t region;} manipulation;uint32_t hover_region;bool native_left_down;} xr;
static struct {struct {void *xcursor_manager;struct {bool active;} drag;} seat;} server;
static bool active=true, theme_missing;
static int shown,adds,removes,unmaps;static uint32_t wire_format;
static bool air_spatial_active(void){return active;}
static void wl_list_remove(int *p){(void)p;removes++;}
static void wl_signal_add(int *signal,struct wl_listener *l){(void)signal;(void)l;adds++;}
static bool wlr_buffer_begin_data_ptr_access(struct wlr_buffer *b,int access,void **pixels,uint32_t *format,size_t *stride){
 (void)access;if(!b->readable)return false;*pixels=b->pixels;*format=b->format;*stride=b->stride;return true;
}
static void wlr_buffer_end_data_ptr_access(struct wlr_buffer *b){(void)b;unmaps++;}
static unsigned char theme_pixel[]={255,255,255,255};
static struct wlr_xcursor_image image={theme_pixel,1,1,0,0};
static struct wlr_xcursor_image *images[]={&image};
static struct wlr_xcursor theme={1,images};
static struct wlr_xcursor *wlr_xcursor_manager_get_xcursor(void *m,const char *name,int scale){
 (void)m;(void)name;(void)scale;return theme_missing?NULL:&theme;
}
static bool cursor_pixels(const void *pixels,int w,int h,int stride,int hx,int hy,uint32_t format,float scale){
 (void)hx;(void)hy;(void)scale;
 if(!acursor_visible(pixels,w,h,stride,format==1||format==DRM_FORMAT_XBGR8888))return false;
 shown++;wire_format=format;return true;
}
bool air_spatial_cursor(struct wlr_surface *,int,int);
'''
        functions = '\n'.join(function(source,name) for name in (
            'cursor_detach','cursor_named','air_spatial_cursor_name','cursor_committed','cursor_destroyed','air_spatial_cursor'))
        suffix = r'''
int main(void){
 assert(air_spatial_cursor(NULL,0,0)&&shown==1); /* Video's null cursor request. */
 struct wlr_surface surface={.current={.scale=1}};
 assert(air_spatial_cursor(&surface,2,3));assert(xr.cursor_surface==&surface&&adds==2);
 assert(!removes); /* Pending-buffer fallback must keep the commit listener. */
 unsigned char pixel[]={1,2,3,255};
 struct wlr_buffer buffer={1,1,DRM_FORMAT_ARGB8888,4,pixel,true};
 struct client_buffer client={&buffer};surface.buffer=&client;
 int before=shown;cursor_committed(NULL,NULL);assert(shown==before+1&&wire_format==0);
 assert(xr.cursor_surface==&surface&&!removes);
 pixel[3]=0;before=shown;cursor_committed(NULL,NULL);assert(shown==before+1); /* Transparent CSS cursor. */
 pixel[3]=255;before=shown;cursor_committed(NULL,NULL);assert(shown==before+1&&!removes);
 buffer.format=DRM_FORMAT_XBGR8888;pixel[3]=0;cursor_committed(NULL,NULL);assert(wire_format==DRM_FORMAT_XBGR8888);
 buffer.format=DRM_FORMAT_XRGB8888;cursor_committed(NULL,NULL);assert(wire_format==1);
 buffer.format=0xffffffff;before=shown;cursor_committed(NULL,NULL);assert(shown==before+1); /* Unsupported upload. */
 buffer.readable=false;before=shown;cursor_committed(NULL,NULL);assert(shown==before+1);
 assert(xr.cursor_surface==&surface); /* Later client commits can still restore shape. */
 before=shown;cursor_destroyed(NULL,NULL);assert(shown==before+1&&!xr.cursor_surface&&removes==2);
 theme_missing=true;before=shown;assert(air_spatial_cursor(NULL,0,0));assert(shown==before+1); /* Built-in arrow. */
 unsigned char padded[16]={0};padded[7]=255;assert(!acursor_visible(padded,1,2,8,false));
 padded[11]=3;assert(acursor_visible(padded,1,2,8,false));padded[11]=2;assert(!acursor_visible(padded,1,2,8,false));
 active=false;before=shown;assert(!air_spatial_cursor(NULL,0,0)&&shown==before); /* Desktop policy unchanged. */
 assert(unmaps>=6);
}
'''
        with tempfile.TemporaryDirectory(prefix='air-cursor-check-') as d:
            path=Path(d)/'cursor.c';path.write_text(prefix+functions+suffix)
            flags=subprocess.check_output(['pkg-config','--cflags','libdrm'],text=True).split()
            binary=Path(d)/'cursor'
            subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror',*flags,'-I',str(ROOT/'native/common'),str(path),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
