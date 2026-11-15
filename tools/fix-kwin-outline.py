#!/usr/bin/env python3
"""Exclude Finder's client contour from KDE Rounded Corners' duplicate outline.
Only Round-Corners/Exclusions changes. Existing entries/settings are preserved.
Refresh the loaded effect itself: general KWin reconfigure leaves window caches stale.
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
    changed = 'org.local.finder' not in values
    proposed=','.join(values+['org.local.finder']) if changed else current
    if not args.apply:
        print(json.dumps({'key':'Round-Corners/Exclusions','before':current,'after':proposed,'refreshLoadedEffect':True}));return
    state=Path(os.environ.get('XDG_STATE_HOME',str(Path.home()/'.local/state')))/'macos-finder-linux'
    state.mkdir(parents=True,exist_ok=True)
    backup=state/('kwin-outline-'+datetime.now().strftime('%Y%m%d-%H%M%S-%f')+'.json')
    backup.write_text(json.dumps({'config':str(config),'key':'Exclusions','before':current,'after':proposed},indent=2)+'\n');backup.chmod(0o600)
    subprocess.run(['kwriteconfig6','--file',str(config),'--group','Round-Corners','--key','Exclusions',proposed],check=True)
    assert run('kreadconfig6','--file',str(config),'--group','Round-Corners','--key','Exclusions')==proposed
    effect='kwin4_effect_shapecorners'
    loaded=run('qdbus6','org.kde.KWin','/Effects','org.freedesktop.DBus.Properties.Get','org.kde.kwin.Effects','loadedEffects').splitlines()
    refreshed=False
    if effect in loaded:
        # reconfigureEffect reloads Config, but upstream Window::isExcluded is
        # cached without configChanged on this path. Recreate only this effect;
        # never restart KWin or change the global enable/outline settings.
        breeze=config.with_name('breezerc')
        before=breeze.read_bytes() if breeze.exists() else None
        subprocess.run(['qdbus6','org.kde.KWin','/Effects','org.kde.kwin.Effects.unloadEffect',effect],check=True,stdout=subprocess.DEVNULL)
        try:
            assert run('qdbus6','org.kde.KWin','/Effects','org.kde.kwin.Effects.loadEffect',effect)=='true'
            assert effect in run('qdbus6','org.kde.KWin','/Effects','org.freedesktop.DBus.Properties.Get','org.kde.kwin.Effects','loadedEffects').splitlines()
            refreshed=True
        finally:
            # Upstream writes three Common Breeze keys in its destructor and
            # constructor. Restore only those keys if the reload changed them.
            import configparser
            old=configparser.ConfigParser(interpolation=None);old.optionxform=str
            if before: old.read_string(before.decode())
            now=configparser.ConfigParser(interpolation=None);now.optionxform=str
            if breeze.exists(): now.read(breeze)
            for key in ('OutlineIntensity','RoundedCorners','OutlineEnabled'):
                was=old.get('Common',key,fallback=None);actual=now.get('Common',key,fallback=None)
                if was==actual: continue
                command=['kwriteconfig6','--file',str(breeze),'--group','Common','--key',key]
                subprocess.run(command+(['--delete'] if was is None else [was]),check=True)
    print(json.dumps({'changed':changed,'finderExcluded':True,'effectRefreshed':refreshed,'backup':str(backup)}))
if __name__=='__main__':main()
