"""Quiet MCU screen parsing/publishing tests; never open a device or window."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parent


class ScreenStateTests(unittest.TestCase):
    def test_owned_fresh_screen_state_and_raw_mcu_packets(self):
        # Reconstructed complete captured Open OLED 2D packet: missing usbmon
        # text tail is verified by the device's original CRC, not assumed.
        on = bytes.fromhex('fda01451cd1d0000000000bffc364c096c00000000004f70656e204f4c4544203244')
        off = bytes.fromhex('fda0a99d21120000000000a32c8b4c046c000000000000')
        for p in (on, off):
            self.assertEqual(zlib.crc32(p[5:]), int.from_bytes(p[1:5], 'little'))
        def declaration(name, p):
            return 'unsigned char '+name+'[] = {'+','.join(map(str, p))+'};\n'
        low = (ROOT / '.local/upstream/XRLinuxDriver/modules/xrealInterfaceLibrary/interface_lib/src/mcu_hid.c').read_text()
        start = low.index('static bool recv_payload(')
        receive = low[start:low.index('\nstatic bool send_payload_action(',start)]
        mocks = declaration('captured_on', on) + r'''
#include <stdbool.h>
#define MAX_PACKET_SIZE 64
#define device_mcu_error(msg) ((void)(msg))
typedef struct {void *handle;} mcu_hid_device;
static int hid_read_timeout(void *handle, unsigned char *data, size_t n, int timeout) {
 (void)handle; assert(timeout==25); assert(n==64); memcpy(data,captured_on,sizeof(captured_on)); return sizeof(captured_on);
}
'''
        source = r'''
#include "air-screen-publish.h"
#include <assert.h>
'''+mocks+receive+r'''
int main(int argc, char **argv) {
 assert(argc == 2);
'''+declaration('on', on)+declaration('off', off)+r'''
 assert(ascreen_packet(on,sizeof(on)) == 1);
 assert(ascreen_packet(off,sizeof(off)) == 0);
 for(size_t n=0;n<sizeof(on);n++) assert(ascreen_packet(on,n)==-1);
 on[2]^=1; assert(ascreen_packet(on,sizeof(on))==-1); on[2]^=1;
 assert(ascreen_packet(NULL,0)==-1);
 assert(ascreen_fresh(42,100,1,42,200)==1);
 assert(ascreen_fresh(42,100,0,42,200)==0);
 assert(ascreen_fresh(42,100,-1,42,200)==-1);
 assert(ascreen_fresh(42,100,1,43,200)==-1);
 assert(ascreen_fresh(42,100,1,42,99)==-1);
 assert(ascreen_fresh(42,100,1,42,3000000101ULL)==-1);
 assert(ascreen_read(argv[1],200)==-1);
 FILE *f=fopen(argv[1],"w"); assert(f);
 fprintf(f,"{\"pid\":%ld}",(long)getpid()); assert(!fclose(f));
 assert(!setenv("AIR_TRACKER_READY_FILE",argv[1],1));
 air_screen_observe(NULL,0); /* Startup is unknown; never assume glasses on. */
 struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
 uint64_t now=(uint64_t)t.tv_sec*1000000000+t.tv_nsec;
 assert(ascreen_read(argv[1],now)==-1);
 unsigned char reply[27];mcu_hid_device device={0};
 assert(recv_payload(&device,sizeof(reply),reply)); /* Async on during a short command reply. */
 clock_gettime(CLOCK_MONOTONIC,&t);now=(uint64_t)t.tv_sec*1000000000+t.tv_nsec;
 assert(ascreen_read(argv[1],now)==1);
 air_screen_observe(off,sizeof(off));
 clock_gettime(CLOCK_MONOTONIC,&t);now=(uint64_t)t.tv_sec*1000000000+t.tv_nsec;
 assert(ascreen_read(argv[1],now)==0);
 air_screen_observe(on,sizeof(on));
 clock_gettime(CLOCK_MONOTONIC,&t);now=(uint64_t)t.tv_sec*1000000000+t.tv_nsec;
 assert(ascreen_read(argv[1],now)==1);
 /* Captured wear-event shape: physical=1, virtual=1, value at byte 30. */
 unsigned char wear[34]={0xfd};wear[5]=29;wear[15]=5;wear[16]=0x6c;wear[22]=1;wear[26]=1;wear[30]=1;
 for(int value=1;value>=0;value--) {
  wear[30]=value;uint32_t crc=UINT32_MAX;
  for(unsigned i=5;i<sizeof(wear);i++){crc^=wear[i];for(int bit=0;bit<8;bit++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));}
  crc=~crc;for(int i=0;i<4;i++)wear[1+i]=crc>>(8*i);
  assert(ascreen_wear_packet(wear,sizeof(wear))==value);
  air_screen_observe(wear,sizeof(wear));
  air_screen_observe(off,sizeof(off)); /* SBS closes OLED, but is not removal. */
  clock_gettime(CLOCK_MONOTONIC,&t);now=(uint64_t)t.tv_sec*1000000000+t.tv_nsec;
  assert(ascreen_read(argv[1],now)==value);
 }
 assert(ascreen_read(argv[1],now+3000000001ULL)==-1);
 f=fopen(argv[1],"w");assert(f);fputs("{\"pid\":0}",f);fclose(f);
 assert(ascreen_read(argv[1],now)==-1);
}
'''
        with tempfile.TemporaryDirectory(prefix='air-screen-check-') as d:
            d = Path(d)
            file = d / 'screen.c'
            file.write_text(source)
            subprocess.run(['cc','-std=c17','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror',
                            '-I',str(ROOT/'native/common'),'-I',str(ROOT/'native/tracker'),
                            str(file),'-o',str(d/'screen')],check=True)
            subprocess.run([str(d/'screen'),str(d/'tracker.json')],check=True)
