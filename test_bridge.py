"""Run: python3 -m unittest -v. Hardware-independent tests; grim is a fixture."""
import base64
import http.client
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
from urllib.parse import urlsplit
from bridge import decode_pose

class TestPacket(unittest.TestCase):
    def test_formats(self):
        for buf in (struct.pack('=6d',1,2,3,10,-15,.1),struct.pack('=6dI',1,2,3,10,-15,.1,99)):
            self.assertEqual(decode_pose(buf),(10,-15,.1))
    def test_invalid(self):
        for buf in (b'',b'x'*51,struct.pack('=6d',0,0,0,float('inf'),0,0),struct.pack('=6d',0,0,0,0,float('nan'),0)):
            self.assertIsNone(decode_pose(buf))

class TestLocalBridge(unittest.TestCase):
    def test_endpoints(self):
        here=Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as d:
            d=Path(d); png=base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jfq0AAAAASUVORK5CYII=')
            (d/'sample').write_bytes(png);g=d/'grim';g.write_text('#!/bin/sh\n[ "$6" = "chosen" ] || exit 2\nexec cat "$(dirname "$0")/sample"\n');g.chmod(0o700)
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
                s.bind(('127.0.0.1',0));udp_port=s.getsockname()[1]
            srv=subprocess.Popen([sys.executable,str(here/'bridge.py'),'--pose','--udp-port',str(udp_port),'--window','chosen'],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,env=dict(os.environ,PATH=str(d)+os.pathsep+os.environ['PATH']))
            try:
                u=urlsplit(srv.stdout.readline().strip());self.assertEqual(u.hostname,'127.0.0.1')
                def get(path,host=None):
                    c=http.client.HTTPConnection(u.hostname,u.port,timeout=3);c.request('GET',path,headers={'Host':host or u.netloc});return c,c.getresponse()
                for path,host,status in [('/bad/config',None,404),(u.path+'config','untrusted',404),(u.path+'frame/1',None,404),(u.path+'config',None,200),(u.path+'frame/0',None,200)]:
                    c,r=get(path,host);self.assertEqual(r.status,status);body=r.read();c.close()
                    if status==200 and path.endswith('frame/0'): self.assertEqual(body,png)
                    elif status==200: self.assertEqual(json.loads(body),{'count':1,'pose':True})
                c,r=get(u.path+'pose')
                with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sender:
                    sender.sendto(struct.pack('=6dI',0,0,0,20,-4,8,1),('127.0.0.1',udp_port))
                for _ in range(10):
                    line=r.readline()
                    if line.startswith(b'data: '): break
                self.assertEqual(json.loads(line[6:])['angles'],[20,-4,8]);r.close();c.close()
            finally:
                srv.terminate();srv.wait(timeout=4);srv.stdout.close()
if __name__=='__main__':unittest.main()
