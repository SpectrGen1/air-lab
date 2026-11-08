#!/usr/bin/env python3
"""Compile the actual upstream connection function against hardware-free mocks."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / '.local/upstream/XRLinuxDriver/src/devices/xreal.c').read_text()
start = source.index('bool xreal_device_connect() {')
end = source.index('\n};', start) + 3
connection = source[start:end]
prefix = r'''
#include <assert.h>
#include <stdlib.h>
#include "device_imu.h"
#include "device_mcu.h"
static bool use_hid_transport = false, mcu_enabled = true;
static bool connected, mcu_heartbeat_required;
static device_imu_type *glasses_imu;
static device_mcu_type *glasses_controller;
struct properties {bool sbs_mode_supported;};
typedef struct properties device_properties_type;
static device_properties_type properties;
static device_properties_type *device_checkout(void){return &properties;}
static void device_checkin(device_properties_type *p){(void)p;}
#define handle_xreal_event NULL
#define handle_xreal_controller_event NULL
#define sleep(x) ((void)(x))
device_mcu_error_type device_mcu_open_hid(device_mcu_type *d, device_mcu_event_callback cb){(void)d;(void)cb;return DEVICE_MCU_ERROR_NO_ERROR;}
device_mcu_error_type device_mcu_clear(device_mcu_type *d){(void)d;return DEVICE_MCU_ERROR_NO_ERROR;}
device_mcu_error_type device_mcu_close(device_mcu_type *d){(void)d;return DEVICE_MCU_ERROR_NO_ERROR;}
device_imu_error_type device_imu_open_hid(device_imu_type *d, device_imu_event_callback cb){(void)d;(void)cb;return DEVICE_IMU_ERROR_NO_ERROR;}
device_imu_error_type device_imu_open_xreal_one(device_imu_type *d, device_imu_event_callback cb){return device_imu_open_hid(d,cb);}
device_imu_error_type device_imu_clear(device_imu_type *d){(void)d;return DEVICE_IMU_ERROR_NO_ERROR;}
device_imu_error_type device_imu_close(device_imu_type *d){(void)d;return DEVICE_IMU_ERROR_NO_ERROR;}
device_imu_error_type device_imu_calibrate(device_imu_type *d,uint32_t n,bool g,bool a,bool m){
 (void)d;(void)n;(void)g;(void)a;(void)m;
 /* Connection may happen during a genuine turn. It must never store it as bias. */
 assert(!"blind startup calibration captures real head movement");
 return DEVICE_IMU_ERROR_NO_ERROR;
}
'''
suffix = r'''
int main(void){
 assert(xreal_device_connect()); free(glasses_imu);
 use_hid_transport = true;
 assert(xreal_device_connect()); free(glasses_imu);free(glasses_controller);
}
'''
with tempfile.TemporaryDirectory(prefix='air-tracker-startup-') as directory:
    file = Path(directory) / 'startup.c'
    file.write_text(prefix + connection + suffix)
    binary = Path(directory) / 'startup'
    subprocess.run(['cc', '-std=c17', '-I', str(ROOT / '.local/upstream/XRLinuxDriver/modules/xrDeviceKit/include'),
                    str(file), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: actual XREAL connection preserves factory calibration instead of learning startup movement.')
