#!/usr/bin/env python3
"""Back up and install the standalone Preview launcher and selected MIME defaults."""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess

spec = importlib.util.spec_from_file_location('finder_install', Path(__file__).with_name('install.py'))
finder = importlib.util.module_from_spec(spec); spec.loader.exec_module(finder)
HOME = Path.home()
STATE = HOME / '.local/state/kubuntu-customization'
MIMES = ('application/pdf', 'image/png', 'image/jpeg', 'image/svg+xml', 'image/webp', 'image/bmp', 'image/tiff')
DESKTOP = 'org.local.preview.desktop'

def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()

def paths():
    return {HOME/'.local/bin/preview':0o755, HOME/'.local/share/applications'/DESKTOP:0o644, HOME/'.config/mimeapps.list':0o644}

def defaults():
    return {mime:run('xdg-mime','query','default',mime) for mime in MIMES}

def contents():
    binary = HOME/'.local/share/finder-v2/bin/finder'
    if not binary.is_file(): raise RuntimeError('Install the verified Finder binary first.')
    mime = HOME/'.config/mimeapps.list'; lines = mime.read_text().splitlines(keepends=True) if mime.exists() else []
    result=[]; section=''; found=False; seen=set()
    def missing():
        if result and not result[-1].endswith('\n'):result[-1]+='\n'
        for key in MIMES:
            if key not in seen:result.append(key+'='+DESKTOP+';\n');seen.add(key)
    for line in lines:
        text=line.strip()
        if text.startswith('[') and text.endswith(']'):
            if section=='Default Applications':missing()
            section=text[1:-1]
            if section=='Default Applications':
                if found:raise RuntimeError('Duplicate Default Applications sections require review.')
                found=True
        key=text.partition('=')[0].strip()
        if section=='Default Applications' and key in MIMES:
            if key not in seen:result.append(key+'='+DESKTOP+';\n');seen.add(key)
        else:result.append(line)
    if not found:
        if result and not result[-1].endswith('\n'):result[-1]+='\n'
        result.append('[Default Applications]\n')
    missing()
    wrapper=('#!/bin/sh\nexec "'+str(binary)+'" --preview -- "$@"\n').encode()
    entry=('[Desktop Entry]\nType=Application\nName=Preview\nGenericName=Image and PDF Viewer\n'
        'Comment=View images and documents\nExec='+str(HOME/'.local/bin/preview')+' %U\nIcon=image-x-generic\n'
        'Terminal=false\nCategories=Graphics;Viewer;\nMimeType='+';'.join(MIMES)+';\n').encode()
    return {HOME/'.local/bin/preview':(wrapper,0o755),HOME/'.local/share/applications'/DESKTOP:(entry,0o644),mime:(''.join(result).encode(),mime.stat().st_mode&0o777 if mime.exists() else 0o644)}

def prepare():
    os.umask(0o077); proposed=contents(); state=STATE/datetime.datetime.now().strftime('finder-preview-%Y%m%d-%H%M%S-%f')
    finder.checked_path(state,HOME); state.mkdir(parents=True)
    previous=[]; staged=[]
    import shutil
    for path,(data,mode) in proposed.items():
        before=finder.path_state(path)
        if before['existed']:
            backup=state/'before'/path.relative_to(HOME);backup.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,backup);before['backup']=str(backup)
        previous.append(before)
        target=state/'proposed'/path.relative_to(HOME);target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data);target.chmod(mode)
        staged.append({'path':str(path),'staged':str(target),'hash':finder.digest(target),'mode':mode})
    binary=HOME/'.local/share/finder-v2/bin/finder'
    manifest={'schema':1,'state':'prepared','files':previous,'proposed':staged,'defaults_before':defaults(),'binary_hash':finder.digest(binary),'writes':[]}
    finder.save_manifest(state,manifest)
    (state/'ROLLBACK.md').write_text('Restore the previous viewer integration:\n\npython3 '+str(Path(__file__).resolve())+' --restore '+str(state)+'\n')
    print(state);return state

def load(state):
    state=finder.checked_path(Path(state).absolute(),STATE)
    if state.parent!=STATE or not state.name.startswith('finder-preview-'):raise RuntimeError('Expected this installer’s private backup.')
    manifest=json.loads((state/'manifest.json').read_text())
    if manifest.get('schema')!=1:raise RuntimeError('Unsupported manifest.')
    allowed={str(p) for p in paths()}
    for key in ('files','proposed'):
        items=manifest[key]
        if len(items)!=len(allowed) or {v['path'] for v in items}!=allowed:raise RuntimeError('Unexpected deployment scope.')
    for item in manifest['files']:
        finder.checked_path(item['path'],HOME)
        if item['existed'] and Path(item['backup'])!=state/'before'/Path(item['path']).relative_to(HOME):raise RuntimeError('Unexpected backup path.')
    for item in manifest['proposed']:
        if Path(item['staged'])!=state/'proposed'/Path(item['path']).relative_to(HOME):raise RuntimeError('Unexpected staged path.')
        finder.checked_path(item['staged'],state)
    return state,manifest

def verify(state):
    state,manifest=load(state)
    for item in manifest['proposed']:
        if finder.path_state(Path(item['path']))['hash']!=item['hash']:raise RuntimeError('Installed viewer integration differs from staging.')
    actual=defaults()
    if any(value!=DESKTOP for value in actual.values()):raise RuntimeError('Viewer MIME defaults differ from staging.')
    if finder.digest(HOME/'.local/share/finder-v2/bin/finder')!=manifest['binary_hash']:raise RuntimeError('Finder binary changed during viewer installation.')
    return {'passed':True,'defaults':actual,'binary_hash':manifest['binary_hash']}

def apply(state):
    state,manifest=load(state)
    if manifest['state']!='prepared':raise RuntimeError('This preparation was already used.')
    finder.validate_snapshot(manifest['files']);finder.validate_backups(state,manifest)
    if defaults()!=manifest['defaults_before']:raise RuntimeError('Viewer defaults changed since preparation.')
    if finder.digest(HOME/'.local/share/finder-v2/bin/finder')!=manifest['binary_hash']:raise RuntimeError('Finder binary changed since preparation.')
    for item in manifest['proposed']:
        path=Path(item['staged'])
        if finder.digest(path)!=item['hash'] or path.stat().st_mode&0o777!=item['mode']:raise RuntimeError('Staged contents changed.')
    manifest['state']='installing';finder.save_manifest(state,manifest)
    for item in manifest['proposed']:
        manifest['writes'].append(item['path']);finder.save_manifest(state,manifest)
        finder.atomic_write(Path(item['path']),Path(item['staged']).read_bytes(),item['mode'])
    run('kbuildsycoca6','--noincremental')
    report=verify(state);manifest.update(state='applied',verification=report);finder.save_manifest(state,manifest)
    print(json.dumps({'backup':str(state),'verification':report},indent=2))

def restore(state):
    state,manifest=load(state);finder.validate_backups(state,manifest)
    written=set(manifest['writes'])
    for item in manifest['proposed']:
        if item['path'] in written and finder.path_state(Path(item['path']))['hash']!=item['hash']:raise RuntimeError('Installed file changed; refusing to overwrite it.')
    for item in manifest['files']:
        if item['path'] not in written:continue
        path=Path(item['path'])
        if item['existed']:finder.atomic_write(path,Path(item['backup']).read_bytes(),item['mode'])
        else:path.unlink(missing_ok=True)
    run('kbuildsycoca6','--noincremental');manifest['state']='restored';finder.save_manifest(state,manifest)
    print(json.dumps({'restored':True,'backup':str(state)},indent=2))

def main():
    parser=argparse.ArgumentParser(description=__doc__);group=parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--prepare',action='store_true');group.add_argument('--apply',type=Path);group.add_argument('--verify',type=Path);group.add_argument('--restore',type=Path);args=parser.parse_args()
    if args.prepare:prepare()
    elif args.apply:apply(args.apply)
    elif args.restore:restore(args.restore)
    else:print(json.dumps(verify(args.verify),indent=2))
if __name__=='__main__':main()
