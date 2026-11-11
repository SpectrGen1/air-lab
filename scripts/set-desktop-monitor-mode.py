#!/usr/bin/env python3
"""Keep desktop selection sticky and include Air when connected."""
import argparse
from pathlib import Path
import re
import subprocess


def desktop_profiles(text, preferred):
    if preferred not in ('dual-static','lg-only'):
        raise ValueError('Unknown desktop profile')
    pattern=re.compile(r'(?m)^profile\s+(dual-xreal|dual-static(?:-unplugged)?|lg-only(?:-unplugged)?)\s*\{[^{}]*\}\s*')
    blocks={m.group(1):m.group(0).rstrip() for m in pattern.finditer(text)}
    if 'dual-static' not in blocks or 'lg-only' not in blocks:
        raise ValueError('Expected dual-static and lg-only profiles')
    for name in ('dual-static', 'lg-only'):
        # Kanshi requires every connected head to match a profile directive.
        # Separate unplugged variants force reapplication on hotplug. An optional
        # head would keep the current profile and leave a new head disabled.
        # The session's SBS transaction owns 1920/3840-wide switching.
        block = re.sub(r'(?m)^\s*(?:\.\.\.)?output\s+\$xreal\b[^\n]*\n', '\n', blocks[name])
        block = block.rstrip()[:-1].rstrip() + '\n}'
        blocks[name+'-unplugged'] = block.replace('profile '+name+' {', 'profile '+name+'-unplugged {')
        air_x = 6880 if name == 'dual-static' else 3440
        blocks[name] = block[:-1] + f'    output $xreal enable position {air_x},0\n}}'
    # Match only these existing profiles; aliases and unrelated profiles survive.
    matches=list(pattern.finditer(text))
    insertion=matches[0].start()
    prefix=text[:insertion]
    remainder=pattern.sub('',text[insertion:])
    other='lg-only' if preferred=='dual-static' else 'dual-static'
    return prefix+'\n\n'.join(blocks[n] for n in (preferred,preferred+'-unplugged',other,other+'-unplugged'))+'\n\n'+remainder


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile',choices=['dual-static','lg-only'])
    parser.add_argument('--config',type=Path,default=Path.home()/'.config/kanshi/config')
    parser.add_argument('--stage-only',action='store_true')
    args=parser.parse_args()
    before=args.config.read_text();updated=desktop_profiles(before,args.profile)
    temp=args.config.with_name(args.config.name+'.air-new')
    temp.write_text(updated);temp.replace(args.config)
    if not args.stage_only:
        subprocess.run(['kanshictl','reload'],check=True,timeout=5)
        import json
        outputs=subprocess.run(['wlr-randr','--json'],check=True,capture_output=True,text=True,timeout=3)
        air=any(o.get('make','').lower() in ('nreal','xreal') and
                o.get('model','').lower() in ('nreal air','xreal air') for o in json.loads(outputs.stdout))
        target=args.profile if air else args.profile+'-unplugged'
        subprocess.run(['kanshictl','switch',target],check=True,timeout=5)


if __name__=='__main__':
    main()
