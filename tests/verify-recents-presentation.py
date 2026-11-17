#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real recentlyused worker, native launch and error checks on a private Wayland session."""
import argparse, json, os, sqlite3, subprocess, time
from pathlib import Path
from urllib.parse import unquote
import dbus
from PyQt6.QtWidgets import QApplication
from PyQt6.QtGui import QImage,QColor
from PyQt6.QtCore import Qt
from PyQt6.QtTest import QTest
assert os.environ.get('KUBUNTU_VIRTUAL_SCREEN')=='1' and os.environ.get('WAYLAND_DISPLAY')=='codex-test-screen'
assert not os.environ.get('DISPLAY')
assert os.environ['XDG_RUNTIME_DIR'] in os.environ['DBUS_SESSION_BUS_ADDRESS']
p=argparse.ArgumentParser();p.add_argument('--binary',type=Path,required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
app=QApplication([]);bus=dbus.SessionBus();checks={};api=None
fixture=Path(os.environ['XDG_RUNTIME_DIR'])/'recents-fixture';fixture.mkdir()
photo=fixture/'Photo # one.png';image=QImage(400,250,QImage.Format.Format_RGB32);image.fill(QColor('#246faa'));assert image.save(str(photo))
empty=fixture/'Empty';empty.mkdir()
record=fixture/'launch.json';helper=fixture/'open-image.py';helper.write_text('import json,sys\nfrom pathlib import Path\nPath('+repr(str(record))+').write_text(json.dumps(sys.argv[1:]))\n')
applications=Path(os.environ['XDG_DATA_HOME'])/'applications';applications.mkdir(parents=True,exist_ok=True)
(applications/'finder-private-image.desktop').write_text('[Desktop Entry]\nType=Application\nName=Private Image Handler\nExec=/usr/bin/python3 '+str(helper)+' %u\nMimeType=image/png;\nNoDisplay=true\n')
(Path(os.environ['XDG_CONFIG_HOME'])/'mimeapps.list').write_text('[Default Applications]\nimage/png=finder-private-image.desktop;\n')
# Isolated activity history, not a copy of or connection to the user's database.
dbpath=Path(os.environ['XDG_DATA_HOME'])/'kactivitymanagerd/resources/database';dbpath.parent.mkdir(parents=True)
db=sqlite3.connect(dbpath);db.execute('PRAGMA journal_mode=WAL')
db.executescript('''CREATE TABLE SchemaInfo(key text PRIMARY KEY,value text);
INSERT INTO SchemaInfo VALUES('version','2015.02.09');
CREATE TABLE ResourceEvent(usedActivity TEXT,initiatingAgent TEXT,targettedResource TEXT,start INTEGER,end INTEGER);
CREATE TABLE ResourceScoreCache(usedActivity TEXT,initiatingAgent TEXT,targettedResource TEXT,scoreType INTEGER,cachedScore FLOAT,firstUpdate INTEGER,lastUpdate INTEGER,PRIMARY KEY(usedActivity,initiatingAgent,targettedResource));
CREATE TABLE ResourceLink(usedActivity TEXT,initiatingAgent TEXT,targettedResource TEXT,PRIMARY KEY(usedActivity,initiatingAgent,targettedResource));
CREATE TABLE ResourceInfo(targettedResource TEXT,title TEXT,mimetype TEXT,autoTitle INTEGER,autoMimetype INTEGER,PRIMARY KEY(targettedResource));''')
now=int(time.time());activity='11111111-2222-3333-4444-555555555555'
db.execute('INSERT INTO ResourceScoreCache VALUES(?,?,?,?,?,?,?)',(activity,'FinderPrivate',str(photo),0,12.0,now-20,now))
db.execute('INSERT INTO ResourceInfo VALUES(?,?,?,?,?)',(str(photo),photo.name,'image/png',1,1));db.commit();db.close()
log=(out/'finder.log').open('w');process=subprocess.Popen([str(args.binary.resolve()),'--test-mode',str(empty)],stdout=log,stderr=log)
def until(pred,label,timeout=15):
 end=time.monotonic()+timeout
 while time.monotonic()<end:
  assert process.poll() is None,(out/'finder.log').read_text()
  if pred():return
  QTest.qWait(80)
 raise AssertionError((label,json.loads(str(api.diagnostics())) if api else None,json.loads(str(api.topLevels())) if api else None))
def passed(name):checks[name]=True;print('PASS',name,flush=True)
def top():return json.loads(str(api.topLevels()))
def trigger(name):assert api.trigger(name);QTest.qWait(160)
try:
 until(lambda:bus.name_has_owner('org.local.FinderTest'),'test API')
 api=dbus.Interface(bus.get_object('org.local.FinderTest','/Test'),'org.local.FinderTest')
 until(lambda:json.loads(str(api.directoryState())) and json.loads(str(api.directoryState()))[0]['state']=='folder','empty state')
 for mode in range(4):
  assert api.setMode(mode);until(lambda:json.loads(str(api.directoryState())) and json.loads(str(api.directoryState()))[0]['title']=='This folder is empty','empty mode')
  assert api.capture(str(out/f'empty-{mode}.png'))
 passed('all-four-empty-presentations')
 assert api.setMode(0);assert api.navigate('recentlyused:/files?activity=any')
 until(lambda:len(json.loads(str(api.presentedFiles())))==1,'real recentlyused fixture')
 rows=json.loads(str(api.presentedFiles()));assert rows[0]['url'].startswith('recentlyused:') and unquote(rows[0]['target'])==unquote(photo.as_uri()),rows
 passed('real-provider-virtual-identity-and-target')
 for mode in range(4):
  assert api.setMode(mode);assert api.selectFiles([str(photo)]);QTest.qWait(120)
  assert unquote(json.loads(str(api.diagnostics()))['url']).startswith('recentlyused:/files')
  trigger('quickLook');until(lambda:unquote(str(api.quickLookTarget()))==unquote(photo.as_uri()),'mode target preview')
  assert api.key(Qt.Key.Key_Escape.value,0,'');QTest.qWait(100)
  trigger('copy');assert json.loads(str(api.clipboardUrls()))==[rows[0]['target']]
 passed('all-four-recents-preview-clipboard-history-routing')
 assert api.setMode(0)
 assert api.selectFiles([str(photo)])
 until(lambda:json.loads(str(api.diagnostics()))['selected']==[str(rows[0]['target'])],'target selection')
 trigger('quickLook');until(lambda:unquote(str(api.quickLookTarget()))==unquote(photo.as_uri()),'Quick Look target')
 until(lambda:any(any(l.get('imageWidth',0)>100 for l in w['labels']) for w in top() if w['title']=='Quick Look'),'decoded original image')
 assert api.captureTopLevel('Quick Look',str(out/'recents-quicklook.png'));passed('recents-quicklook-decodes-real-image')
 assert api.key(Qt.Key.Key_Escape.value,0,'');QTest.qWait(100)
 trigger('info');until(lambda:json.loads(str(api.infoState(''))).get('url','')==rows[0]['target'],'native Get Info target')
 assert json.loads(str(api.infoState('')))['name']==photo.name
 assert api.dialogButton('Cancel');passed('recents-native-info-real-target')
 trigger('copy');until(lambda:json.loads(str(api.clipboardUrls()))==[rows[0]['target']],'native clipboard target');passed('recents-native-clipboard-target')
 trigger('open');until(record.exists,'native image launcher')
 launched=json.loads(record.read_text());assert len(launched)==1 and launched[0] in (str(photo),photo.as_uri()),launched
 passed('recents-native-open-real-file')
 assert api.capture(str(out/'recents.png'))
 photo.unlink();trigger('open')
 until(lambda:any('couldn’t be opened' in w['title'] for w in top()),'missing original native error')
 alert=next(w for w in top() if 'couldn’t be opened' in w['title']);assert photo.name in alert['title'] and 'recentlyused:' not in alert['title']
 assert any('moved or deleted' in l.get('text','') for l in alert['labels']),alert
 assert api.captureTopLevel(alert['title'],str(out/'missing-alert.png'));passed('missing-original-truthful-rounded-alert')
 assert api.dialogButton('OK');QTest.qWait(80)
 assert api.navigate(str(empty));until(lambda:json.loads(str(api.directoryState())),'empty recovery');passed('error-dismiss-and-navigation-recovery')
 (out/'result.json').write_text(json.dumps({'passed':True,'checks':checks,'privateWayland':True,'realRecentlyUsedWorker':True},indent=2)+'\n')
except Exception:
 (out/'failure.json').write_text(json.dumps({'checks':checks,'state':json.loads(str(api.diagnostics())) if api else None,'top':top() if api else None},indent=2)+'\n');raise
finally:
 process.terminate()
 try:process.wait(timeout=5)
 except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
 log.close()
