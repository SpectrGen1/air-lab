"""Actual presenter smoothing/recenter path, with no sensor or display connection."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_native_cursor import function
ROOT=Path(__file__).resolve().parent

class PoseSmoothingTests(unittest.TestCase):
    def test_recenter_horizon_and_real_sample_clock(self):
        source=(ROOT/'native/presenter/presenter.c').read_text()
        program=r'''
#define _POSIX_C_SOURCE 200809L
#include "air-pose.h"
#include <assert.h>
#include <stdlib.h>
struct app {bool zero_valid;double raw[9],zero[9],basis[9];
 uint64_t pose_stamp;struct air_pose_filter pose_filter;};
''' +function(source,'update_basis')+r'''
int main(void) {
 struct app a={0};a.pose_stamp=1000000000ULL;
 abasis(.6,.2,.1,a.raw);update_basis(&a);
 for(int i=0;i<9;i++)assert(fabs(a.basis[i]-(i%4==0))<1e-10);
 double raw[9];abasis(.601,.201,.11,a.raw);memcpy(raw,a.raw,sizeof(raw));
 a.pose_stamp+=1000000;update_basis(&a);
 assert(a.pose_stamp==1001000000ULL&&!memcmp(raw,a.raw,sizeof(raw)));
 assert(a.pose_filter.stamp==a.pose_stamp&&fabs(a.basis[1])<1e-10);
 double basis[9];memcpy(basis,a.basis,sizeof(basis));
 update_basis(&a);assert(!memcmp(a.basis,basis,sizeof(basis)));
 /* Recenter while the filter trails a moving pose must give exact neutral. */
 a.zero_valid=false;update_basis(&a);
 for(int i=0;i<9;i++)assert(fabs(a.basis[i]-(i%4==0))<1e-10);
 /* A new neutral must not interpolate from the old workspace reference. */
 abasis(-2,.6,-.4,a.raw);a.zero_valid=false;update_basis(&a);
 for(int i=0;i<9;i++)assert(fabs(a.basis[i]-(i%4==0))<1e-10);
 setenv("AIR_SPATIAL_HEAD_ROLL","1",1);
 a.pose_stamp+=500000000;abasis(-2,.6,-.2,a.raw);update_basis(&a);
 double expected[9];arelative_basis(a.raw,a.zero,true,expected);
 for(int i=0;i<9;i++)assert(fabs(a.basis[i]-expected[i])<1e-10);
 assert(a.pose_stamp==1501000000ULL);
}
'''
        with tempfile.TemporaryDirectory(prefix='air-pose-') as d:
            path=Path(d)/'pose.c';path.write_text(program);binary=Path(d)/'pose'
            subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror','-I',str(ROOT/'native/common'),str(path),'-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
