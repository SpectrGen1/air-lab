#!/usr/bin/env python3
"""Stage desktop profiles with an optional reserved Air, without live reload."""
from datetime import datetime
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
from zoneinfo import ZoneInfo

ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('desktop_profiles',ROOT/'scripts/set-desktop-monitor-mode.py')
profiles=importlib.util.module_from_spec(spec);spec.loader.exec_module(profiles)


def main():
    home=Path.home()
    config=home/'.config/kanshi/config'
    toggle=home/'.config/waybar/scripts/monitor-toggle.sh'
    status=home/'.config/waybar/scripts/monitor-status.sh'
    before={p:p.read_text() for p in (config,toggle,status)}
    try:
        result=subprocess.run(['kanshictl','status'],capture_output=True,text=True,timeout=3)
        active=next((l.split(': ',1)[1] for l in result.stdout.splitlines() if l.startswith('Current profile: ')),'')
    except (OSError,subprocess.TimeoutExpired):
        active=''
    if active not in ('dual-static','dual-static-unplugged','lg-only','lg-only-unplugged','dp3-only'):
        # A failed/unresponsive compositor must not erase the saved preference.
        import re
        first=re.search(r'(?m)^profile\s+(dual-static|lg-only)\b',before[config])
        active=first.group(1) if first else 'dual-static'
    preferred='lg-only' if active in ('lg-only','lg-only-unplugged','dp3-only') else 'dual-static'
    updated_config=profiles.desktop_profiles(before[config],preferred)
    old='if ! kanshictl switch "$target" >/dev/null 2>&1; then'
    new='if ! python3 '+shlex.quote(str(ROOT/'scripts/set-desktop-monitor-mode.py'))+' "$target" >/dev/null 2>&1; then'
    if old not in before[toggle] and new not in before[toggle]:
        raise SystemExit('Monitor toggle was edited; cannot identify its profile switch.')
    updated_toggle=before[toggle].replace(old,new).replace('  dual-static)\n','  dual-static|dual-static-unplugged)\n').replace('  lg-only|dp3-only)\n','  lg-only|lg-only-unplugged|dp3-only)\n')
    updated_status=before[status].replace('  dual-static)\n','  dual-static|dual-xreal)\n').replace('  dual-static|dual-xreal)\n','  dual-static|dual-static-unplugged|dual-xreal)\n').replace('  lg-only|dp3-only)\n','  lg-only|lg-only-unplugged|dp3-only)\n')
    updates={config:updated_config,toggle:updated_toggle,status:updated_status}
    changed={p:s for p,s in updates.items() if before[p]!=s}
    if not changed:
        print('Desktop monitor policy is already staged. No outputs changed.')
        return
    date=datetime.now(ZoneInfo('America/Denver')).strftime('%Y%m%d-%H%M%S')
    backup=ROOT/'.local/rollback'/('desktop-monitors-'+date);backup.mkdir(parents=True)
    for p,text in changed.items():
        shutil.copy2(p,backup/(p.parent.name+'-'+p.name))
        temp=p.with_name(p.name+'.air-new')
        shutil.copy2(p,temp);temp.write_text(text)
        if p.suffix=='.sh':subprocess.run(['bash','-n',str(temp)],check=True)
        temp.replace(p)
    (backup/'manifest.json').write_text(json.dumps({'preferred':preferred,'files':[str(p) for p in changed]},indent=2)+'\n')
    print('Staged connected/unplugged Air profiles, preferred '+preferred+'. No kanshi reload or output commands issued.')
    print('Backups: '+str(backup))


if __name__=='__main__':
    main()
