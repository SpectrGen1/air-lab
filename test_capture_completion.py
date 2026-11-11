"""Protocol regression: image-copy ready/failed complete a buffer without release."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


def function(source, name):
    start = source.index('static void ' + name + '(')
    cursor = source.index('{', start)
    depth = 1
    end = cursor + 1
    while depth:
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
        end += 1
    return source[start:end]


class TestCaptureCompletion(unittest.TestCase):
    def test_capture_completion_does_not_wait_for_wl_buffer_release(self):
        source = (ROOT / 'native/presenter/presenter.c').read_text()
        prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
struct ext_image_copy_capture_frame_v1 {int unused;};
struct capture {
 struct ext_image_copy_capture_frame_v1 *frame;
 bool ready, dirty, released, closed;
};
#define EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED 2
static int retried, freed;
static void ext_image_copy_capture_frame_v1_destroy(struct ext_image_copy_capture_frame_v1 *f){(void)f;}
static void free_buffer(struct capture *c){(void)c;freed++;}
static void request_capture(struct capture *c){if(c->released&&!c->closed)retried++;}
'''
        suffix = r'''
int main(void){
 struct ext_image_copy_capture_frame_v1 frame;
 struct capture c={.frame=&frame,.released=false};
 capture_ready(&c,&frame);
 assert(c.ready&&c.dirty&&c.released&&!c.frame);
 c=(struct capture){.frame=&frame,.released=false};
 capture_failed(&c,&frame,1);
 assert(!c.frame&&!c.ready&&c.released&&!c.closed&&freed==1&&retried==1);
 c=(struct capture){.frame=&frame,.released=false};
 capture_failed(&c,&frame,2);
 assert(c.closed&&retried==1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / 'capture.c'
            binary = Path(directory) / 'capture'
            file.write_text(prefix + function(source, 'capture_ready') + function(source, 'capture_failed') + suffix)
            subprocess.run(['cc', '-std=c17', str(file), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, capture_output=True)


class TestDeferredCapture(unittest.TestCase):
    def test_scene_transition_retries_without_waiting_for_another_dirty_frame(self):
        source = (ROOT / 'native/presenter/presenter.c').read_text()
        prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
struct app { uint64_t perf_upload_ns; };
struct capture { struct app *app; bool wanted,dirty; unsigned texture,allocated_width,allocated_height,stride,format; void *map; };
static uint64_t now_ns(void){return 1;}
static int attempts,uploads;
static void start_capture(struct capture *c){(void)c;}
static void capture_upload(struct capture *c){(void)c;uploads++;}
static void request_capture(struct capture *c){if(c->wanted&&!c->dirty)attempts++;}
'''
        suffix = r'''
int main(void){
 struct app a={0};
 struct capture c={.app=&a,.wanted=false,.dirty=false};
 prepare_capture(&c);assert(c.wanted&&attempts==1&&uploads==0);
 c.dirty=true;prepare_capture(&c);assert(!c.dirty&&attempts==2&&uploads==1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            file=Path(directory)/'retry.c'
            binary=Path(directory)/'retry'
            file.write_text(prefix+function(source,'prepare_capture')+suffix)
            subprocess.run(['cc','-std=c17',str(file),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
