#!/usr/bin/env python3
import os,json,time,tempfile,subprocess,argparse,shutil
from pathlib import Path
from urllib.parse import unquote
import dbus
from PyQt6.QtWidgets import QApplication
from PyQt6.QtTest import QTest
from PyQt6.QtCore import Qt
assert os.environ.get('KUBUNTU_VIRTUAL_SCREEN')=='1' and not os.environ.get('DISPLAY')
parser=argparse.ArgumentParser();parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
for name in ('kdeglobals','fontconfig/fonts.conf'):
 src=Path(os.environ['KUBUNTU_WORKING_CONFIG'])/name;dst=Path(os.environ['XDG_CONFIG_HOME'])/name
 if src.is_file():dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)
app=QApplication([]);bus=dbus.SessionBus();api=None;p=None;checks={};result={}
def status():return json.loads(str(api.diagnostics()))
def top():return json.loads(str(api.topLevels()))
def windows():return json.loads(str(api.windows()))
def until(pred,label,timeout=15):
 end=time.monotonic()+timeout
 while time.monotonic()<end:
  assert p.poll() is None
  if pred():return
  QTest.qWait(75)
 state=status();state.pop('listed',None);raise AssertionError((label,state,top()))
def trigger(name):assert api.trigger(name);QTest.qWait(150)
def record(name):checks[name]=True;print('PASS',name,flush=True)
def select(*paths):
 until(lambda:status()['count']>0,'listing before selection');assert api.selectFiles([str(path) for path in paths]);until(lambda:all(unquote(path.as_uri()) in [unquote(url) for url in status()['selected']] for path in paths),'selection');QTest.qWait(120)
def action(name):return json.loads(str(api.actionState(name)))
def navigate(path):assert api.navigate(str(path));until(lambda:unquote(status()['url'])==unquote(path.as_uri()),'navigate')
try:
 with tempfile.TemporaryDirectory(prefix='finder-extra-',dir=Path.home()) as scope:
  root=Path(scope)/'Documents';root.mkdir();other=Path(scope)/'Other';other.mkdir()
  first=root/'First.txt';first.write_text('Native fixture: searchable galaxy phrase')
  second=root/'Second.txt';second.write_text('Keep this file unchanged')
  for i in range(350):(root/('Item '+str(i).zfill(3)+'.txt')).write_text('Text fixture')
  folder=root/'Folder';folder.mkdir();(folder/'Nested.txt').write_text('Nested')
  link=root/'First link';link.symlink_to(first)
  image=root/'Picture.png'
  from PyQt6.QtGui import QImage,QColor
  raster=QImage(900,700,QImage.Format.Format_RGB32);raster.fill(QColor('cyan'));assert raster.save(str(image))
  with (out/'finder.log').open('w') as log:
   p=subprocess.Popen([str(args.binary.resolve()),'--test-mode',str(root)],stdout=log,stderr=log)
   until(lambda:bus.name_has_owner('org.local.FinderTest'),'startup');api=dbus.Interface(bus.get_object('org.local.FinderTest','/Test'),'org.local.FinderTest')
   until(lambda:status()['count']==355,'listing')
   assert not action('rename')['enabled'];select(first,second);assert action('rename')['enabled'];record('multi-selection-rename-capability')
   select(first);trigger('makeAlias');alias=root/'First.txt alias';until(alias.is_symlink,'alias creation');assert alias.resolve()==first
   QTest.qWait(200);trigger('undo');until(lambda:not alias.exists(),'alias native Undo');record('alias-creation-and-native-undo')
   select(link);assert action('showOriginal')['enabled'];trigger('showOriginal');until(lambda:first.as_uri() in status()['selected'],'show original');record('symlink-show-original')
   select(second);trigger('trash');until(lambda:not second.exists(),'trash file');QTest.qWait(250)
   assert api.navigate('trash:/');until(lambda:status()['count']>=1,'trash listing')
   trash=Path(os.environ['XDG_DATA_HOME'])/'Trash/info';names=list(trash.glob('*.trashinfo'));assert names
   trash_url=next(url for url in json.loads(str(api.listedUrls())) if 'Second.txt' in url);assert api.selectFiles([trash_url]);QTest.qWait(120)
   assert action('putBack')['enabled'];assert not action('rename')['enabled'];trigger('putBack');until(second.exists,'put back original');assert second.read_text()=='Keep this file unchanged';record('trash-put-back-and-correct-capabilities')
   navigate(root);select(second);trigger('deleteImmediately');until(lambda:any(w['title']=='Delete Immediately' for w in top()),'permanent delete confirmation');assert api.dialogButton('Cancel');assert second.exists();record('permanent-delete-cancel-preserves-file')
   assert api.setMode(1);QTest.qWait(120);trigger('viewOptions');assert api.setControl('viewOptionIconSize',32);assert status()['iconSize']==32
   assert api.setControl('viewOptionGridSpacing',24);assert api.dialogButton('Save');navigate(other);navigate(root);until(lambda:status()['mode']==1 and status()['iconSize']==32,'per-folder view settings');record('live-view-options-and-folder-state')
   trigger('viewOptions');trigger('viewOptions');until(lambda:not any(w['title'].startswith('View Options') for w in top()),'view options toggle');record('view-options-toggle')
   trigger('allTabs');until(lambda:any(w['title']=='All Tabs' for w in top()),'tab overview');assert api.dialogButton('Close');record('tab-overview-window')
   trigger('connectServer');until(lambda:any(w['title']=='Connect to Server' for w in top()),'connect dialog');assert api.dialogButton('Cancel');record('connect-server-dialog-cancel')
   trigger('search');assert api.setControl('finderSearchScope',1);assert api.setText('finderSearch','First');until(lambda:status()['searchVisible'] and not status()['searchRunning'] and status()['searchResults']>=1,'name search')
   assert api.setControl('finderSearchKind',1);assert api.setText('finderSearchContent','searchable galaxy');until(lambda:not status()['searchRunning'] and status()['searchResults']==1,'kind and content search');select(first);record('search-real-kind-and-text-content')
   assert api.key(int(Qt.Key.Key_Return),0,'');until(lambda:any(w['class']=='QInputDialog' for w in top()),'Return rename search');assert api.dialogButton('Cancel');record('search-return-renames')
   assert api.key(int(Qt.Key.Key_Escape),0,'');until(lambda:not status()['searchVisible'],'search escape')
   select(image);trigger('quickLook');until(lambda:any(w['title']=='Quick Look' and w.get('labels') for w in top()),'quick look image')
   trigger('quickLook');until(lambda:not any(w['title']=='Quick Look' for w in top()),'quick look toggle')
   select(first);trigger('quickLook');until(lambda:any(w['title']=='Quick Look' and w.get('labels') for w in top()),'quick look reuse');assert api.captureTopLevel('Quick Look',str(out/'quicklook-reopened.png'));record('quick-look-reopen-after-toggle')
   assert api.key(int(Qt.Key.Key_Space),0,'');QTest.qWait(150)
   assert api.setMode(1);QTest.qWait(150);assert api.wheel('finderListView',0,0,0,-480,2);before=json.loads(str(api.scrollState('finderListView')))['vertical'];assert before>0
   trigger('newTab');navigate(other);trigger('previousTab');trigger('detachTab');until(lambda:len(windows())==2,'detach');QTest.qWait(200)
   target=next(i for i,w in enumerate(windows()) if unquote(w['url'])==unquote(root.as_uri()));assert api.activateWindow(target);until(lambda:unquote(status()['url'])==unquote(root.as_uri()),'detached window active');QTest.qWait(100)
   after=json.loads(str(api.scrollState('finderListView')))['vertical'];assert before==after,(before,after);record('detach-preserves-native-view-scroll')
   assert api.activateWindow(1);QTest.qWait(300);navigate(root)
   # Check mode-specific options, exact tab ownership and ancestor inheritance.
   assert api.setMode(0);QTest.qWait(120);trigger('viewOptions');assert api.setControl('viewOptionIconSize',128)
   assert api.setControl('viewOptionMode',1);QTest.qWait(120);assert status()['iconSize']==32,status()
   assert api.setControl('viewOptionMode',0);QTest.qWait(120);assert status()['iconSize']==128,status()
   assert api.setControl('viewOptionAlwaysOpen',1);assert api.setControl('viewOptionBrowseIn',1);assert api.dialogButton('Save')
   navigate(folder);until(lambda:status()['mode']==0,'ancestor view inheritance');navigate(root);record('independent-mode-options-and-folder-inheritance')
   trigger('viewOptions');trigger('newTab');until(lambda:not any(w['title'].startswith('View Options') for w in top()),'view panel closes across tabs');trigger('close');record('view-panel-target-tab-ownership')
   trigger('customizeToolbar');until(lambda:any(w['title']=='Customize Toolbar' for w in top()),'toolbar customization');assert api.captureTopLevel('Customize Toolbar',str(out/'toolbar-customization.png'));assert api.dialogButton('Close');record('toolbar-customization-dialog')
   select(first);trigger('inspector');until(lambda:any(w['title']=='Inspector' for w in top()),'selection inspector');assert api.captureTopLevel('Inspector',str(out/'inspector.png'))
   select(second);QTest.qWait(400);assert api.captureTopLevel('Inspector',str(out/'inspector-second.png'));trigger('inspector');until(lambda:not any(w['title']=='Inspector' for w in top()),'inspector toggle');record('selection-following-inspector')
   trigger('tile0');QTest.qWait(500);assert abs(status()['width']-683)<8,status()
   trigger('tile1');QTest.qWait(500);assert abs(status()['width']-683)<8,status()
   assert api.resizeWindow(1100,720);QTest.qWait(200);record('compositor-left-and-right-window-layouts')
   select(first);trigger('compress');archive=root/'First.txt.zip';until(archive.exists,'native ZIP creation');until(lambda:not status()['archiveBusy'],'archive settled')
   import zipfile
   with zipfile.ZipFile(archive) as zipped:assert zipped.read('First.txt')==first.read_bytes()
   QTest.qWait(200);trigger('undo');until(lambda:not archive.exists(),'native ZIP Undo');record('native-compress-integrity-and-undo')
   zipped=other/'Bundle.zip'
   with zipfile.ZipFile(zipped,'w') as container:container.writestr('From archive.txt','Extracted original contents')
   navigate(other);until(lambda:status()['count']>=1,'ZIP listing');select(zipped);trigger('open');expanded=other/'From archive.txt';until(expanded.exists,'Open expands ZIP');until(lambda:not status()['archiveBusy'],'extract settled');assert expanded.read_text()=='Extracted original contents'
   QTest.qWait(200);trigger('undo');until(lambda:not expanded.exists(),'native extraction Undo');record('zip-open-extracts-and-native-undo')
   navigate(root);trigger('search');assert api.setControl('finderSearchScope',1);assert api.setControl('finderSearchKind',0);assert api.setText('finderSearchContent','');assert api.setText('finderSearch','name:First OR name:Second')
   until(lambda:not status()['searchRunning'] and status()['searchResults']==3,'Boolean keyword search');record('boolean-keyword-search')
   select(first)
   for search_mode in range(4):
    assert api.setMode(search_mode);QTest.qWait(150);assert status()['searchVisible'] and status()['searchResults']==3 and first.as_uri() in status()['selected'];assert api.capture(str(out/('search-mode-'+str(search_mode)+'.png')))
   record('all-four-search-result-views-preserve-hits-and-selection')
   until(lambda:any(label.get('name')=='previewText' and 'searchable galaxy phrase' in label.get('text','') for window in top() for label in window.get('labels',[])),'search Gallery actual text content');record('search-gallery-decodes-selected-text')
   select(first,second);until(lambda:any(label.get('name')=='previewText' and 'Keep this file unchanged' in label.get('text','') for window in top() for label in window.get('labels',[])),'Gallery primary selection');assert api.selectFiles([]);QTest.qWait(150);select(first);until(lambda:any(label.get('name')=='previewText' and 'searchable galaxy phrase' in label.get('text','') for window in top() for label in window.get('labels',[])),'Gallery after cleared selection');record('search-gallery-primary-and-clear-reselection')
   search_window=next(i for i,w in enumerate(windows()) if w.get('searchVisible'));assert api.dialogButton('finderAddSearchCriteria');until(lambda:any(w['title']=='Search Criteria' for w in top()),'visual criteria editor');assert api.captureTopLevel('Search Criteria',str(out/'search-criteria-editor.png'));assert api.dialogButton('Cancel');record('visual-nested-search-criteria-editor')
   assert api.activateWindow(search_window);QTest.qWait(250);before_tabs=status()['tabs'];trigger('newTab');until(lambda:status()['tabs']==before_tabs+1,'new tab ownership');navigate(other);trigger('previousTab');until(lambda:status()['search']=='name:First OR name:Second' and not status()['searchRunning'] and status()['searchResults']==3,'independent search tab restoration');trigger('nextTab');trigger('close');record('independent-tab-search-state')
   trigger('newTab');navigate(other);trigger('previousTab');source_window=next(i for i,w in enumerate(windows()) if w.get('searchVisible'));before_windows=len(windows());trigger('detachTab');until(lambda:len(windows())==before_windows+1,'detach search tab');until(lambda:status()['searchVisible'] and status()['searchResults']==3,'detached search restored');assert api.activateWindow(source_window);QTest.qWait(250);assert not status()['searchVisible'] and unquote(status()['url'])==unquote(other.as_uri());target_window=next(i for i,w in enumerate(windows()) if w.get('searchVisible'));assert api.activateWindow(target_window);QTest.qWait(250);record('detached-search-leaves-source-on-own-folder')
   assert api.key(int(Qt.Key.Key_Escape),0,'');QTest.qWait(150)
   # Criteria-only descriptor opens asynchronously and FIFO is rejected.
   smart=other/'Images.finder-search';smart.write_text(json.dumps({'format':'macos-finder-linux-smart-folder','version':1,'root':root.as_uri(),'scope':'current','kind':'image','name':''}))
   assert api.navigate(str(smart));until(lambda:status()['searchVisible'] and not status()['searchRunning'] and status()['searchResults']==1,'criteria-only Smart Folder');record('criteria-only-smart-folder')
   select(image);assert api.setMode(3);until(lambda:any(label.get('name')=='previewImage' and label.get('imageWidth',0)>200 and label.get('imageHeight',0)>100 for window in top() for label in window.get('labels',[])),'search Gallery actual decoded image');assert api.capture(str(out/'search-gallery-image.png'));record('search-gallery-decodes-selected-image')
   assert api.key(int(Qt.Key.Key_Escape),0,'');QTest.qWait(150)
   fifo=other/'Blocked.finder-search';os.mkfifo(fifo);before_url=status()['url'];assert api.navigate(str(fifo));QTest.qWait(400);assert status()['url']==before_url;assert api.diagnostics(timeout=2);record('fifo-smart-folder-rejected-without-gui-block')
   nested=other/'Nested.finder-search';nested.write_text(json.dumps({'format':'macos-finder-linux-smart-folder','version':1,'root':root.as_uri(),'scope':'current','criteria':{'match':'all','criteria':[{'field':'kind','op':'equals','value':'text'},{'match':'none','criteria':[{'field':'name','op':'beginsWith','value':'Item'}]}]}}))
   assert api.navigate(str(nested));until(lambda:status()['searchVisible'] and not status()['searchRunning'] and status()['searchResults']==3,'nested saved criteria');record('nested-smart-folder-criteria-roundtrip')
   assert api.key(int(Qt.Key.Key_Escape),0,'');QTest.qWait(150)
   before=status();trigger('newTab');navigate(other);assert api.setMode(1);QTest.qWait(150);before_close=status();window_count=len(windows());assert api.closeWindow();until(lambda:len(windows())==window_count-1,'close snapshot');trigger('reopenWindow');until(lambda:len(windows())==window_count,'reopen window');until(lambda:status()['tabs']==2 and status()['mode']==1 and unquote(status()['url'])==unquote(other.as_uri()),'restored tabs and selected view');record('reopen-closed-window-tabs-and-view-state')
   trigger('settings');until(lambda:any(w['title']=='Finder Settings' for w in top()),'settings appearance')
   assert api.setControl('finderSettingsTabs',0);assert api.setControl('finderSettingsAppearanceMode',1);QTest.qWait(250);assert api.capture(str(out/'light-window.png'))
   assert api.setControl('finderSettingsReduceTransparency',1);until(lambda:not status()['blur'],'reduced transparency disables blur');assert api.capture(str(out/'reduced-transparency.png'))
   assert api.setControl('finderSettingsReduceTransparency',0);assert api.setControl('finderSettingsIncreaseContrast',1);until(lambda:not status()['blur'],'increased contrast opaque');assert api.capture(str(out/'increased-contrast.png'))
   assert api.setControl('finderSettingsIncreaseContrast',0);assert api.setControl('finderSettingsAppearanceMode',0);until(lambda:status()['blur'],'dark blur restored');assert api.dialogButton('Close');record('appearance-light-contrast-and-transparency-controls')
   trigger('search');assert api.setControl('finderSearchScope',1);assert api.setText('finderSearch','Bundle');until(lambda:status()['searchVisible'] and not status()['searchRunning'] and status()['searchResults']>=1,'session hidden search');hidden_index=next(i for i,w in enumerate(windows()) if w.get('searchVisible'));other_index=next(i for i in range(len(windows())) if i!=hidden_index);assert api.activateWindow(other_index);QTest.qWait(250);trigger('hideOthers');trigger('quit');p.wait(timeout=10);p=subprocess.Popen([str(args.binary.resolve()),'--test-mode','--restore-session'],stdout=log,stderr=log);until(lambda:bus.name_has_owner('org.local.FinderTest'),'restart session');api=dbus.Interface(bus.get_object('org.local.FinderTest','/Test'),'org.local.FinderTest');until(lambda:any(w.get('search')=='Bundle' and w.get('searchVisible') and w.get('sidebarVisible') and w.get('headerHeight')==52 for w in windows()),'hidden window chrome and search restored');record('hidden-window-session-preserves-chrome-and-search')
   assert api.capture(str(out/'completed-window.png'))
   result={'passed':True,'checks':checks,'privateWayland':True,'fixtureOnlyWrites':True}
except Exception as e:
 result={'passed':False,'checks':checks,'error':str(e)};raise
finally:
 if p:
  p.terminate()
  try:p.wait(timeout=5)
  except subprocess.TimeoutExpired:p.kill();p.wait(timeout=5)
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
