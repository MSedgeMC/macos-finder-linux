#!/usr/bin/env python3
"""Real private Wayland move/resize, window state and ordinary UX checks.
Run with KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 tools/virtual-screen.py -- ...
Never connect to a working display or session bus.
"""
import argparse, importlib.util, json, os, subprocess, time
from pathlib import Path
import runpy
_control_tools=runpy.run_path(str(Path(__file__).with_name("control-location.py")))
control_origin=_control_tools["control_origin"]
red_anchor=_control_tools["red_anchor"]
import dbus
from PIL import Image
from PyQt6.QtCore import Qt
from PyQt6.QtTest import QTest
from PyQt6.QtWidgets import QApplication
assert os.environ.get('KUBUNTU_VIRTUAL_SCREEN')=='1'
assert os.environ.get('WAYLAND_DISPLAY')=='codex-test-screen'
assert not os.environ.get('DISPLAY')
assert os.environ.get('KWIN_WAYLAND_NO_PERMISSION_CHECKS')=='1'
runtime=Path(os.environ['XDG_RUNTIME_DIR'])
assert str(runtime) in os.environ['DBUS_SESSION_BUS_ADDRESS']
parser=argparse.ArgumentParser();parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
app=QApplication([]);bus=dbus.SessionBus();checks={}
spec=importlib.util.spec_from_file_location('private_input',Path(__file__).with_name('private-wayland-input.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
fixture=runtime/'window-fixture';fixture.mkdir();(fixture/'Notes.txt').write_text('Private file fixture')
from PyQt6.QtGui import QImage,QColor
image=QImage(900,700,QImage.Format.Format_RGB32);image.fill(QColor('cyan'));image.save(str(fixture/'Picture.png'))
log=(out/'finder.log').open('w');process=subprocess.Popen([str(args.binary.resolve()),'--test-mode',str(fixture)],stdout=log,stderr=log)
api=None;native=None

def wait(pred,label,timeout=15):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        assert process.poll() is None,(out/'finder.log').read_text()
        if pred():return
        QTest.qWait(75)
    raise AssertionError((label,state() if api else None))
def state():return json.loads(str(api.diagnostics()))
def action(name):return json.loads(str(api.actionState(name)))
def trigger(name):assert api.trigger(name);QTest.qWait(160)
def passed(name):checks[name]=True;print('PASS',name,flush=True)
def capture():
    QTest.qWait(250);path=out/'screen.png'
    p=subprocess.Popen(['spectacle','-b','-n','-f','-o',str(path)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    wait(lambda:p.poll() is not None,'screenshot');assert p.returncode==0
    return Image.open(path).convert('RGB')
def origin():
    image=capture();client=out/'control-client.png';assert api.capture(str(client))
    return control_origin(Image.open(client),image)

def drag(x,y,dx,dy):
    native.move(x,y);QTest.qWait(100);native.button(1);QTest.qWait(100)
    for i in range(1,9):native.move(x+dx*i/8,y+dy*i/8);QTest.qWait(35)
    native.button(0);QTest.qWait(200)
try:
    wait(lambda:bus.name_has_owner('org.local.FinderTest'),'startup')
    api=dbus.Interface(bus.get_object('org.local.FinderTest','/Test'),'org.local.FinderTest')
    wait(lambda:state()['count']==2,'listing');native=module.NativeInput()
    assert api.resizeWindow(900,620);QTest.qWait(200)
    ox,oy=origin();native.move(ox+1,oy+100);QTest.qWait(120)
    assert api.pointerCursor()==Qt.CursorShape.SizeHorCursor.value,api.pointerCursor();passed('native-resize-cursor')
    native.move(ox+400,oy+150);QTest.qWait(100)
    assert api.pointerCursor()!=Qt.CursorShape.SizeHorCursor.value;passed('resize-cursor-restored')
    before=state();drag(ox+before['width']-2,oy+150,60,0)
    wait(lambda:state()['width']>=before['width']+45,'native edge resize');passed('native-edge-resize')
    ox,oy=origin();drag(ox+360,oy+20,45,35)
    nx,ny=origin();assert nx-ox>=30 and ny-oy>=20,(ox,oy,nx,ny);passed('native-header-move')
    trigger('zoom');wait(lambda:state()['maximized'],'maximize');assert not state()['fullscreen']
    trigger('fullscreen');wait(lambda:state()['fullscreen'],'full screen');assert action('fullscreen')['checked'];passed('maximize-to-fullscreen-state')
    trigger('fullscreen');wait(lambda:state()['maximized'] and not state()['fullscreen'],'restore maximized');passed('fullscreen-restores-maximize')
    trigger('zoom');wait(lambda:not state()['maximized'],'restore normal')
    # Green primary click enters fullscreen without first opening a menu.
    ox,oy=origin();native.move(ox+70,oy+26);QTest.qWait(70);native.button(1);native.button(0)
    wait(lambda:state()['fullscreen'],'green primary full screen');passed('green-primary-fullscreen')
    trigger('fullscreen');wait(lambda:not state()['fullscreen'],'green restore')
    assert not state()['maximized'];passed('fullscreen-restores-normal')
    assert state()['sidebarVisible'];trigger('toolbar')
    assert not state()['toolbarVisible'] and state()['sidebarVisible'];assert state()['headerHeight']==40
    assert api.capture(str(out/'toolbar-hidden.png'))
    assert api.clickObject('window-minimize');QTest.qWait(200);assert api.activateWindow(0);QTest.qWait(200)
    assert not state()['toolbarVisible'] and state()['sidebarVisible'];passed('toolbar-hidden-preserves-controls-and-sidebar')
    trigger('sidebar');assert not state()['sidebarVisible'];trigger('toolbar');assert not state()['sidebarVisible']
    assert api.clickObject('finderSidebarToggle');QTest.qWait(100);assert state()['sidebarVisible'] and action('sidebar')['checked'];passed('sidebar-button-and-menu-synchronized')
    trigger('settings');QTest.qWait(100);assert api.dialogButton('Close');passed('settings-dialog-close')
    # Plain-text clipboard/menu editing must not use filesystem capabilities.
    QApplication.clipboard().setText('temporary search phrase')
    trigger('search');assert action('paste')['enabled'];trigger('paste')
    assert api.focusedText()=='temporary search phrase';passed('text-paste-menu-routing')
    trigger('selectAll');assert action('cut')['enabled'];trigger('cut');assert api.focusedText()==''
    trigger('undo');assert api.focusedText()=='temporary search phrase';passed('text-cut-and-undo')
    assert api.key(Qt.Key.Key_Escape.value,0,'');QTest.qWait(150)
    assert api.pinch('finderIconView',1.5);assert state()['iconSize']==96;passed('host-native-pinch-icons')
    assert api.selectFiles([str(fixture/'Picture.png')]);trigger('quickLook');QTest.qWait(300)
    assert api.captureTopLevel('Quick Look',str(out/'quicklook.png'));passed('quicklook-native-host')
    assert api.key(Qt.Key.Key_Escape.value,0,'');QTest.qWait(150)
    trigger('zoom');wait(lambda:state()['maximized'],'maximize before activation')
    trigger('bringAll');assert state()['maximized'];assert api.activateWindow(0);assert state()['maximized'];passed('activation-preserves-maximize')
    trigger('zoom');wait(lambda:not state()['maximized'],'normal after activation')
    trigger('newTab');assert state()['tabs']==2;trigger('close');assert state()['tabs']==1;passed('ordinary-tab-open-close')
    (out/'result.json').write_text(json.dumps({'passed':True,'checks':checks,'privateWayland':True},indent=2)+'\n')
finally:
    if native:native.close()
    process.terminate()
    try:process.wait(timeout=5)
    except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
    log.close()
