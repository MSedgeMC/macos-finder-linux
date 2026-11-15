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
from PyQt6.QtGui import QImage
im=QImage(1200,800,QImage.Format.Format_RGB32);im.fill(QColor('#28c6dd'));im.save(str(fixture/'Picture.png'))
im.save(str(fixture/'Second.png'))
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
    wait_for(lambda:json.loads(str(api.diagnostics()))['count']==3,'file listing')
    assert api.selectFiles([str(fixture/'Picture.png')]);assert api.trigger('quickLook')
    def state():return json.loads(str(api.topLevelDiagnostics('Quick Look')))
    def widgets():return {w['name']:w for w in state()['widgets']}
    wait_for(lambda:bool(str(api.topLevelDiagnostics('Quick Look'))),'Quick Look open')
    QTest.qWait(600)
    initial=state();w=widgets()
    assert initial['width']<=app.primaryScreen().availableGeometry().width()-60,initial
    assert initial['height']<=app.primaryScreen().availableGeometry().height()-60,initial
    assert w['finderQuickLookHeader']['height']==40
    for name in ('previewName','previewOpen','previewNativeActions'):
        item=w[name];assert item['visible'] and item['y']+item['height']<=40,(name,item)
    assert not w['previewViewControls']['visible']
    assert api.captureTopLevel('Quick Look',str(out/'quicklook-alpha.png'))
    alpha=Image.open(out/'quicklook-alpha.png').convert('RGBA')
    opaque=sum(alpha.getpixel((x,y))[3]==255 for y in range(26,alpha.height-26) for x in range(26,alpha.width-26))
    assert opaque==(alpha.width-52)*(alpha.height-52),'transparent preview content'
    first=capture('quicklook-composited')
    pattern.setColor(QColor('#2183dd'));second=capture('quicklook-backdrop-changed')
    def red_bounds(image):
        pts=[(x,y) for y in range(image.height) for x in range(image.width) if image.getpixel((x,y))[:3]==(255,95,87)]
        assert pts
        return min(x for x,y in pts),min(y for x,y in pts)
    cr=red_bounds(alpha);sr=red_bounds(first);origin=(sr[0]-cr[0],sr[1]-cr[1])
    crop=(origin[0]+26,origin[1]+26,origin[0]+alpha.width-26,origin[1]+alpha.height-26)
    assert ImageChops.difference(first.crop(crop),second.crop(crop)).getbbox() is None,'backdrop visible through preview'
    for width,height in ((480,320),(620,420)):
        assert api.resizeTopLevel('Quick Look',width,height);QTest.qWait(200)
        for name in ('previewOpen','previewNativeActions','previewName'):
            item=widgets()[name];assert item['visible'] and item['x']>=0 and item['x']+item['width']<=width,(name,item)
        assert api.captureTopLevel('Quick Look',str(out/f'quicklook-{width}.png'))
    assert api.key(0x01000000,0,'');QTest.qWait(100)
    assert api.selectFiles([str(fixture/'Picture.png'),str(fixture/'Second.png')]);assert api.trigger('quickLook');QTest.qWait(300)
    for name in ('previewPrevious','previewNext','previewCount'):assert widgets()[name]['visible']
    assert api.clickObject('previewNext');assert api.captureTopLevel('Quick Look',str(out/'quicklook-multi.png'))
    assert api.clickObject('window-maximize');wait_for(lambda:state()['fullscreen'],'Quick Look full screen')
    assert api.key(0x01000000,0,'');wait_for(lambda:not state()['fullscreen'],'Quick Look restore')
    result={'passed':True,'single40pxHeader':True,'opaquePreview':True,'backdropUnchanged':True,'smallSizesUnclipped':True,'multiFileNavigation':True,'fullScreenRestore':True,'privateWayland':True}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2),flush=True)

finally:
    process.terminate()
    try: process.wait(timeout=5)
    except subprocess.TimeoutExpired: process.kill();process.wait(timeout=5)
    log.close();pattern.close()
