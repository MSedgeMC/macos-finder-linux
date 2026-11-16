#!/usr/bin/env python3
"""Exercise portable Finder v7 interactions on the private Wayland screen."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import traceback
from urllib.parse import unquote

import dbus
from PIL import Image
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor, QImage
from PyQt6.QtTest import QTest
from PyQt6.QtWidgets import QApplication

assert os.environ.get("KUBUNTU_VIRTUAL_SCREEN") == "1"
assert os.environ.get("WAYLAND_DISPLAY") == "codex-test-screen"
assert not os.environ.get("DISPLAY")
assert os.environ.get("KWIN_WAYLAND_NO_PERMISSION_CHECKS") == "1"
assert os.environ["XDG_RUNTIME_DIR"] in os.environ["DBUS_SESSION_BUS_ADDRESS"]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--binary", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
for name in ("failure.json", "result.json"):
    (out / name).unlink(missing_ok=True)
app = QApplication([])
bus = dbus.SessionBus()
checks = {}
api = process = native = None
spec = importlib.util.spec_from_file_location("native_input", Path(__file__).with_name("private-wayland-input.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def state(): return json.loads(str(api.diagnostics()))
def top(): return json.loads(str(api.topLevels()))
def windows(): return json.loads(str(api.windows()))
def jobs(): return json.loads(str(api.nativeJobState()))
def action(name): return json.loads(str(api.actionState(name)))
def trigger(name):
    assert api.trigger(name), name
    QTest.qWait(120)
def until(predicate, label, timeout=15):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        assert process.poll() is None, (out / "finder.log").read_text()
        if predicate(): return
        QTest.qWait(60)
    raise AssertionError((label, state(), top()))
def record(name):
    checks[name] = True
    print("PASS", name, flush=True)
def navigate(path):
    assert api.navigate(str(path))
    until(lambda: unquote(state()["url"]).rstrip("/") == unquote(path.as_uri()).rstrip("/"), "navigate")
    QTest.qWait(150)
def select(*paths):
    assert api.selectFiles([str(path) for path in paths])
    until(lambda: {unquote(url) for url in state()["selected"]} == {unquote(path.as_uri()) for path in paths}, "select")
def native_key(key, modifiers=()):
    for code in modifiers: native.send(10, native.C.c_uint(code), native.C.c_uint(1))
    native.send(10, native.C.c_uint(key), native.C.c_uint(1))
    native.send(10, native.C.c_uint(key), native.C.c_uint(0))
    for code in reversed(modifiers): native.send(10, native.C.c_uint(code), native.C.c_uint(0))
    QTest.qWait(120)
def undo():
    before = jobs()["undoCompletions"]
    assert jobs()["undoAvailable"]
    trigger("undo")
    until(lambda: jobs()["undoCompletions"] == before + 1, "native Undo finishes")
def display(path):
    return next((item["name"] for item in json.loads(str(api.presentedFiles())) if unquote(item["url"]) == unquote(path.as_uri())), None)
def xattr(path, name):
    try: return os.getxattr(path,name)
    except OSError: return b""
def normalize_shortcut(value): return value.replace("Control+", "Ctrl+")
def close_dialog():
    assert api.dialogButton("Cancel")
    QTest.qWait(150)
def screenshot(name, title=None):
    target = str(out / (name + ".png"))
    assert api.captureTopLevel(title, target) if title else api.capture(target)
def origin():
    target = out / "native-screen.png"
    result = subprocess.run(["spectacle", "-b", "-n", "-f", "-o", str(target)], capture_output=True, timeout=10)
    assert result.returncode == 0, result.stderr
    image = Image.open(target).convert("RGB")
    points = [(x,y) for y in range(image.height) for x in range(image.width) if image.getpixel((x,y)) == (255,95,87)]
    assert points
    return min(x for x,y in points) - 20, min(y for x,y in points) - 21
def drag(x, y, dx, dy):
    native.move(x,y); QTest.qWait(80); native.button(1); QTest.qWait(80)
    for step in range(1,9): native.move(x+dx*step/8,y+dy*step/8); QTest.qWait(35)
    native.button(0); QTest.qWait(250)

try:
    with tempfile.TemporaryDirectory(prefix="finder-ux-", dir=Path.home()) as fixture:
        root = Path(fixture) / "Documents"
        root.mkdir()
        other = Path(fixture) / "Other"
        other.mkdir()
        nested = root / "Folder with spaces"
        nested.mkdir()
        alpha = root / "Alpha.txt"; alpha.write_text("Alpha original")
        beta = root / "Beta.txt"; beta.write_text("Beta original")
        image_a = root / "Image A.png"
        image_b = root / "Image B.png"
        raster = QImage(320,220,QImage.Format.Format_RGB32)
        raster.fill(QColor("cyan")); assert raster.save(str(image_a))
        raster.fill(QColor("orange")); assert raster.save(str(image_b))
        cfg = Path(os.environ["XDG_CONFIG_HOME"]) / "LocalDesktop"
        cfg.mkdir(exist_ok=True)
        (cfg / "Finder.conf").write_text("[General]\nnewWindowFolder="+root.as_uri()+"\n[Search]\ndefaultScope=current\n[Tags]\nfavoriteSlots=standard:red, standard:orange, standard:yellow, standard:green, standard:blue, standard:purple, standard:gray\n")
        log = (out / "finder.log").open("w")
        process = subprocess.Popen([str(args.binary.resolve()), "--test-mode", str(root)], stdout=log, stderr=log)
        until(lambda: bus.name_has_owner("org.local.FinderTest"), "startup")
        api = dbus.Interface(bus.get_object("org.local.FinderTest", "/Test"), "org.local.FinderTest")
        until(lambda: state()["count"] == 5, "initial listing")
        native = module.NativeInput()
        assert api.resizeWindow(1000,650)
        # Native keyboard events run through Qt's shortcut map, not QAction::trigger.
        select(alpha)
        native_key(3, (125,)); until(lambda: state()["mode"] == 1, "Command-2 list")
        native_key(2, (125,)); until(lambda: state()["mode"] == 0, "Command-1 icons")
        until(lambda: "Ctrl+1" in [normalize_shortcut(s) for s in action("tagRed").get("shortcuts",[])], "favorite tag slot shortcut")
        until(lambda: state()["tagsReady"], "selection tag metadata ready")
        native_key(2, (29,)); until(lambda: "Red" in xattr(alpha, "user.xdg.tags").decode(), "Control-1 tag")
        assert state()["mode"] == 0
        native_key(11, (29,)); until(lambda: xattr(alpha,"user.xdg.tags").decode() == "", "Control-0 clears tags")
        record("native-view-and-tag-shortcuts-do-not-conflict")
        width = state()["sidebarWidth"]
        native_key(31,(125,56)); until(lambda: not state()["sidebarVisible"], "Option-Command-S hide")
        native_key(31,(125,56)); until(lambda: state()["sidebarVisible"], "Option-Command-S show")
        assert state()["sidebarWidth"] == width
        record("native-finder-sidebar-shortcut")
        before = jobs()
        native_key(49,(125,42))
        created = root / "Untitled Folder"
        until(created.is_dir, "Command-Shift-N creates folder")
        assert api.dialogText("Keyboard Folder")
        renamed = root / "Keyboard Folder"
        until(renamed.is_dir, "inline folder rename")
        until(lambda: jobs()["recordings"] >= before["recordings"]+2, "both folder recordings")
        undo(); assert created.is_dir() and not renamed.exists()
        undo(); assert not created.exists()
        record("native-new-folder-shortcut-and-two-undo-steps")
        # Batch preview, native publication and Undo through the merged host.
        select(alpha,beta); trigger("rename")
        until(lambda: any(w["class"] == "BatchRenameDialog" for w in top()), "batch dialog")
        assert api.setControl("batchRenameMode",2)
        assert api.setText("batchCustomFormat","Combined")
        screenshot("batch-rename", "Rename Finder Items")
        before = jobs()["recordings"]
        assert api.dialogButton("Rename")
        outputs = [root / "Combined 1.txt", root / "Combined 2.txt"]
        until(lambda: all(path.exists() for path in outputs), "batch outputs")
        until(lambda: jobs()["recordings"] >= before+2, "batch recordings")
        assert {path.read_text() for path in outputs} == {"Alpha original","Beta original"}
        undo(); undo(); assert alpha.exists() and beta.exists()
        record("merged-batch-format-and-native-undo")
        select(alpha); trigger("info")
        until(lambda: any(w["class"] in ("KPropertiesDialog", "FinderInfoDialog") for w in top()), "native Get Info")
        assert api.showFileDetailsPage()
        until(lambda: api.setControl("fileDetailsHideExtension",0), "metadata page ready")
        until(lambda: xattr(alpha,"user.finder.hide_extension") == b"0" and display(alpha)=="Alpha.txt", "show per-file extension")
        for mode in range(4):
            assert api.setMode(mode)
            until(lambda: display(alpha)=="Alpha.txt", "extension shown across views")
        assert api.setText("fileDetailsComment","Comments survive native rename, tags and contents.")
        assert api.clickObject("fileDetailsSaveComment")
        until(lambda: xattr(alpha,"user.xdg.comment").decode().startswith("Comments survive"), "save comment")
        assert api.setControl("fileDetailsHideExtension",1)
        until(lambda: xattr(alpha,"user.finder.hide_extension")==b"1" and display(alpha)=="Alpha", "hide per-file extension")
        assert api.setText("fileDetailsName","Renamed Info.txt")
        before=jobs()["recordings"]
        assert api.clickObject("fileDetailsRename")
        info_name=root/"Renamed Info.txt"
        until(info_name.exists,"Get Info native rename")
        until(lambda: unquote(json.loads(str(api.infoState(""))).get("url","")) == unquote(info_name.as_uri()) and not json.loads(str(api.infoState(""))).get("busy",True), "reloaded native Get Info editor")
        coherent=json.loads(str(api.infoState("General")))
        assert coherent["page"]=="General" and coherent["name"]==info_name.name and info_name.name in coherent["nativeEditorNames"],coherent
        assert coherent["width"]>=500 and coherent["height"]>=600,coherent
        assert api.showFileDetailsPage()
        until(lambda: jobs()["recordings"]>before,"Info rename recorded")
        assert info_name.read_text()=="Alpha original" and os.getxattr(info_name,"user.xdg.comment").decode().startswith("Comments survive")
        info_title=next(w["title"] for w in top() if w["class"] in ("KPropertiesDialog", "FinderInfoDialog"))
        screenshot("get-info",info_title)
        native_key(1); until(lambda:not any(w["class"] in ("KPropertiesDialog", "FinderInfoDialog") for w in top()), "embedded Escape closes native Get Info")
        undo(); assert alpha.exists()
        record("native-get-info-metadata-extension-and-rename")
        record("custom-rename-reloads-native-general-name-and-type")
        assert api.setMode(0)
        # Safe repeated completion model updates from actual editor key events.
        trigger("location")
        native_key(30,(29,))
        for key in (53,20,50,25,53,14,53,14,53): native_key(key)
        assert process.poll() is None
        trigger("location")
        home_relative="~/"+str(nested.relative_to(Path.home()))
        assert api.setText("finderLocation",home_relative)
        native_key(28)
        until(lambda: unquote(state()["url"]).rstrip("/")==unquote(nested.as_uri()), "tilde folder")
        trigger("location"); assert api.setText("finderLocation", home_relative+"/missing")
        native_key(28)
        assert api.focusedText()==home_relative+"/missing"
        assert unquote(state()["url"]).rstrip("/")==unquote(nested.as_uri())
        native_key(1); record("safe-completion-tilde-and-invalid-path-retention")
        native_key(103,(125,29))
        until(lambda: len(windows())==2,"enclosing folder new window")
        assert any(unquote(w["url"]).rstrip("/")==unquote(root.as_uri()) for w in windows())
        assert api.closeWindow(); until(lambda:len(windows())==1,"close enclosing folder window")
        record("enclosing-folder-new-window-preserves-source")
        navigate(root); select(nested)
        native_key(20,(125,29))
        until(lambda: any(unquote(row["url"]).rstrip("/")==unquote(nested.as_uri()) for row in json.loads(str(api.sidebarPlaces()))),"native sidebar bookmark")
        native_key(20,(125,29))
        assert sum(unquote(row["url"]).rstrip("/")==unquote(nested.as_uri()) for row in json.loads(str(api.sidebarPlaces())))==1
        assert (Path(os.environ["XDG_DATA_HOME"])/"user-places.xbel").is_file()
        record("add-to-sidebar-native-bookmark-no-duplicates")
        navigate(nested); trigger("path")
        assert api.crumbMenu(str(root))
        QTest.qWait(100)
        assert api.triggerMenuItem("Copy “Documents” as Pathname")
        assert api.clipboardText()==str(root)
        assert unquote(state()["url"]).rstrip("/")==unquote(nested.as_uri())
        record("ancestor-path-copy-preserves-navigation")
        navigate(root); select(image_a,image_b)
        trigger("quickLook"); until(lambda: any(w["title"]=="Quick Look" for w in top()),"Quick Look")
        trigger("slideshow")
        until(lambda: json.loads(str(api.previewState())).get("slideshow") and json.loads(str(api.previewState())).get("fullscreen"),"existing Quick Look slideshow")
        native_key(1); until(lambda:not json.loads(str(api.previewState())).get("slideshow"),"Escape stops slideshow")
        assert api.clickTopLevel("Quick Look","window-close")
        record("slideshow-reuses-quick-look-and-escape-stops")
        # Search presentations retain native Return/Space semantics in every mode.
        trigger("search"); assert api.setControl("finderSearchScope",1); assert api.setText("finderSearch","Alpha")
        until(lambda: state()["searchVisible"] and not state()["searchRunning"] and state()["searchResults"]==1,"search result")
        for mode in range(4):
            assert api.setMode(mode); select(alpha)
            native_key(57); until(lambda:any(w["title"]=="Quick Look" for w in top()),"search Space")
            assert api.clickTopLevel("Quick Look","window-close")
            select(alpha); native_key(28)
            until(lambda:any(w["class"]=="QInputDialog" for w in top()),"search Return rename")
            close_dialog()
        native_key(1)
        record("search-space-and-return-all-four-views")
        select(alpha,beta); trigger("search"); assert api.setText("finderSearch","txt")
        until(lambda:not state()["searchRunning"] and state()["searchResults"]==2,"two search hits")
        select(alpha,beta)
        burst=json.loads(str(api.renameBurst(40,True)))
        assert burst["starts"]==1 and 0 < burst["activeBeforeCancel"] <= 4 and burst["active"]==0 and not burst["busy"],burst
        QTest.qWait(250)
        assert not any(w["class"]=="BatchRenameDialog" for w in top())
        trigger("search"); assert api.setText("finderSearch","txt")
        until(lambda:not state()["searchRunning"] and state()["searchResults"]==2,"search after preflight cancel")
        select(alpha,beta); trigger("rename")
        until(lambda:any(w["class"]=="BatchRenameDialog" for w in top()),"search batch uses asynchronous type validation")
        assert api.setControl("batchRenameMode",1);assert api.setText("batchAddText"," search")
        before=jobs()["recordings"];assert api.dialogButton("Rename")
        search_outputs=[root/"Alpha search.txt",root/"Beta search.txt"]
        until(lambda:all(p.exists() for p in search_outputs),"search batch outputs")
        until(lambda:jobs()["recordings"]>=before+2,"search batch recordings")
        undo();undo();assert alpha.exists() and beta.exists();native_key(1)
        record("search-batch-renaming-and-undo")
        record("repeated-search-rename-bounded-and-cancelled")
        navigate(root); trigger("newTab");navigate(other)
        trigger("previousTab");trigger("allTabs")
        until(lambda:any(w["title"]=="All Tabs" for w in top()),"tab overview")
        screenshot("tab-overview","All Tabs");assert api.dialogButton("Close")
        tabs=json.loads(str(api.tabGeometry()));assert len(tabs)==2
        ox,oy=origin();tab=tabs[0]
        drag(ox+tab["x"]+tab["width"]//2,oy+tab["y"]+tab["height"]//2,0,180)
        until(lambda:len(windows())==2,"native drag tab out")
        assert all(w["tabs"]==1 for w in windows())
        assert {unquote(w["url"]).rstrip("/") for w in windows()}=={unquote(root.as_uri()),unquote(other.as_uri())}
        record("native-tab-drag-detach-and-overview")
        result={"passed":True,"checks":checks,"privateWayland":True,"fixtureOnlyWrites":True}
        (out/"result.json").write_text(json.dumps(result,indent=2)+"\n")
except Exception:
    failure={"checks":checks,"exception":traceback.format_exc()}
    try: failure.update(state=state() if api else None, windows=top() if api else None)
    except Exception: pass
    (out/"failure.json").write_text(json.dumps(failure,indent=2)+"\n")
    raise
finally:
    if native: native.close()
    if process:
        process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired: process.kill();process.wait(timeout=5)
