"""Actual tracker MCU helpers, with report queues instead of a USB device."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parent


def function(source, signature):
    a=source.index(signature)
    return source[a:source.index('\n}',a)+2]


class MCUTransactionTests(unittest.TestCase):
    def compile_run(self, source):
        with tempfile.TemporaryDirectory(prefix='air-mcu-check-') as d:
            p=Path(d)/'check.c';p.write_text(source)
            out=Path(d)/'check'
            subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror','-I',str(ROOT/'native/common'),str(p),'-o',str(out)],check=True)
            subprocess.run([str(out)],check=True)

    def test_reply_wait_skips_oled_other_commands_and_corrupt_frames_with_bounded_timeout(self):
        low=(ROOT/'.local/upstream/XRLinuxDriver/modules/xrealInterfaceLibrary/interface_lib/src/mcu_hid.c').read_text()
        a=low.index('struct __attribute__((__packed__)) mcu_hid_packet_t')
        b=low.index('\nstatic bool send_payload(',a)
        self.compile_run(r'''
#include "air-screen.h"
#include <assert.h>
#define MAX_PACKET_SIZE 64
#define le16toh(x) (x)
#define device_mcu_error(x) ((void)(x))
typedef struct {void *handle;} mcu_hid_device;
static unsigned char reports[8][64];
static int sizes[8],count,read_count,observed_on;
static int hid_read_timeout(void *h,unsigned char *p,size_t n,int ms) {
 (void)h; assert(n==64 && ms==25); int i=read_count++;
 if(i>=count)return 0;
 memcpy(p,reports[i],sizes[i]);return sizes[i];
}
static void air_screen_observe(const void *p,size_t n) {if(ascreen_packet(p,n)==1)observed_on++;}
static void packet(int i,unsigned id,unsigned value) {
 unsigned char *p=reports[i];memset(p,0,64);p[0]=0xfd;p[5]=19;p[15]=id;p[16]=id>>8;p[23]=value;sizes[i]=24;
 uint32_t crc=UINT32_MAX;for(int j=5;j<24;j++){crc^=p[j];for(int k=0;k<8;k++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));}
 crc=~crc;for(int j=0;j<4;j++)p[1+j]=crc>>(8*j);
}
'''+low[a:b]+function(low,'static bool recv_payload(')+'\n'+function(low,'static bool recv_payload_msg(')+r'''
int main(void) {
 mcu_hid_device d={0};unsigned char mode=99;
 packet(0,0x6c09,0); /* replace with captured, CRC-verified OLED-on log */
 unsigned char on[]={0xfd,0xa0,0x14,0x51,0xcd,0x1d,0,0,0,0,0,0xbf,0xfc,0x36,0x4c,9,0x6c,0,0,0,0,0,0x4f,0x70,0x65,0x6e,0x20,0x4f,0x4c,0x45,0x44,0x20,0x32,0x44};
 memcpy(reports[0],on,sizeof(on));sizes[0]=sizeof(on);
 packet(1,0x1234,7);packet(2,0x3319,8);reports[2][1]^=1;packet(3,0x3319,3);count=4;
 assert(recv_payload_msg(&d,0x3319,1,&mode));assert(mode==3 && read_count==4 && observed_on==1);
 unsigned char version[41];read_count=0;packet(0,0x26,65);count=1;
 assert(recv_payload_msg(&d,0x26,sizeof(version),version));assert(version[0]==65);
 for(unsigned j=1;j<sizeof(version);j++)assert(version[j]==0);
 read_count=0;count=0;mode=99;assert(!recv_payload_msg(&d,0x3319,1,&mode));assert(read_count==64 && mode==99);
 read_count=0;packet(0,0x3319,3);reports[0][5]=18; /* invalid CRC/length */
 count=1;assert(!recv_payload_msg(&d,0x3319,1,&mode));assert(mode==99);
}
''')

    def test_full_length_firmware_log_does_not_disconnect_controller(self):
        low=(ROOT/'.local/upstream/XRLinuxDriver/modules/xrealInterfaceLibrary/interface_lib/src/mcu_hid.c').read_text()
        a=low.index('case DEVICE_MCU_MSG_P_ASYNC_TEXT_LOG: {')
        b=low.index('case DEVICE_MCU_MSG_P_END_HEARTBEAT:',a)
        body=low[a:b].split('{',1)[1].rsplit('}',1)[0]
        self.compile_run(r'''
#include <string.h>
#include <assert.h>
#include <stddef.h>
#define MCU_HID_EVENT_MESSAGE 1
struct output {int event;char msg[64];};
static int process(struct output *out) {
 struct {char text[42];char guard[8];} packet;
 memset(&packet,'X',sizeof(packet));packet.guard[7]=0;
 size_t length=59,data_len=17;
'''+body+r'''
}
int main(void) {
 struct output out={0};assert(process(&out)==1);
 assert(out.event==1 && strlen(out.msg)==42);
 for(int i=0;i<42;i++)assert(out.msg[i]=='X');
}
''')

    def test_mode_write_uses_four_byte_value_and_full_report_id_zero_frame(self):
        low=(ROOT/'.local/upstream/XRLinuxDriver/modules/xrealInterfaceLibrary/interface_lib/src/mcu_hid.c').read_text()
        a=low.index('struct __attribute__((__packed__)) mcu_hid_packet_t')
        b=low.index('static bool send_payload(',a)
        self.compile_run(r'''
#include "air-screen.h"
#include <assert.h>
#define MAX_PACKET_SIZE 64
#define PACKET_HEAD 0xfd
#define DEVICE_MCU_MSG_W_DISP_MODE 8
#define htole16(x) (x)
#define htole32(x) (x)
#define htole64(x) (x)
#define device_mcu_error(x) ((void)(x))
typedef struct {void *handle;unsigned char disp_mode;} mcu_hid_device;
static unsigned char captured[65];static bool partial,ack=true;static int acknowledgements;
static int hid_write(void *h,const unsigned char *p,size_t n) {
 (void)h;assert(n==65);memcpy(captured,p,n);return partial?64:65;
}
static bool recv_payload_msg(mcu_hid_device *d,uint16_t id,uint8_t len,uint8_t *data) {
 (void)d;assert(id==8 && len==0 && data==NULL);acknowledgements++;return ack;
}
static uint32_t crc32_checksum(const uint8_t *p,unsigned n) {
 uint32_t crc=UINT32_MAX;for(unsigned i=0;i<n;i++){crc^=p[i];for(int b=0;b<8;b++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));}return ~crc;
}
'''+low[a:b]+function(low,'bool mcu_hid_update_display_mode(')+r'''
int main(void) {
 mcu_hid_device d={.handle=(void*)1,.disp_mode=3};
 assert(mcu_hid_update_display_mode(&d));assert(acknowledgements==1);
 assert(captured[0]==0 && captured[1]==0xfd && captured[6]==21 && captured[7]==0);
 assert(captured[8]==0 && captured[9]==0x91 && captured[16]==8 && captured[17]==0);
 assert(captured[23]==3 && captured[24]==0 && captured[25]==0 && captured[26]==0);
 assert(ascreen_valid_packet(captured+1,64));
 for(int i=27;i<65;i++)assert(captured[i]==0);
 partial=true;assert(!mcu_hid_update_display_mode(&d));assert(acknowledgements==1);
 partial=false;ack=false;assert(!mcu_hid_update_display_mode(&d));
 d.disp_mode=1;ack=true;assert(mcu_hid_update_display_mode(&d));assert(captured[23]==1);
}
''')

    def test_requested_mode_survives_old_cached_read_and_is_not_confirmation(self):
        source=(ROOT/'.local/upstream/XRLinuxDriver/src/devices/xreal.c').read_text()
        self.compile_run(r'''
#include <stdatomic.h>
#include <stdbool.h>
#include <assert.h>
#define MAPPED_DISPLAY_MODE_COUNT 2
static const int sbs_display_modes[]={3,4},non_sbs_display_modes[]={1,2};
static bool connected=true,mcu_enabled=true;
static struct {int disp_mode;} controller={1},*glasses_controller=&controller;
static atomic_int requested_display_mode=-1,confirmed_display_mode=1;
'''+function(source,'int get_display_mode_index(')+'\n'+function(source,'bool xreal_device_is_sbs_mode(')+'\n'+function(source,'bool xreal_device_set_sbs_mode(')+r'''
int main(void) {
 assert(xreal_device_set_sbs_mode(true));assert(!xreal_device_is_sbs_mode());
 assert(xreal_device_set_sbs_mode(false));assert(atomic_load(&requested_display_mode)==1);
 assert(xreal_device_set_sbs_mode(true));
 controller.disp_mode=1; /* old normal event used to overwrite the request */
 assert(atomic_load(&requested_display_mode)==3);
 atomic_store(&confirmed_display_mode,atomic_exchange(&requested_display_mode,-1));
 assert(xreal_device_is_sbs_mode());
 assert(xreal_device_set_sbs_mode(false));assert(xreal_device_is_sbs_mode());
 assert(atomic_load(&requested_display_mode)==1);
 connected=false;assert(!xreal_device_set_sbs_mode(true));assert(!xreal_device_is_sbs_mode());
}
''')
