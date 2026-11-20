#!/usr/bin/env python3
"""Launch standalone Preview on a private native screen, capture without input."""
import argparse, json, os, subprocess, tempfile, time, shutil
from pathlib import Path
import dbus
from PyQt6.QtWidgets import QApplication
from PyQt6.QtGui import QImage, QColor, QPainter, QPdfWriter, QGuiApplication
from PyQt6.QtTest import QTest
assert os.environ.get('KUBUNTU_VIRTUAL_SCREEN')=='1' and not os.environ.get('DISPLAY')
parser=argparse.ArgumentParser();parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
for name in ('kdeglobals','fontconfig/fonts.conf'):
    src=Path(os.environ['KUBUNTU_WORKING_CONFIG'])/name;dst=Path(os.environ['XDG_CONFIG_HOME'])/name
    if src.is_file():dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)
app=QApplication([]);bus=dbus.SessionBus();checks={}
with tempfile.TemporaryDirectory(prefix='preview-native-',dir=Path.home()) as temporary:
    root=Path(temporary);image=root/'Mountains.png'
    canvas=QImage(1200,800,QImage.Format.Format_RGB32);canvas.fill(QColor('#cce0f0'));paint=QPainter(canvas)
    paint.fillRect(0,440,1200,360,QColor('#447c64'));paint.fillRect(170,150,260,260,QColor('#6e95ae'));paint.end();assert canvas.save(str(image))
    text=root/'Readme.txt';text.write_text('A readable document\n\n'+'This line follows the document width. '*90)
    pdf=root/'Document.pdf';writer=QPdfWriter(str(pdf));paint=QPainter(writer);paint.drawText(400,500,'Native Preview document');writer.newPage();paint.drawText(400,500,'Page two');paint.end();del writer
    for name,files in [('single-image',[image]),('image',[image,pdf]),('pdf',[pdf,image]),('text',[text,image])]:
        with (out/(name+'.log')).open('w') as log:
            process=subprocess.Popen([str(args.binary.resolve()),'--preview','--',*[str(path) for path in files]],stdout=log,stderr=log)
            try:
                QTest.qWait(2000); assert process.poll() is None
                screenshot=out/(name+'.png')
                capture=subprocess.Popen(['spectacle','-b','-n','-f','-o',str(screenshot)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
                deadline=time.monotonic()+12
                while capture.poll() is None and time.monotonic()<deadline: QTest.qWait(50)
                if capture.poll() is None: capture.kill();capture.wait();raise AssertionError('private compositor capture timed out')
                error=capture.stderr.read().decode(errors='replace');assert capture.returncode==0 and screenshot.is_file(),error
                assert not bus.name_has_owner('org.local.Finder') and not bus.name_has_owner('org.freedesktop.FileManager1')
                checks[name+'-standalone-native-no-filemanager-service']=True
            finally:
                process.terminate()
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
(out/'result.json').write_text(json.dumps({'passed':True,'checks':checks,'privateWayland':True},indent=2)+'\n')
print(json.dumps(checks))
