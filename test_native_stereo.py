"""Quiet mode transaction tests. All output and device control calls are mocked."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('native_stereo',ROOT/'scripts/native_stereo.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)


class StereoTransactionTests(unittest.TestCase):
    def setUp(self):
        self.directory=tempfile.TemporaryDirectory();self.addCleanup(self.directory.cleanup)
        self.path=Path(self.directory.name)
        self.ready=self.path/'tracker.json'
        self.manifest=self.path/'manifest.json'
        self.manifest.write_text(json.dumps({'ipd_mm':64,'labwc':'preserve-new-release'}))
        self.time=10.;self.commands=[];self.calls=[]
        self.state={'sbs_mode_enabled':'false','sbs_mode_supported':'true',
                    'connected_device_brand':'XREAL','connected_device_model':'Air','_observed_mtime':1}
        self.output={'name':'DP-4','make':'Nreal','model':'nreal air','enabled':True,
                     'scale':1.,'transform':'normal','position':{'x':6880,'y':0},
                     'modes':[{'width':1920,'height':1080,'refresh':60.,'current':True},
                              {'width':3840,'height':1080,'refresh':60.,'current':False}]}
        self.manager=module.StereoMode(self.ready,{'ipd_mm':64},self.manifest,run=self.run_command,clock=lambda:self.time,wall_clock=lambda:1,switch_modes=True)
        self.manager.driver_state=lambda:dict(self.state)
        self.manager.command=self.command

    def run_command(self,cmd,**kwargs):
        self.calls.append(cmd)
        if cmd==['wlr-randr','--json']:
            return subprocess.CompletedProcess(cmd,0,json.dumps([self.output]),'')
        if '--mode' in cmd:
            width,height=map(int,cmd[cmd.index('--mode')+1].split('@')[0].split('x'))
            for mode in self.output['modes']:
                mode['current']=(mode['width'],mode['height'])==(width,height)
        if '--scale' in cmd:self.output['scale']=float(cmd[cmd.index('--scale')+1])
        if '--transform' in cmd:self.output['transform']=cmd[cmd.index('--transform')+1]
        return subprocess.CompletedProcess(cmd,0,'','')

    def command(self,enabled):
        self.commands.append(enabled);self.manager.last_command=self.time;return True

    def request(self,wanted,ipd=64,pid=12,display='wayland-0'):
        Path(str(self.ready)+'.controls').write_text(f'{pid} {int(wanted)} {ipd} {display}\n')

    def update(self):
        self.manager.update(12,34,True)

    def test_switch_once_confirm_real_state_configure_full_sbs_and_restore(self):
        self.request(True);self.update();self.assertEqual(self.commands,[True])
        self.time+=1;self.update();self.assertEqual(self.commands,[True])
        self.state['sbs_mode_enabled']='true';self.time+=1;self.update()
        changes=[c for c in self.calls if '--output' in c]
        self.assertEqual(len(changes),1)
        self.assertIn('3840x1080@60Hz',changes[0]);self.assertEqual(changes[0][2],'DP-4')
        self.assertEqual(Path(str(self.ready)+'.stereo').read_text().split()[::2],['34','1'])
        self.request(False);self.time+=5;self.update();self.assertEqual(self.commands,[True,False])
        self.state['sbs_mode_enabled']='false';self.time+=2;self.update()
        self.assertIn('1920x1080@60Hz',[c for c in self.calls if '--output' in c][-1])

    def test_invalid_owner_range_or_display_cannot_change_outputs(self):
        for pid,ipd,display in [(99,64,'wayland-0'),(12,float('nan'),'wayland-0'),(12,90,'wayland-0'),(12,64,'other')]:
            self.request(True,ipd,pid,display);self.update()
        self.assertEqual(self.commands,[]);self.assertEqual(self.calls,[])

    def test_ipd_save_merges_only_preference_and_restoration_on_exit(self):
        self.request(True,65.5);self.update()
        self.assertEqual(json.loads(self.manifest.read_text()),{'ipd_mm':65.5,'labwc':'preserve-new-release'})
        self.manager.restore();self.assertEqual(self.commands,[True,False])

    def test_other_model_is_not_eligible_and_unknown_state_is_not_confirmed(self):
        self.state['connected_device_model']='Air 2';self.request(True);self.update()
        self.assertEqual(self.commands,[])
        self.assertEqual(Path(str(self.ready)+'.stereo').read_text().split()[-1],'-1')

    def test_initial_sbs_restores_original_scale_instead_of_forcing_mono(self):
        self.state['sbs_mode_enabled']='true'
        self.output['modes'][0]['current']=False;self.output['modes'][1]['current']=True
        self.output['scale']=2
        self.request(True);self.update();self.assertEqual(self.commands,[])
        self.output['scale']=1
        self.request(False);self.time+=2;self.update()
        changes=[c for c in self.calls if '--output' in c]
        self.assertEqual(changes[-1][changes[-1].index('--scale')+1],'2')
        self.manager.restore();self.assertEqual(self.commands,[])

    def test_previous_reader_state_does_not_confirm_current_stereo(self):
        self.state['_observed_mtime']=0
        self.request(True);self.update()
        self.assertEqual(self.commands,[])
        self.assertEqual(Path(str(self.ready)+'.stereo').read_text().split()[-1],'-1')

    def test_default_passive_policy_publishes_actual_mode_but_never_switches_it(self):
        passive=module.StereoMode(self.ready,{'ipd_mm':64},self.manifest,run=self.run_command,clock=lambda:self.time,wall_clock=lambda:1)
        passive.driver_state=lambda:dict(self.state)
        passive.command=self.command
        self.request(True,65)
        passive.update(12,34,True)
        self.assertEqual(self.commands,[]);self.assertEqual(self.calls,[])
        self.assertEqual(json.loads(self.manifest.read_text())['ipd_mm'],65)
        self.assertEqual(Path(str(self.ready)+'.stereo').read_text().split()[-1],'0')
        self.state['sbs_mode_enabled']='true';self.request(False)
        passive.update(12,34,True);passive.restore()
        self.assertEqual(Path(str(self.ready)+'.stereo').read_text().split()[-1],'1')
        self.assertEqual(self.commands,[]);self.assertEqual(self.calls,[])

    def test_eye_profile_publication_and_adjustment_preserve_measured_baseline(self):
        profile={'ipd_mm':63.5,'ipd_center_mm':.25,'ipd_baseline_mm':63.5,
                 'ipd_measurement':{'far_left_mm':31.5,'far_right_mm':32.0,'near_total_mm':60.0}}
        self.manifest.write_text(json.dumps(profile))
        passive=module.StereoMode(self.ready,profile,self.manifest,run=self.run_command)
        passive.driver_state=lambda:dict(self.state)
        self.request(False,62)
        passive.update(12,34,True)
        saved=json.loads(self.manifest.read_text())
        self.assertEqual(saved,profile|{'ipd_mm':62})
        fields=Path(str(self.ready)+'.ipd').read_text().split()
        self.assertEqual(fields[0],'12');self.assertEqual(list(map(float,fields[2:])),[62,.25,63.5,62])
        # An external profile replacement must survive the unchanged old controls.
        self.manifest.write_text(json.dumps(profile))
        passive.update(12,34,True)
        self.assertEqual(json.loads(self.manifest.read_text()),profile)
        fields=Path(str(self.ready)+'.ipd').read_text().split()
        self.assertEqual(list(map(float,fields[2:])),[63.5,.25,63.5,62])
        self.assertEqual(self.calls,[]);self.assertEqual(self.commands,[])
        before=Path(str(self.ready)+'.ipd').read_text()
        self.manifest.write_text(json.dumps(profile|{'ipd_center_mm':float('nan')}))
        passive.update(12,34,True)
        self.assertEqual(Path(str(self.ready)+'.ipd').read_text(),before)
