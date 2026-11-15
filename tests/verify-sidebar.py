#!/usr/bin/env python3
"""Pixel and resize regression check on a private Wayland screen only."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

import dbus
from PIL import Image
from PyQt6.QtCore import Qt
from PyQt6.QtTest import QTest
from PyQt6.QtWidgets import QApplication

parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
assert os.environ.get('KUBUNTU_VIRTUAL_SCREEN') == '1'
assert os.environ['WAYLAND_DISPLAY'] == 'codex-test-screen'
assert not os.environ.get('DISPLAY')
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
for name in ('kdeglobals', 'fontconfig/fonts.conf'):
    source = Path(os.environ['KUBUNTU_WORKING_CONFIG']) / name
    target = Path(os.environ['XDG_CONFIG_HOME']) / name
    if source.is_file():
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
for name in ('MacTahoe', 'MacTahoe-dark'):
    source = Path(os.environ['KUBUNTU_WORKING_DATA']) / 'icons' / name
    if source.is_dir():
        shutil.copytree(source, Path(os.environ['XDG_DATA_HOME']) / 'icons' / name,
                        dirs_exist_ok=True, symlinks=True)
app = QApplication([])
bus = dbus.SessionBus()
records = {}
process = None
api = None


def wait_for(predicate, label, timeout=15):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        assert process.poll() is None, (out / 'finder.log').read_text()
        if predicate():
            return
        QTest.qWait(100)
    raise AssertionError((label, status()))


def status():
    return json.loads(str(api.diagnostics(timeout=3)))


def capture(name):
    QTest.qWait(200)
    state = status()
    path = out / (name + '.png')
    assert api.capture(str(path), timeout=3)
    image = Image.open(path).convert('RGBA')
    scale = image.width / state['width']
    assert state['headerHeight'] == 52, state
    assert state['navigationWidth'] == 73, state
    if state['width'] >= 980:
        assert state['viewCapsuleWidth'] == 142, state
    if state['sidebarVisible']:
        assert state['contentX'] == state['sidebarWidth'], state
        assert state['splitterGrabWidth'] >= 4, state
        seam = round(state['contentX'] * scale)
        top = round(state['bodyY'] * scale)
        bottom = round((state['bodyY'] + state['bodyHeight'] - 18) * scale)
        pixels = [image.getpixel((x, y)) for x in range(seam - 3, seam + 3)
                  for y in range(top, bottom)]
        assert all(pixel[3] > 0 for pixel in pixels), (name, 'transparent seam')
        # Every pixel in the content-side boundary must remain opaque.
        content_alpha = [image.getpixel((seam, y))[3] for y in range(top, bottom)]
        assert min(content_alpha) == 255, (name, 'content boundary alpha', min(content_alpha))
        state['checkedBoundaryPixels'] = len(pixels)
        state['minimumBoundaryAlpha'] = min(pixel[3] for pixel in pixels)
        state['minimumContentAlpha'] = min(content_alpha)
    records[name] = state
    print('PASS', name, flush=True)


try:
    with tempfile.TemporaryDirectory(prefix='finder-boundary-fixture-', dir=Path.home()) as scope:
        fixture = Path(scope) / 'Documents'
        fixture.mkdir()
        (fixture / 'Folder').mkdir()
        (fixture / 'Notes.txt').write_text('Disposable sidebar regression fixture.\n')
        with (out / 'finder.log').open('w') as log:
            process = subprocess.Popen([str(args.binary.resolve()), '--test-mode', str(fixture)],
                                       stdout=log, stderr=subprocess.STDOUT)
            wait_for(lambda: bus.name_has_owner('org.local.FinderTest'), 'test service')
            api = dbus.Interface(bus.get_object('org.local.FinderTest', '/Test'),
                                 'org.local.FinderTest')
            wait_for(lambda: status()['count'] == 2, 'native directory listing')
            initial_width = status()['sidebarWidth']
            assert initial_width == 256, status()
            rows=json.loads(str(api.sidebarRows(timeout=3)))
            assert [row['text'] for row in rows[:3]]==['Recents','Shared','Favorites'], rows
            assert all(row['height']==(32 if row['selectable'] else 18) for row in rows), rows
            (out/'sidebar-rows.json').write_text(json.dumps(rows,indent=2)+'\n')
            for mode, name in enumerate(('icons', 'list', 'columns', 'gallery')):
                assert api.setMode(mode, timeout=3)
                capture(name)
            assert api.setMode(0, timeout=3)
            for group in range(6):
                assert api.trigger('group'+str(group),timeout=3)
                wait_for(lambda:status()['group']==group,'group mode')
                assert api.selectFiles([str(fixture/'Notes.txt')],timeout=3)
                wait_for(lambda:len(status()['selected'])==1,'group native selection')
                capture('group-'+str(group))
            assert api.trigger('group0',timeout=3)
            assert api.trigger('preview',timeout=3)
            wait_for(lambda:status()['previewShown'],'preview toggle')
            capture('preview-pane')
            assert api.trigger('preview',timeout=3)
            assert api.dragSidebar(80, timeout=3)
            wait_for(lambda: abs(status()['sidebarWidth'] - (initial_width + 80)) <= 2,
                     'native sidebar drag changes geometry')
            capture('sidebar-wider')
            assert api.dragSidebar(-160, timeout=3)
            wait_for(lambda: abs(status()['sidebarWidth'] - (initial_width - 80)) <= 4,
                     'native sidebar drag left')
            capture('sidebar-narrower')
            assert api.dragSidebar(initial_width - status()['sidebarWidth'] + 2, timeout=3)
            wait_for(lambda: abs(status()['sidebarWidth'] - initial_width) <= 2, 'sidebar restored')
            assert api.resizeWindow(700, 440, timeout=3)
            capture('minimum-window')
            assert api.resizeWindow(1100, 720, timeout=3)
            capture('window-restored')
            assert api.clickObject('finderSidebarToggle', timeout=3)
            wait_for(lambda: not status()['sidebarVisible'], 'hide sidebar')
            capture('sidebar-hidden')
            assert api.clickObject('finderSidebarToggle', timeout=3)
            wait_for(lambda: status()['sidebarVisible'], 'show sidebar')
            capture('sidebar-shown')
            assert api.trigger('newTab', timeout=3)
            wait_for(lambda: status()['tabs'] == 2, 'new tab')
            capture('tab-bar')
            assert api.trigger('search', timeout=3)
            assert api.setText('finderSearch', 'Notes', timeout=3)
            wait_for(lambda: status()['searchVisible'] and not status()['searchRunning']
                     and status()['searchResults'] == 1, 'search completion')
            capture('search-results')
            assert api.key(int(Qt.Key.Key_Escape), 0, '', timeout=3)
            wait_for(lambda: not status()['searchVisible'], 'search close')
            os.setxattr(fixture/'Notes.txt','user.xdg.tags',b'Blue,Project')
            assert api.activateSidebar('Blue',timeout=3)
            wait_for(lambda:status()['searchVisible'] and not status()['searchRunning'] and status()['searchResults']>=1,'tag sidebar results', timeout=25)
            assert api.selectFiles([str(fixture/'Notes.txt')],timeout=3)
            wait_for(lambda:status()['selected']==[(fixture/'Notes.txt').as_uri()],'actual fixture in tag results')
            capture('tag-results')
            assert api.key(int(Qt.Key.Key_Escape),0,'',timeout=3)
            wait_for(lambda:not status()['searchVisible'],'tag results close')
            assert api.clickObject('window-maximize', timeout=3)
            capture('maximized')
            assert api.clickObject('window-maximize', timeout=3)
            capture('restored-after-maximize')
            summary = {'passed': True, 'captures': records, 'checks': len(records),
                       'fixtureOnlyWrites': True, 'privateWayland': True,
                       'scale': os.environ.get('QT_SCALE_FACTOR', '1')}
            (out / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
finally:
    if process:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
