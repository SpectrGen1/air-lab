#!/usr/bin/env python3
"""Loopback-only lab for an original Air. No input injection or output control."""
import argparse
import hmac
import json
import math
import secrets
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

HERE = Path(__file__).resolve().parent

def decode_pose(data):
    # XRLinuxDriver sends native-endian doubles, optionally followed by a u32 frame.
    if len(data) not in (48, 52):
        return None
    angles = struct.unpack_from('=6d', data)[3:]
    return angles if all(math.isfinite(x) and abs(x) < 10000 for x in angles) else None

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pose', action='store_true', help='accept OpenTrack UDP from the same machine')
    p.add_argument('--udp-port', type=int, default=4243)
    p.add_argument('--http-port', type=int, default=0, help='default: choose a free port')
    p.add_argument('--window', action='append', default=[], metavar='ID', help='grim -T ID, repeat up to three times (center, left, right)')
    opt = p.parse_args()
    if len(opt.window) > 3: p.error('three window slots maximum')
    if not 1 <= opt.udp_port <= 65535 or not 0 <= opt.http_port <= 65535: p.error('invalid port')
    if opt.window and not shutil.which('grim'): p.error('grim (with -T support) is required for --window')
    udp = None
    if opt.pose:
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try: udp.bind(('127.0.0.1', opt.udp_port))
        except OSError as e: p.error(f'cannot bind local UDP port: {e}')
    stop = threading.Event()
    guard = threading.Lock()
    sample = [0, None, 0.0]
    prefix = '/' + secrets.token_urlsafe(24) + '/'
    cap_locks = [threading.Lock() for _ in opt.window]
    cap_times = [0.0 for _ in opt.window]
    html = (HERE / 'index.html').read_bytes()
    def receive():
        udp.settimeout(.3)
        while not stop.is_set():
            try: data = udp.recv(1024)
            except socket.timeout: continue
            except OSError: return
            angles = decode_pose(data)
            if angles is not None:
                with guard: sample[:] = [sample[0] + 1, angles, time.monotonic()]
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args): pass  # Never log credentials or captured IDs.
        def reply(self, code, mime, body=b''):
            self.send_response(code); self.send_header('Content-Type', mime)
            self.send_header('Cache-Control', 'no-store'); self.send_header('Referrer-Policy','no-referrer')
            self.send_header('X-Content-Type-Options','nosniff'); self.send_header('X-Frame-Options','DENY')
            self.send_header('Content-Length', str(len(body))); self.end_headers()
            try: self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError): pass
        def do_GET(self):
            path = urlsplit(self.path).path
            n = len(prefix)
            if self.headers.get('Host') != f'127.0.0.1:{self.server.server_port}' or not hmac.compare_digest(prefix.encode(), path[:n].encode()):
                return self.reply(404,'text/plain')
            route = path[n:]
            if route in ('', 'index.html'): return self.reply(200,'text/html; charset=utf-8',html)
            if route == 'config': return self.reply(200,'application/json',json.dumps({'count':len(opt.window),'pose':opt.pose}).encode())
            if route == 'pose' and opt.pose:
                self.send_response(200); self.send_header('Content-Type','text/event-stream'); self.send_header('Cache-Control','no-store'); self.end_headers()
                last, nextbeat = -1, 0.0
                while not stop.is_set():
                    with guard: seq, angles, stamp = sample
                    now = time.monotonic(); msg = None
                    if seq != last and angles is not None:
                        msg = 'data: '+json.dumps({'angles': angles, 'age': now - stamp},separators=(',',':'))+'\n\n'; last = seq
                    elif now > nextbeat:
                        msg = ': heartbeat\n\n'; nextbeat = now + 1
                    if msg:
                        try: self.wfile.write(msg.encode()); self.wfile.flush()
                        except (BrokenPipeError, ConnectionResetError, OSError): return
                    stop.wait(1/90)
                return
            if route.startswith('frame/') and route[6:] in ('0','1','2'):
                i = int(route[6:])
                if i >= len(opt.window): return self.reply(404,'text/plain')
                if not cap_locks[i].acquire(False): return self.reply(429,'text/plain')
                try:
                    if time.monotonic() - cap_times[i] < .75: return self.reply(429,'text/plain')
                    cap_times[i] = time.monotonic()
                    # argv only; never accept a window ID from an HTTP client.
                    try: cp = subprocess.run(['grim','-t','png','-l','1','-T', opt.window[i], '-'], capture_output=True,timeout=3.0,check=False)
                    except (OSError,subprocess.TimeoutExpired): return self.reply(503,'text/plain', b'Capture tool failed or timed out')
                    if cp.returncode or not cp.stdout.startswith(b'\x89PNG\r\n\x1a\n'): return self.reply(503,'text/plain', b'Capture unavailable; check grim -T manually')
                    return self.reply(200,'image/png',cp.stdout)
                finally: cap_locks[i].release()
            self.reply(404,'text/plain')
    try: http = ThreadingHTTPServer(('127.0.0.1', opt.http_port), Handler)
    except OSError as e: p.error(f'cannot bind local HTTP port: {e}')
    if udp: threading.Thread(target=receive, daemon=True).start()
    print(f'http://127.0.0.1:{http.server_port}{prefix}', flush=True)
    print('Open the private URL locally. Ctrl+C stops the lab. No display settings are changed.', file=sys.stderr, flush=True)
    try: http.serve_forever()
    except KeyboardInterrupt: pass
    finally:
        stop.set(); http.server_close()
        if udp: udp.close()
if __name__ == '__main__': main()
