"""Publish the selected hardware mode/IPD; mode writes are diagnostic opt-in only."""
import json
import math
import os
from pathlib import Path
import re
import subprocess
import time


class StereoMode:
    def __init__(self, ready, config, config_path, run=subprocess.run, clock=time.monotonic, wall_clock=time.time, switch_modes=False):
        self.switch_modes = switch_modes
        self.ready = Path(ready)
        self.config = config
        self.config_path = Path(config_path)
        self.run, self.clock, self.wall_clock = run, clock, wall_clock
        self.connection_epoch = 0
        self.was_connected = False
        self.original_sbs = None
        self.original_output = None
        self.changed = False
        self.last_command = -100.
        self.last_output = -100.
        self.last_ipd = float(config.get('ipd_mm', 64))

    @staticmethod
    def driver_state():
        try:
            p = Path('/dev/shm/xr_driver_state')
            age = time.time() - p.stat().st_mtime
            if age < 0 or age > 3:
                return {}
            wanted = {'sbs_mode_enabled', 'sbs_mode_supported', 'connected_device_brand', 'connected_device_model'}
            stamp = p.stat().st_mtime
            result = {k:v for line in p.read_text().splitlines() if '=' in line
                      for k,v in [line.split('=',1)] if k in wanted}
            result['_observed_mtime'] = stamp
            return result
        except OSError:
            return {}

    def command(self, enabled):
        # IN_CLOSE_WRITE is the driver's documented control-file notification;
        # rename alone would not notify it. O_EXCL never overwrites other controls.
        p = Path('/dev/shm/xr_driver_control')
        try:
            fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        except FileExistsError:
            return False
        with os.fdopen(fd, 'w') as f:
            f.write('sbs_mode=' + ('enable' if enabled else 'disable') + '\n')
        self.last_command = self.clock()
        return True

    def outputs(self, display):
        env = os.environ | {'WAYLAND_DISPLAY': display}
        r = self.run(['wlr-randr', '--json'], env=env, capture_output=True, text=True, timeout=2)
        if r.returncode:
            return []
        data = json.loads(r.stdout)
        return [o for o in data if o.get('make','').lower() in ('nreal','xreal')
                and o.get('model','').lower() in ('nreal air','xreal air')]

    def configure(self, display, enabled):
        if self.clock() - self.last_output < 1:
            return False
        self.last_output = self.clock()
        matches = self.outputs(display)
        if len(matches) != 1:
            return
        o = matches[0]
        if self.original_output is None:
            return
        width,height = (3840,1080) if enabled else (
            self.original_output['mode']['width'], self.original_output['mode']['height'])
        current = next((m for m in o.get('modes',[]) if m.get('current')),None)
        saved=self.original_output
        correct_layout = (o.get('scale')==1 and o.get('transform')=='normal') if enabled else (
            o.get('scale')==saved['scale'] and o.get('transform')==saved['transform'] and o.get('position')==saved['position'])
        if current and (current['width'],current['height']) == (width,height) and o.get('enabled') and correct_layout:
            return True
        mode = next((m for m in o.get('modes',[]) if (m['width'],m['height']) == (width,height)
                     and abs(m.get('refresh',60)-(60 if enabled else self.original_output['mode']['refresh']))<.1),None)
        if not mode:
            return
        args = ['wlr-randr','--output',o['name'],'--on','--mode',f'{width}x{height}@{mode["refresh"]:g}Hz']
        if enabled:
            args += ['--scale','1','--transform','normal']
        if not enabled:
            saved = self.original_output
            args += ['--scale',str(saved['scale']),'--transform',saved['transform'],
                     '--pos',f'{saved["position"]["x"]},{saved["position"]["y"]}']
        r = self.run(args,env=os.environ|{'WAYLAND_DISPLAY':display},capture_output=True,text=True,timeout=2)
        if r.returncode:
            print('Air stereo: waiting for the identified Air output mode.',flush=True)
        else:
            self.changed=True
        return not r.returncode

    def publish_ipd(self, compositor_pid):
        try:
            config=json.loads(self.config_path.read_text())
            total=float(config.get('ipd_mm',64))
            center=float(config.get('ipd_center_mm',0))
            baseline=float(config.get('ipd_baseline_mm',total))
            if not (all(math.isfinite(v) for v in (total,center,baseline)) and
                    45<=total<=80 and 45<=baseline<=80 and abs(center)<=5):
                return
            path=Path(str(self.ready)+'.ipd')
            temp=path.with_suffix('.ipd.tmp')
            temp.write_text(f'{compositor_pid} {time.monotonic_ns()} {total:.6f} {center:.6f} {baseline:.6f} {self.last_ipd:.6f}\n')
            temp.replace(path)
        except (OSError,ValueError,TypeError):
            return

    def update(self, compositor_pid, tracker_pid, connected):
        self._update(compositor_pid,tracker_pid,connected)
        self.publish_ipd(compositor_pid)

    def _update(self, compositor_pid, tracker_pid, connected):
        if connected and not self.was_connected:
            self.connection_epoch = self.wall_clock()
        self.was_connected = connected
        state = self.driver_state() if connected else {}
        known = (state.get('sbs_mode_enabled') in ('true','false') and state.get('sbs_mode_supported') == 'true'
                 and state.get('connected_device_brand') == 'XREAL' and state.get('connected_device_model') == 'Air'
                 and state.get('_observed_mtime', 0) >= self.connection_epoch)
        is_sbs = state.get('sbs_mode_enabled') == 'true'
        # Publish only the current session's tracker-derived state, never a
        # requested mode. Native entry additionally checks real output dimensions.
        mode = int(is_sbs) if known else -1
        path = Path(str(self.ready)+'.stereo')
        temp = path.with_suffix('.stereo.tmp')
        temp.write_text(f'{tracker_pid} {time.monotonic_ns()} {mode}\n')
        temp.replace(path)
        try:
            pid, wanted, ipd, display = Path(str(self.ready)+'.controls').read_text().split()
            ipd = float(ipd)
            if int(pid)!=compositor_pid or wanted not in ('0','1') or not math.isfinite(ipd) or not 45<=ipd<=80:
                return
            if not re.fullmatch(r'wayland-[0-9]+',display):
                return
        except (OSError,ValueError):
            return
        if ipd != self.last_ipd:
            # Merge only this preference into the latest manifest, preserving a
            # concurrently staged release and all other settings.
            current = json.loads(self.config_path.read_text())
            current['ipd_mm'] = ipd
            tmp = self.config_path.with_name(f'.native-ipd-{os.getpid()}.tmp')
            tmp.write_text(json.dumps(current,indent=2)+'\n')
            tmp.replace(self.config_path)
            self.last_ipd = ipd
        if not self.switch_modes:
            return  # Production follows the physical selection; never change it.
        desired = wanted=='1'
        if desired and known and self.original_sbs is None:
            self.original_sbs = is_sbs
        if desired and self.original_output is None:
            matches = self.outputs(display)
            if len(matches)!=1:
                return
            o=matches[0]
            mode=next((m for m in o.get('modes',[]) if m.get('current')),None)
            if not mode:
                return
            self.original_output={'mode':mode,'scale':o['scale'],'transform':o['transform'],'position':o['position']}
        target = True if desired else self.original_sbs
        if known and target is not None:
            if is_sbs != target and self.clock()-self.last_command>=5:
                if self.command(target):
                    self.changed=True
            elif is_sbs == target and (desired or self.changed):
                restored = self.configure(display, desired)
                if not desired and restored:
                    self.original_sbs=None
                    self.original_output=None
                    self.changed=False

    def restore(self):
        if not self.switch_modes:
            return
        if self.changed and self.original_sbs is not None:
            # Drain an already queued control before restoring, without replacing
            # another writer's file or terminating the sole reader too early.
            deadline = time.monotonic() + 1.5
            while not self.command(self.original_sbs) and time.monotonic() < deadline:
                time.sleep(.05)
