#!/usr/bin/env python3
"""Check actual Finder scrolling and selection on an isolated Wayland screen."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from urllib.parse import unquote

import dbus
from PyQt6.QtTest import QTest
from PyQt6.QtWidgets import QApplication

assert os.environ.get("KUBUNTU_VIRTUAL_SCREEN") == "1"
assert os.environ.get("WAYLAND_DISPLAY") == "codex-test-screen"
assert not os.environ.get("DISPLAY")
assert os.environ["XDG_RUNTIME_DIR"] in os.environ["DBUS_SESSION_BUS_ADDRESS"]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--binary", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--measure-only", action="store_true")
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
app = QApplication([])
bus = dbus.SessionBus()
checks, measurements = {}, []
process = api = None

def state(): return json.loads(str(api.diagnostics()))
def scrolling(name): return json.loads(str(api.scrollState(name)))
def until(predicate, label):
    end = time.monotonic() + 15
    while time.monotonic() < end:
        assert process.poll() is None, (out / "finder.log").read_text()
        if predicate(): return
        QTest.qWait(40)
    raise AssertionError((label, state()))
def record(name):
    checks[name] = True
    print("PASS", name, flush=True)
def settled(name): until(lambda: not scrolling(name)["animating"], "wheel settled")
def step(name, angle=-120, pixel=0, phase=0):
    before = scrolling(name)
    assert api.wheel(name, 0, angle, 0, pixel, phase)
    settled(name)
    after = scrolling(name)
    return after["vertical"] - before["vertical"], after["horizontal"] - before["horizontal"]
def start(name):
    assert api.wheel(name, 0, 0, 0, -400, 2)
    QTest.qWait(40)

try:
    with tempfile.TemporaryDirectory(prefix="finder-scroll-", dir=Path(os.environ["XDG_RUNTIME_DIR"])) as fixture:
        root = Path(fixture)
        for i in range(350): (root / f"File {i:03}.txt").write_text(f"Selection fixture {i}\n")
        folder = Path(os.environ["XDG_CONFIG_HOME"]) / "LocalDesktop"
        folder.mkdir(exist_ok=True)
        (folder / "Finder.conf").write_text("[Search]\ndefaultScope=current\n")
        log = (out / "finder.log").open("w")
        process = subprocess.Popen([str(args.binary.resolve()), "--test-mode", str(root)], stdout=log, stderr=log)
        until(lambda: bus.name_has_owner("org.local.FinderTest"), "startup")
        api = dbus.Interface(bus.get_object("org.local.FinderTest", "/Test"), "org.local.FinderTest")
        until(lambda: state()["count"] == 350, "native listing")
        assert api.resizeWindow(950, 620)
        selected = root / "File 020.txt"
        assert api.selectFiles([str(selected)])
        for mode, name in ((0,"finderIconView"),(1,"finderListView"),(3,"finderGalleryStrip")):
            assert api.setMode(mode)
            QTest.qWait(150)
            start(name)
            travel = step(name)
            measurements.append({"mode":mode,"iconSize":state()["iconSize"],"oneNotch":travel,"scroll":scrolling(name)})
            if not args.measure_only:
                distance = travel[1] if mode == 3 else travel[0]
                assert 0 < distance <= 72, (mode, travel)
                assert not (travel[0] if mode == 3 else travel[1]), travel
                assert step(name, angle=0, pixel=-17, phase=2) == ((0,17) if mode==3 else (17,0))
                assert step(name, angle=0, pixel=-9, phase=3) == ((0,9) if mode==3 else (9,0))
                final = scrolling(name)
                QTest.qWait(250)
                assert scrolling(name)==final, "second momentum tail"
                assert unquote(state()["selected"][0])==unquote(selected.as_uri())
                record(f"mode-{mode}-bounded-wheel-direct-pixels-momentum-and-selection")
        assert api.setMode(0)
        assert api.trigger("viewOptions")
        QTest.qWait(180)
        assert api.setControl("viewOptionIconSize",256)
        assert api.dialogButton("Close")
        QTest.qWait(180)
        start("finderIconView")
        travel=step("finderIconView")
        measurements.append({"mode":0,"iconSize":256,"oneNotch":travel,"scroll":scrolling("finderIconView")})
        if not args.measure_only:
            assert 0 < travel[0] <= 72, travel
            record("256px-icons-do-not-multiply-wheel-distance")
            assert api.trigger("settings")
            QTest.qWait(150)
            assert api.setControl("finderSettingsTabs",3)
            assert api.setControl("finderSettingsWheelSpeed",25)
            assert api.dialogButton("Close")
            QTest.qWait(100)
            slow=step("finderIconView")[0]
            assert slow*2==travel[0], (slow,travel)
            assert api.trigger("settings")
            QTest.qWait(150)
            assert api.setControl("finderSettingsTabs",3)
            assert api.setControl("finderSettingsWheelSpeed",50)
            assert api.setControl("finderSettingsSmoothScroll",0)
            assert api.dialogButton("Close")
            QTest.qWait(100)
            direct=step("finderIconView")[0]
            assert direct==travel[0], (direct,travel)
            assert step("finderIconView",angle=-120,phase=2)[0]==direct
            assert step("finderIconView",angle=0,pixel=-17,phase=2)[0]==17
            record("live-speed-control-and-reduced-animation-preserve-distance")
            assert api.trigger("settings")
            QTest.qWait(150)
            assert api.setControl("finderSettingsTabs",3)
            assert api.setControl("finderSettingsSmoothScroll",1)
            assert api.dialogButton("Close")
            QTest.qWait(120)
            assert api.wheel("finderIconView",0,-120,0,0,0)
            assert api.trigger("newTab")
            QTest.qWait(230)
            assert not scrolling("finderIconView")["animating"]
            assert api.capture(str(out / "window.png"))
            record("tab-navigation-stops-pending-wheel-animation")
        result={"passed":True,"checks":checks,"measurements":measurements,"measureOnly":args.measure_only,"privateWayland":True}
except Exception as error:
    result={"passed":False,"checks":checks,"measurements":measurements,"error":repr(error)}
    raise
finally:
    if process:
        process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
    (out / "result.json").write_text(json.dumps(result,indent=2)+"\n")
