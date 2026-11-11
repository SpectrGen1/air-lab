"""Actual presenter upload path: damage, row padding, storage reuse, and alpha."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_native_cursor import function

ROOT=Path(__file__).resolve().parent

class CaptureUploadTests(unittest.TestCase):
    def test_partial_upload_preserves_pixels_and_reuses_storage(self):
        source=(ROOT/'native/presenter/presenter.c').read_text()
        program=r'''
#include "air-pixels.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#define GLuint unsigned
#define GL_TEXTURE_2D 1
#define GL_TEXTURE_MAG_FILTER 2
#define GL_LINEAR 3
#define GL_TEXTURE_WRAP_S 4
#define GL_TEXTURE_WRAP_T 5
#define GL_CLAMP_TO_EDGE 6
#define GL_RGBA 7
#define GL_UNSIGNED_BYTE 8
struct capture {
 bool has_capture_panel;
 GLuint texture;uint32_t allocated_width,allocated_height,upload_width,upload_height,stride,format;
 unsigned char *rgba;size_t rgba_capacity;void *map;struct air_pixel_damage damage;
};
struct ext_image_copy_capture_frame_v1;
static unsigned images,subimages,mipmaps,tw,th;
static unsigned char stored[256];
static void fatal(const char *s){fprintf(stderr,"%s\n",s);abort();}
#pragma GCC diagnostic ignored "-Wunused-parameter"
static void glGenTextures(int n,GLuint *id){assert(n==1);*id=7;}
static void glBindTexture(int target,GLuint id){assert(id==7);}
static void glTexParameteri(int target,int key,int value){}
static void glTexImage2D(int target,int level,int internal,unsigned w,unsigned h,int border,int fmt,int type,const void *data){
 images++;tw=w;th=h;assert(w*h*4<=sizeof(stored));memcpy(stored,data,w*h*4);
}
static void glTexSubImage2D(int target,int level,unsigned x,unsigned y,unsigned w,unsigned h,int fmt,int type,const void *data){
 subimages++;assert(x+w<=tw&&y+h<=th);
 for(unsigned row=0;row<h;row++)memcpy(stored+((y+row)*tw+x)*4,(const unsigned char *)data+row*w*4,w*4);
}
static void afilter_texture(unsigned w,unsigned h){mipmaps++;}
''' +function(source,'capture_damage')+function(source,'capture_upload')+r'''
int main(void){
 unsigned char src[24];memset(src,9,sizeof(src));
 for(unsigned y=0;y<2;y++)for(unsigned x=0;x<3;x++){
  src[y*12+x*3]=20+x;src[y*12+x*3+1]=40+y;src[y*12+x*3+2]=60+x;
 }
 struct capture c={.allocated_width=3,.allocated_height=2,.stride=12,.format=WL_SHM_FORMAT_BGR888,.map=src};
 capture_upload(&c);assert(images==1&&!subimages&&c.rgba_capacity==24);
 assert(stored[0]==20&&stored[1]==40&&stored[2]==60&&stored[3]==255);
 unsigned char *staging=c.rgba;
 src[15]=230;src[16]=231;src[17]=232;
 capture_damage(&c,NULL,1,1,1,1);capture_upload(&c);
 assert(images==1&&subimages==1&&c.rgba==staging&&c.rgba_capacity==24);
 assert(stored[16]==230&&stored[17]==231&&stored[18]==232&&stored[19]==255);
 assert(stored[0]==20&&stored[8]==22&&stored[20]==22); /* Undamaged texels remain. */
 c.damage=(struct air_pixel_damage){0};
 capture_damage(&c,NULL,0,0,1,1);capture_damage(&c,NULL,2,1,1,1);
 assert(c.damage.width==3&&c.damage.height==2);capture_upload(&c);assert(images==1&&subimages==2);
 c.damage=(struct air_pixel_damage){0};
 capture_damage(&c,NULL,INT_MAX,INT_MAX,INT_MAX,INT_MAX);assert(!c.damage.width);
 capture_damage(&c,NULL,-1,-1,2,2);assert(c.damage.x==0&&c.damage.y==0&&c.damage.width==1&&c.damage.height==1);
 c.allocated_width=4;c.damage=(struct air_pixel_damage){1,1,1,1};capture_upload(&c);
 assert(images==2&&c.upload_width==4&&c.rgba_capacity==32); /* Resize needs full upload. */
 c.upload_width=0;capture_upload(&c);assert(images==3); /* Replaced SHM buffer. */
 unsigned char argb[]={3,2,1,4};c.map=argb;c.allocated_width=c.allocated_height=1;
 c.stride=4;c.format=WL_SHM_FORMAT_ARGB8888;capture_upload(&c);
 assert(stored[0]==1&&stored[1]==2&&stored[2]==3&&stored[3]==4);
 assert(mipmaps==6);free(c.rgba);
}
'''
        with tempfile.TemporaryDirectory(prefix='air-upload-') as directory:
            path=Path(directory)/'upload.c';path.write_text(program)
            binary=Path(directory)/'upload'
            subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror','-I',str(ROOT/'native/common'),str(path),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
