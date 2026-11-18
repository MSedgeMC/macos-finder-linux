#!/usr/bin/env python3
"""Finder presentation checks on a private native Wayland screen."""
import argparse, json, os, subprocess, tempfile, time, shutil
from pathlib import Path
import dbus
from PyQt6.QtWidgets import QApplication
from PyQt6.QtTest import QTest
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QImage, QColor
assert os.environ.get('KUBUNTU_VIRTUAL_SCREEN') == '1' and not os.environ.get('DISPLAY')
parser = argparse.ArgumentParser(); parser.add_argument('--binary', type=Path, required=True); parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args(); out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
for name in ('kdeglobals', 'fontconfig/fonts.conf'):
    src = Path(os.environ['KUBUNTU_WORKING_CONFIG'])/name; dst = Path(os.environ['XDG_CONFIG_HOME'])/name
    if src.is_file(): dst.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(src, dst)
app = QApplication([]); bus = dbus.SessionBus(); process = None; api = None; checks = {}
def state(): return json.loads(str(api.diagnostics()))
def tops(): return json.loads(str(api.topLevels()))
def until(condition, label, timeout=15):
    end = time.monotonic()+timeout
    while time.monotonic()<end:
        assert process.poll() is None
        if condition(): return
        QTest.qWait(75)
    raise AssertionError((label,state(),tops()))
def trigger(name): assert api.trigger(name); QTest.qWait(150)
def action(name): return json.loads(str(api.actionState(name)))
def panel(title): return json.loads(str(api.topLevelDiagnostics(title)))
def controls(title): return {item['name']:item for item in panel(title)['widgets']}
def record(name): checks[name] = True; print('PASS',name,flush=True)
try:
    with tempfile.TemporaryDirectory(prefix='finder-presentation-', dir=Path.home()) as scope:
        root = Path(scope)/'Documents'; root.mkdir(); other = Path(scope)/'Other'; other.mkdir()
        text = root/'Readme.txt'; text.write_text('Plain document\n'+'a long line '*400)
        image = root/'Image.png'; raster = QImage(1600,1000,QImage.Format.Format_RGB32); raster.fill(QColor('#6795c4')); assert raster.save(str(image))
        (root/'Subfolder').mkdir()
        with (out/'finder.log').open('w') as log:
            process = subprocess.Popen([str(args.binary.resolve()),'--test-mode',str(root)],stdout=log,stderr=log)
            until(lambda:bus.name_has_owner('org.local.FinderTest'),'startup'); api = dbus.Interface(bus.get_object('org.local.FinderTest','/Test'),'org.local.FinderTest')
            until(lambda:state()['count']==3,'native listing')
            for mode in range(4):
                assert api.setMode(mode); QTest.qWait(150)
                assert action('sortNone')['visible']==(mode==0)
                assert action('sortNone')['enabled']==(mode==0)
                for name in ('snapToGrid','cleanUp','cleanUpBy'): assert action(name)['visible']==(mode==0),(mode,name,action(name))
                for column in (8,9): assert not action('sort'+str(column))['enabled']
                trigger('viewOptions'); title = 'View Options — Documents'; until(lambda:any(w['title']==title for w in tops()),'view panel')
                c = controls(title); assert c['viewOptionGridSpacing']['visible']==(mode==0)
                assert c['viewOptionLabels']['visible']==(mode==0)
                assert c['viewOptionColumnsHeading']['visible']==(mode==1)
                assert c['viewOptionCalculateSizes']['visible']==(mode==1)
                assert c['viewOptionFilenames']['visible']==(mode==3)
                assert c['viewOptionUseDefaults']['visible']==(mode!=2)
                assert ('None' in c['viewOptionSort']['items'])==(mode==0)
                assert 'Date Added' not in c['viewOptionSort']['items'] and 'Last Opened' not in c['viewOptionSort']['items']
                if mode==2:
                    before=state()['iconSize']; assert api.setControl('viewOptionTextSize',14); assert state()['iconSize']==before
                if mode==3:
                    assert api.setControl('viewOptionFilenames',0)
                assert api.captureTopLevel(title,str(out/f'view-options-{mode}.png')); assert api.dialogButton('Close')
            record('four-mode-actions-and-live-view-options')
            assert api.setMode(0); trigger('sortNone'); trigger('viewOptions'); c=controls('View Options — Documents')
            assert not c['viewOptionReverse']['enabled']; assert api.setControl('viewOptionSort',0); assert controls('View Options — Documents')['viewOptionReverse']['enabled']
            assert api.setControl('viewOptionIconSize',112); assert api.clickObject('viewOptionUseDefaults'); assert any(w['title']=='View Options — Documents' for w in tops()); assert api.dialogButton('Close')
            assert api.navigate(str(other)); until(lambda:state()['url']==other.as_uri(),'defaults target'); assert state()['iconSize']==112
            assert api.navigate(str(root)); until(lambda:state()['url']==root.as_uri(),'original target'); record('direct-defaults-and-sort-none-availability')
            trigger('settings'); until(lambda:any(w['title']=='Finder Settings' for w in tops()),'Settings')
            for page in range(4):
                assert api.setControl('finderSettingsTabs',page); QTest.qWait(150); assert api.captureTopLevel('Finder Settings',str(out/f'settings-{page}.png'))
            assert api.setControl('finderSettingsTabs',2); c=controls('Finder Settings')
            assert c['finderSettingsFavoritesHeading']['visible'] and c['finderSettingsLocationsHeading']['visible']
            assert api.resizeTopLevel('Finder Settings',370,520); QTest.qWait(200); assert api.captureTopLevel('Finder Settings',str(out/'settings-narrow.png'))
            assert api.dialogButton('Close'); record('settings-four-pages-and-adaptive-layout')
            assert api.setMode(3); assert api.selectFiles([str(image)]); QTest.qWait(600)
            trigger('previewOptions'); until(lambda:any(w['title']=='Preview Options' for w in tops()),'Preview Options')
            assert api.setControl('previewOption_showInformation',0); assert api.setControl('previewOption_showQuickActions',0)
            assert api.capture(str(out/'gallery-details-hidden.png')); assert api.setControl('previewOption_showInformation',1); assert api.setControl('previewOption_showQuickActions',1)
            assert api.captureTopLevel('Preview Options',str(out/'preview-options.png')); assert api.dialogButton('Close'); record('preview-options-update-real-gallery')
            assert api.setMode(1); trigger('search'); assert api.setControl('finderSearchScope',1); assert api.setText('finderSearch','Readme')
            until(lambda:state()['searchVisible'] and not state()['searchRunning'] and state()['searchResults']==1,'search results')
            trigger('viewOptions'); until(lambda:any(w['title']=='View Options — Search' for w in tops()),'search presentation')
            assert not any(w['title']=='Search Criteria' for w in tops()); assert api.setControl('viewOptionMode',3)
            assert state()['searchResults']==1; assert api.captureTopLevel('View Options — Search',str(out/'search-view-options.png'))
            assert api.dialogButton('Close'); assert api.key(int(Qt.Key.Key_Escape),0,''); record('search-view-options-preserve-query-and-results')
            assert api.selectFiles([str(image)]); trigger('quickLook'); until(lambda:any(w['title']=='Quick Look' for w in tops()),'image Quick Look'); QTest.qWait(500)
            assert api.captureTopLevel('Quick Look',str(out/'quicklook-image.png')); trigger('quickLook')
            assert api.selectFiles([str(text)]); trigger('quickLook'); until(lambda:any(w['title']=='Quick Look' and any(label.get('name')=='previewText' for label in w.get('labels',[])) for w in tops()),'text Quick Look')
            assert api.captureTopLevel('Quick Look',str(out/'quicklook-document.png')); record('image-and-document-hosted-previews')
except Exception as error:
    (out/'failure.json').write_text(json.dumps({'error':str(error),'checks':checks},indent=2)+'\n'); raise
finally:
    if process:
        process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
    (out/'result.json').write_text(json.dumps({'passed':len(checks)==6,'checks':checks},indent=2)+'\n')
