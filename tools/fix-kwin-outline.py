#!/usr/bin/env python3
"""Exclude Finder's client contour from KDE Rounded Corners' duplicate outline.
Only Round-Corners/Exclusions changes. Existing entries/settings are preserved.
"""
import argparse, json, os, subprocess
from pathlib import Path
from datetime import datetime

def run(*command):return subprocess.check_output(command,text=True).strip()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply',action='store_true')
    args=parser.parse_args()
    config=Path(os.environ.get('XDG_CONFIG_HOME',str(Path.home()/'.config')))/'kwinrc'
    current=run('kreadconfig6','--file',str(config),'--group','Round-Corners','--key','Exclusions')
    values=[value for value in current.split(',') if value]
    if 'org.local.finder' in values:
        print(json.dumps({'changed':False,'finderExcluded':True}));return
    proposed=','.join(values+['org.local.finder'])
    if not args.apply:
        print(json.dumps({'key':'Round-Corners/Exclusions','before':current,'after':proposed}));return
    state=Path(os.environ.get('XDG_STATE_HOME',str(Path.home()/'.local/state')))/'macos-finder-linux'
    state.mkdir(parents=True,exist_ok=True)
    backup=state/('kwin-outline-'+datetime.now().strftime('%Y%m%d-%H%M%S-%f')+'.json')
    backup.write_text(json.dumps({'config':str(config),'key':'Exclusions','before':current,'after':proposed},indent=2)+'\n');backup.chmod(0o600)
    subprocess.run(['kwriteconfig6','--file',str(config),'--group','Round-Corners','--key','Exclusions',proposed],check=True)
    assert run('kreadconfig6','--file',str(config),'--group','Round-Corners','--key','Exclusions')==proposed
    subprocess.run(['qdbus6','org.kde.KWin','/KWin','org.kde.KWin.reconfigure'],check=True)
    print(json.dumps({'changed':True,'finderExcluded':True,'backup':str(backup)}))
if __name__=='__main__':main()
