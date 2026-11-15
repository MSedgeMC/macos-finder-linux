#!/usr/bin/env python3
"""Measure real KWin backdrop blur, never the working screen."""
import os
from pathlib import Path
import json
import subprocess
import time
import shutil
import argparse

assert os.environ.get('KUBUNTU_VIRTUAL_SCREEN') == '1'
assert os.environ.get('WAYLAND_DISPLAY') == 'codex-test-screen'
assert not os.environ.get('DISPLAY')
os.environ['QT_WAYLAND_SHELL_INTEGRATION'] = 'layer-shell'
import dbus
from PyQt6.QtCore import QUrl
from PyQt6.QtQml import QQmlApplicationEngine
from PyQt6.QtGui import QGuiApplication, QColor
from PyQt6.QtQuick import QQuickWindow
from PyQt6.QtTest import QTest
from PIL import Image, ImageChops, ImageStat

parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
app = QGuiApplication([])
assert all(screen.name().startswith('Virtual-') for screen in app.screens())
bus = dbus.SessionBus()
effects = bus.get_object('org.kde.KWin', '/Effects')
effects.loadEffect('blur', dbus_interface='org.kde.kwin.Effects')
assert 'blur' in effects.Get('org.kde.kwin.Effects', 'loadedEffects',
                              dbus_interface='org.freedesktop.DBus.Properties')
runtime = Path(os.environ['XDG_RUNTIME_DIR'])
(runtime / 'pattern.qml').write_text('''import QtQuick
import QtQuick.Window
import org.kde.layershell as LayerShell
Window {
    objectName: "pattern"; width: 1366; height: 768; visible: true
    color: "#b84180"
    LayerShell.Window.layer: LayerShell.Window.LayerBackground
    LayerShell.Window.anchors: LayerShell.Window.AnchorTop | LayerShell.Window.AnchorBottom | LayerShell.Window.AnchorLeft | LayerShell.Window.AnchorRight
    LayerShell.Window.exclusionZone: -1
    Repeater { model: 171
        Rectangle { x: index*8; width: 4; height: 768; color: "white" }
    }
}''')
engine = QQmlApplicationEngine()
engine.load(QUrl.fromLocalFile(str(runtime / 'pattern.qml')))
assert engine.rootObjects()
pattern = engine.rootObjects()[0]
fixture = runtime / 'Documents'; fixture.mkdir()
(fixture / 'Notes.txt').write_text('Private material fixture.\n')
for name in ('MacTahoe', 'MacTahoe-dark'):
    src = Path(os.environ['KUBUNTU_WORKING_DATA']) / 'icons' / name
    if src.is_dir(): shutil.copytree(src, Path(os.environ['XDG_DATA_HOME']) / 'icons' / name,
                                   dirs_exist_ok=True, symlinks=True)
env = os.environ.copy(); env.pop('QT_WAYLAND_SHELL_INTEGRATION', None)
log = (out / 'finder.log').open('w')
process = subprocess.Popen([str(args.binary.resolve()), '--test-mode', str(fixture)],
                           env=env, stdout=log, stderr=subprocess.STDOUT)

def wait_for(predicate, label):
    end=time.monotonic()+15
    while time.monotonic()<end:
        assert process.poll() is None, (out/'finder.log').read_text()
        if predicate(): return
        QTest.qWait(100)
    raise AssertionError(label)

def capture(name):
    QTest.qWait(750)
    path=out/(name+'.png')
    child=subprocess.Popen(['spectacle','-b','-n','-f','-o',str(path)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    deadline=time.monotonic()+12
    while child.poll() is None and time.monotonic()<deadline: QTest.qWait(50)
    if child.poll() is None: child.kill()
    _,err=child.communicate()
    assert child.returncode==0 and path.is_file(),err.decode()
    return Image.open(path).convert('RGB')

try:
    wait_for(lambda:bus.name_has_owner('org.local.FinderTest'),'test service')
    api=dbus.Interface(bus.get_object('org.local.FinderTest','/Test'),'org.local.FinderTest')
    assert api.moveWindow(100,20)
    QTest.qWait(300)
    assert api.setMaterialBlur(True)
    blurred=capture('blurred')
    assert api.setMaterialBlur(False)
    sharp=capture('unblurred')
    inside=(128,590,330,650)
    def detail(image):
        crop=image.crop(inside)
        return sum(ImageStat.Stat(ImageChops.difference(crop,ImageChops.offset(crop,4,0))).mean)/3
    blurred_detail,sharp_detail=detail(blurred),detail(sharp)
    assert sharp_detail>15,(blurred_detail,sharp_detail)
    assert blurred_detail<sharp_detail*.45,('backdrop not blurred',blurred_detail,sharp_detail)
    opaque=(390,580,640,650)
    assert ImageChops.difference(blurred.crop(opaque),sharp.crop(opaque)).getbbox() is None
    assert api.setMaterialBlur(True)
    pattern.setColor(QColor('#2183dd'))
    changed=capture('backdrop-changed')
    delta=sum(ImageStat.Stat(ImageChops.difference(blurred.crop(inside),changed.crop(inside))).mean)/3
    assert delta>10,delta
    result={'passed':True,'actualCompositorBackdrop':True,'blurredDetail':blurred_detail,
            'sharpDetail':sharp_detail,'detailRatio':blurred_detail/sharp_detail,
            'backdropColorDelta':delta,'opaqueFilePaneUnchanged':True,
            'privateWayland':True,'noWorkingDesktopInput':True}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2),flush=True)
finally:
    process.terminate()
    try: process.wait(timeout=5)
    except subprocess.TimeoutExpired: process.kill();process.wait(timeout=5)
    log.close();pattern.close()
