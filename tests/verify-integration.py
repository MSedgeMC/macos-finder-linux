#!/usr/bin/env python3
"""Exercise real Finder/Qt/KIO behavior on a private Wayland screen.

Run with tools/virtual-screen.py. All filesystem fixtures live on the home
filesystem so native Trash is exercised; settings, clipboard and session bus
belong to the disposable virtual session. No real desktop input is used.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import traceback
from urllib.parse import unquote, urlparse

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-dir", type=Path, default=ROOT / "build/gui-integration")
parser.add_argument("--binary", type=Path, default=ROOT / "build/finder")
parser.add_argument("--smoke", action="store_true", help="Check views, navigation and selection only.")
parser.add_argument("--skip-view-options", action="store_true", help="Continue independent checks while a reported View Options defect is repaired.")
args = parser.parse_args()
assert os.environ.get("KUBUNTU_VIRTUAL_SCREEN") == "1", "Use tools/virtual-screen.py"
assert os.environ.get("WAYLAND_DISPLAY"), "A private native Wayland screen is required"

from PyQt6.QtCore import Qt, QRectF
from PyQt6.QtGui import QColor, QFont, QImage, QPainter, QPdfWriter
from PyQt6.QtTest import QTest
from PyQt6.QtWidgets import QApplication
import dbus
from PIL import Image, ImageChops

out = args.output_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
for old in out.glob("*.png"):
    old.unlink()
for name in ("verification.json", "initial.json", "failure.json"):
    (out / name).unlink(missing_ok=True)

config = Path(os.environ["XDG_CONFIG_HOME"])
data = Path(os.environ["XDG_DATA_HOME"])
working_config = Path(os.environ["KUBUNTU_WORKING_CONFIG"])
working_data = Path(os.environ["KUBUNTU_WORKING_DATA"])
for name in ("kdeglobals", "fontconfig/fonts.conf"):
    source, target = working_config / name, config / name
    if source.is_file():
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
for theme in ("MacTahoe", "MacTahoe-dark"):
    source, target = working_data / "icons" / theme, data / "icons" / theme
    if source.is_dir():
        shutil.copytree(source, target, dirs_exist_ok=True, symlinks=True)

app = QApplication([])
fixture_scope = tempfile.TemporaryDirectory(prefix="fixtures-", dir=out)
fixtures = Path(fixture_scope.name) / "Documents"
fixtures.mkdir()
reports, artwork = fixtures / "Reports", fixtures / "Artwork"
reports.mkdir()
artwork.mkdir()
nested = artwork / "Nested Folder"
nested.mkdir()
(nested / "Inside.txt").write_text("Nested columns content.\n")
source = fixtures / "Copy Source.txt"
content = b"Content must survive native copy, rename, move and Trash.\n"
source.write_bytes(content)
notes = fixtures / "Project Notes.txt"
notes.write_text("Read-only Quick Look verification.\nSecond line.\n")
collision = reports / source.name
collision.write_bytes(b"Destination collision must not be overwritten.\n")
(fixtures / ".Hidden Note.txt").write_text("Hidden fixture.\n")
image_path = fixtures / "Color Sample.png"
image = QImage(640, 400, QImage.Format.Format_ARGB32)
image.fill(QColor("#3e6990"))
painter = QPainter(image)
painter.fillRect(60, 70, 200, 240, QColor("#d8ac7a"))
painter.fillRect(280, 110, 300, 190, QColor("#72aaa6"))
painter.setPen(QColor("#f8f6ef"))
painter.setFont(QFont("Ubuntu", 25))
painter.drawText(QRectF(0, 325, 640, 65), int(Qt.AlignmentFlag.AlignCenter), "Finder preview fixture")
painter.end()
assert image.save(str(image_path))
pdf_path = fixtures / "Two Pages.pdf"
pdf = QPdfWriter(str(pdf_path))
pdf.setResolution(72)
pdf.setTitle("Finder two-page fixture")
painter = QPainter(pdf)
painter.setFont(QFont("Ubuntu", 24))
painter.drawText(45, 90, "Quick Look PDF · page one")
assert pdf.newPage()
painter.drawText(45, 90, "Quick Look PDF · page two")
painter.end()
del pdf
initial_hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in (source, notes, image_path, pdf_path)}

log = (out / "finder.log").open("w")
env = os.environ.copy()
env["QT_NO_XDG_DESKTOP_PORTAL"] = "1"
process = subprocess.Popen([str(args.binary.resolve()), "--test-mode", str(fixtures)],
                           env=env, stdout=log, stderr=subprocess.STDOUT)
bus = dbus.SessionBus()
api = None
checks = {}
initial = None


def wait(milliseconds=100):
    QTest.qWait(milliseconds)


def local(url):
    parsed = urlparse(str(url))
    return unquote(parsed.path).rstrip("/") if parsed.scheme == "file" else str(url)


def status():
    return json.loads(str(api.diagnostics(timeout=3)))


def top_levels():
    return json.loads(str(api.topLevels(timeout=3)))


def until(predicate, label, timeout=10):
    deadline = time.monotonic() + timeout
    value = None
    while time.monotonic() < deadline:
        assert process.poll() is None, (out / "finder.log").read_text()
        value = predicate()
        if value:
            return value
        wait(100)
    raise AssertionError(f"Timed out: {label}; state={status()}; topLevels={top_levels()}")


def record(name, detail=True):
    checks[name] = detail
    print("PASS", name, flush=True)
    (out / "verification.json").write_text(json.dumps({
        "state": "running", "checks": checks, "fixtureOnlyWrites": True,
    }, indent=2) + "\n")


def operation_complete(kind=None):
    if "undoText" in status() and kind:
        until(lambda: status().get("undoEnabled") and kind.lower() in status()["undoText"].lower(),
              f"native Undo available after {kind}")
    else:
        # Native jobs can move an inode before delivering the completion that
        # records Undo; give this disposable fixture job time to finish.
        wait(500)


def navigate(path):
    assert api.navigate(str(path), timeout=3)
    until(lambda: local(status()["url"]) == str(path), f"navigate {path.name}")
    expected = len([p for p in path.iterdir() if not p.name.startswith(".")])
    until(lambda: status()["count"] == expected, f"list {path.name}: {expected} items")


def select(*paths):
    assert api.selectFiles([str(p) for p in paths], timeout=3)
    expected = {str(p) for p in paths}
    until(lambda: {local(p) for p in status()["selected"]} == expected,
          "native selected URLs " + repr(expected))


def trigger(name):
    assert api.trigger(name, timeout=3), name
    wait(100)


def key(code, modifiers=0, text=""):
    assert api.key(int(code), int(modifiers), text, timeout=3)
    wait(100)


def capture(name, title=None):
    path = out / f"{name}.png"
    wait(150)
    okay = api.captureTopLevel(title, str(path), timeout=3) if title else api.capture(str(path), timeout=3)
    assert okay and path.is_file() and path.stat().st_size > 1000, path


def visible_preview():
    return next((w for w in top_levels() if w["title"] == "Quick Look"), None)


def label(window, name):
    return next((item["text"] for item in window.get("labels", []) if item["name"] == name), None)


def evidence(window, name):
    return next((item for item in window.get("labels", []) if item["name"] == name), {})


def image_rendered(name, pdf=False):
    # Check captured pixels as well as the widget selection. A visible image
    # QLabel can otherwise pass while retaining a stale thumbnail-sized pixmap.
    screenshot = Image.open(out / f"{name}.png").convert("RGB")
    if pdf:
        count = sum(r > 240 and g > 240 and b > 240 for r, g, b in screenshot.get_flattened_data())
    else:
        count = sum(b > 100 and b > r + 10 and b > g + 10 for r, g, b in screenshot.get_flattened_data())
    assert count > 50000, f"Preview did not render at useful size: {name}: {count} pixels"


def idle_cpu(name):
    def ticks():
        fields = Path(f"/proc/{process.pid}/stat").read_text().split(") ", 1)[1].split()
        return int(fields[11]) + int(fields[12])
    wait(300)
    begin, stamp = ticks(), time.monotonic()
    wait(1000)
    seconds = (ticks() - begin) / os.sysconf("SC_CLK_TCK")
    percent = seconds / (time.monotonic() - stamp) * 100
    assert percent < 15, f"Sustained idle renderer CPU: {name}: {percent:.1f}%"
    record(name, {"processCpuPercent": round(percent, 2), "sampleSeconds": 1})


def close_preview():
    assert api.clickTopLevel("Quick Look", "window-close", timeout=3)
    until(lambda: not visible_preview(), "close Quick Look")


def dismiss_error():
    # User-cancelled KIO jobs may display a non-destructive error acknowledgement.
    for _ in range(4):
        dialogs = [w for w in top_levels() if w["class"] in ("QMessageBox", "KMessageBox")]
        if not dialogs:
            return
        assert api.dialogButton("OK", timeout=3), dialogs
        wait(150)


def prepare_tags():
    assert api.prepareTagMenu(timeout=3)
    if "tagsReady" in status():
        until(lambda: status()["tagsReady"] and not status()["tagsBusy"], "tag metadata ready")
    else:
        wait(250)


try:
    until(lambda: bus.name_has_owner("org.local.FinderTest"), "Finder test bridge", timeout=20)
    api = dbus.Interface(bus.get_object("org.local.FinderTest", "/Test"), "org.local.FinderTest")
    navigate(fixtures)
    initial = status()
    (out / "initial.json").write_text(json.dumps(initial, indent=2) + "\n")
    assert initial["headerHeight"] == 52 and initial["tabs"] == 1, initial
    record("nativeWaylandWindow")

    select(source, notes)
    for mode, name in enumerate(("icons", "list", "columns", "gallery")):
        assert api.setMode(mode, timeout=3)
        until(lambda: status()["mode"] == mode, name)
        until(lambda: {local(p) for p in status()["selected"]} == {str(source), str(notes)},
              f"selection survives {name}")
        capture(f"finder-{name}")
        record(f"{name}ViewWithNativeSelection")
    assert api.setMode(0, timeout=3)
    navigate(reports)
    trigger("back")
    until(lambda: local(status()["url"]) == str(fixtures), "back")
    trigger("forward")
    until(lambda: local(status()["url"]) == str(reports), "forward")
    record("backForwardHistory")
    navigate(fixtures)
    trigger("newTab")
    until(lambda: status()["tabs"] == 2, "new tab")
    navigate(artwork)
    trigger("previousTab")
    until(lambda: local(status()["url"]) == str(fixtures), "independent previous tab")
    trigger("nextTab")
    until(lambda: local(status()["url"]) == str(artwork), "independent next tab")
    trigger("close")
    until(lambda: status()["tabs"] == 1 and local(status()["url"]) == str(fixtures), "close active tab")
    record("independentTabs")

    assert api.setMode(2, timeout=3)
    select(artwork)
    until(lambda: local(status()["url"]) == str(artwork), "columns select folder")
    until(lambda: any(item["rows"] == 1 for item in status()["visibleLists"]), "columns children loaded")
    select(nested)
    until(lambda: local(status()["url"]) == str(nested), "columns nested selection")
    until(lambda: len(status()["visibleLists"]) >= 3, "three native columns")
    capture("finder-nested-columns")
    record("realNestedColumnNavigation")
    assert api.setMode(0, timeout=3)
    navigate(fixtures)
    assert api.resizeWindow(700, 440, timeout=3)
    until(lambda: status()["width"] == 700, "narrow layout")
    capture("finder-narrow")
    assert api.resizeWindow(1100, 720, timeout=3)
    record("responsiveNarrowWindow")

    if not args.smoke:
        assert api.setMode(0, timeout=3)
        until(lambda: status()["mode"] == 0, "Icons before separate Quick Look")
        select(image_path)
        key(Qt.Key.Key_Space)
        preview = until(visible_preview, "Space Quick Look image")
        until(lambda: label(visible_preview(), "previewName") == image_path.name, "image preview name")
        assert label(visible_preview(), "previewImage") is not None, visible_preview()
        assert evidence(visible_preview(), "previewContent").get("current") == "previewImage"
        capture("quick-look-image", "Quick Look")
        image_rendered("quick-look-image")
        idle_cpu("staticImagePreviewIdleCPU")
        key(Qt.Key.Key_Space)
        until(lambda: not visible_preview(), "Space closes Quick Look")
        record("spaceQuickLookImage")
        select(pdf_path)
        trigger("quickLook")
        preview = until(visible_preview, "PDF Quick Look")
        until(lambda: label(visible_preview(), "previewPageCount") == "Page 1 of 2", "native PDF two pages")
        capture("quick-look-pdf-page-1", "Quick Look")
        image_rendered("quick-look-pdf-page-1", pdf=True)
        assert api.clickTopLevel("Quick Look", "previewNextPage", timeout=3)
        until(lambda: label(visible_preview(), "previewPageCount") == "Page 2 of 2", "PDF next page")
        capture("quick-look-pdf-page-2", "Quick Look")
        image_rendered("quick-look-pdf-page-2", pdf=True)
        first = Image.open(out / "quick-look-pdf-page-1.png").convert("RGB")
        second_page = Image.open(out / "quick-look-pdf-page-2.png").convert("RGB")
        page_crop = (240, 75, 600, 525)
        assert ImageChops.difference(first.crop(page_crop), second_page.crop(page_crop)).getbbox(), "PDF page pixels did not change"
        close_preview()
        record("nativeMultipagePDFQuickLook")
        select(image_path, notes)
        trigger("quickLook")
        until(visible_preview, "multiple-file Quick Look")
        until(lambda: label(visible_preview(), "previewCount") == "1 of 2", "multi-file first")
        assert api.clickTopLevel("Quick Look", "previewNext", timeout=3)
        until(lambda: label(visible_preview(), "previewCount") == "2 of 2", "multi-file next")
        assert label(visible_preview(), "previewName") == notes.name, visible_preview()
        text_evidence = evidence(visible_preview(), "previewText")
        assert text_evidence.get("readOnly") and text_evidence.get("text") == notes.read_text(), text_evidence
        capture("quick-look-text", "Quick Look")
        close_preview()
        record("multiFileQuickLookNavigation")
        assert api.setMode(3, timeout=3)
        select(image_path)
        capture("finder-gallery-image")
        image_rendered("finder-gallery-image")
        idle_cpu("staticGalleryIdleCPU")
        record("galleryImagePreview")
        assert api.setMode(0, timeout=3)

        select(source)
        trigger("search")
        assert api.setControl("finderSearchScope", 1, timeout=3)
        assert api.setText("finderSearch", "Inside", timeout=3)
        until(lambda: status()["searchVisible"] and not status()["searchRunning"]
              and status()["searchResults"] == 1, "recursive nested search")
        capture("finder-recursive-search")
        inside = nested / "Inside.txt"
        select(inside)
        trigger("trash")
        until(lambda: not inside.exists(), "search result Trash")
        operation_complete("trash")
        assert source.read_bytes() == content
        trigger("undo")
        until(inside.is_file, "undo search result Trash")
        assert inside.read_text() == "Nested columns content.\n" and source.read_bytes() == content
        record("searchActionsOperateOnResultsAndUndo")
        key(Qt.Key.Key_Escape)
        until(lambda: not status()["searchVisible"] and status()["search"] == "", "Escape restores folder")
        assert local(status()["url"]) == str(fixtures)
        record("recursiveSearchAndEscapeRestore")
        trigger("location")
        assert api.setText("finderLocation", str(nested), timeout=3)
        key(Qt.Key.Key_End)
        key(Qt.Key.Key_Space, text=" ")
        assert not visible_preview() and status()["focus"] == "QLineEdit", status()
        key(Qt.Key.Key_Escape)
        record("spaceInLocationRemainsText")
        before_bad = status()["url"]
        assert api.navigate(str(fixtures / "Does Not Exist"), timeout=3)
        wait(200)
        assert status()["url"] == before_bad
        assert not (fixtures / "Does Not Exist").exists()
        record("unavailableFolderPreservesNavigation")

        # Exercise the actual named toolbar QAction after its asynchronous
        # metadata read, preserving a custom tag added by another application.
        os.setxattr(notes, "user.xdg.tags", b"Existing Custom Tag")
        select(notes)
        prepare_tags()
        trigger("tagRed")
        until(lambda: b"Red" in os.getxattr(notes, "user.xdg.tags"), "toolbar adds red tag")
        assert b"Existing Custom Tag" in os.getxattr(notes, "user.xdg.tags")
        until(lambda: not status()["tagsBusy"], "tag write and read completed")
        prepare_tags()
        trigger("tagRed")
        until(lambda: b"Red" not in os.getxattr(notes, "user.xdg.tags"), "toolbar removes red tag")
        assert os.getxattr(notes, "user.xdg.tags") == b"Existing Custom Tag"
        os.removexattr(notes, "user.xdg.tags")
        record("nativeToolbarTagTogglePreservesCustomTag")

        if not args.skip_view_options:
            trigger("viewOptions")
            options_title = "View Options — Documents"
            until(lambda: any(w["title"] == options_title for w in top_levels()), "native View Options")
            assert api.setControl("viewOptionMode", 1, timeout=3)
            assert api.setControl("viewOptionIconSize", 32, timeout=3)
            assert api.setControl("viewOptionSort", 2, timeout=3)
            assert api.setControl("viewOptionReverse", 1, timeout=3)
            assert api.setControl("viewOptionColumn1", 0, timeout=3)
            capture("finder-view-options", options_title)
            assert api.dialogButton("Save", timeout=3)
            until(lambda: status()["mode"] == 1 and status()["iconSize"] == 32 and not status()["sortAscending"], "View Options Save")
            saved_sort = status()["sortColumn"]
            navigate(artwork)
            navigate(fixtures)
            until(lambda: status()["mode"] == 1 and status()["iconSize"] == 32
                  and status()["sortColumn"] == saved_sort and not status()["sortAscending"], "per-folder view restore")
            assert 1 in status()["hiddenColumns"], status()
            trigger("viewOptions")
            until(lambda: any(w["title"] == options_title for w in top_levels()), "restored View Options")
            capture("finder-view-options-restored", options_title)
            assert api.dialogButton("Close", timeout=3)
            record("nativeViewOptionsSaveAndFolderPersistence")
            assert api.setMode(0, timeout=3)

        before = status()["count"]
        watched = fixtures / "Watched Addition.txt"
        watched.write_text("Added outside the application.\n")
        until(lambda: status()["count"] == before + 1, "filesystem watcher addition")
        select(watched)
        watched.unlink()
        until(lambda: status()["count"] == before, "filesystem watcher removal")
        record("filesystemWatching")

        select(source)
        trigger("copy")
        navigate(artwork)
        trigger("paste")
        copied = artwork / source.name
        until(lambda: copied.is_file(), "native copy/paste")
        assert copied.read_bytes() == content and source.read_bytes() == content
        record("nativeCopyPastePreservesContents")
        select(copied)
        trigger("rename")
        until(lambda: status()["focus"] in ("QTextEdit", "QLineEdit"), "inline native rename editor")
        key(Qt.Key.Key_A, Qt.KeyboardModifier.ControlModifier.value)
        key(0, text="Renamed Copy.txt")
        capture("finder-inline-rename")
        key(Qt.Key.Key_Return)
        renamed = artwork / "Renamed Copy.txt"
        until(lambda: renamed.is_file() and not copied.exists(), "rename with spaces")
        operation_complete("rename")
        assert renamed.read_bytes() == content
        record("nativeInlineRenameWithSpaces")
        trigger("undo")
        until(lambda: copied.is_file() and not renamed.exists(), "undo rename")
        assert copied.read_bytes() == content
        record("nativeUndoRename")

        before_jobs = json.loads(str(api.nativeJobState()))
        trigger("newFolder")
        until(lambda: (artwork / "Untitled Folder").is_dir(), "new folder inline rename")
        assert api.dialogText("New Test Folder", timeout=3)
        created = artwork / "New Test Folder"
        until(created.is_dir, "native new folder")
        until(lambda: json.loads(str(api.nativeJobState()))["recordings"] >= before_jobs["recordings"] + 2,
              "folder creation and inline rename recording")
        trigger("undo")
        until(lambda: json.loads(str(api.nativeJobState()))["undoCompletions"] == before_jobs["undoCompletions"] + 1,
              "undo inline folder rename finished")
        assert not created.exists() and (artwork / "Untitled Folder").is_dir()
        trigger("undo")
        until(lambda: json.loads(str(api.nativeJobState()))["undoCompletions"] == before_jobs["undoCompletions"] + 2,
              "undo folder creation finished")
        assert not (artwork / "Untitled Folder").exists()
        record("nativeNewFolderAndUndo")

        select(copied)
        trigger("cut")
        navigate(nested)
        trigger("paste")
        moved = nested / copied.name
        until(lambda: moved.is_file() and not copied.exists(), "cut paste move")
        operation_complete("move")
        assert moved.read_bytes() == content
        trigger("undo")
        until(lambda: copied.is_file() and not moved.exists(), "undo cut paste")
        assert copied.read_bytes() == content
        record("nativeCutPasteAndUndo")
        navigate(artwork)
        select(copied)
        trigger("trash")
        until(lambda: not copied.exists(), "native Trash")
        operation_complete("trash")
        trash_files = data / "Trash/files"
        assert trash_files.is_dir() and any(p.read_bytes() == content for p in trash_files.iterdir() if p.is_file())
        trigger("undo")
        until(copied.is_file, "restore Trash via Undo")
        assert copied.read_bytes() == content
        record("nativeTrashAndUndo")

        navigate(fixtures)
        select(source)
        trigger("copy")
        navigate(reports)
        before_collision = collision.read_bytes()
        trigger("paste")
        until(lambda: len(top_levels()) > 1, "native destination collision dialog")
        capture("finder-copy-collision", next(w["title"] for w in top_levels() if w["title"] != "Finder"))
        assert api.dialogButton("Cancel", timeout=3), top_levels()
        wait(500)
        dismiss_error()
        assert source.read_bytes() == content and collision.read_bytes() == before_collision
        record("nativeCollisionCancelPreservesBothFiles")

        navigate(fixtures)
        select(source)
        trigger("duplicate")
        duplicate = fixtures / "Copy Source copy.txt"
        until(duplicate.is_file, "native duplicate")
        select(source)
        trigger("duplicate")
        second = fixtures / "Copy Source copy 2.txt"
        until(second.is_file, "duplicate collision naming")
        assert duplicate.read_bytes() == second.read_bytes() == source.read_bytes() == content
        record("nativeDuplicateAvoidsCollision")

        # Standard FileManager1 activation and a second executable must reach
        # the same private running process, preserving native reveal selection.
        file_manager = dbus.Interface(bus.get_object("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1"),
                                      "org.freedesktop.FileManager1")
        manager = dbus.Interface(bus.get_object("org.local.Finder", "/Finder"), "org.local.Finder")
        window_count = int(manager.windowCount(timeout=3))
        file_manager.ShowItems([source.as_uri()], "", timeout=3)
        until(lambda: int(manager.windowCount(timeout=3)) == window_count + 1, "FileManager1 ShowItems")
        until(lambda: str(source) in {local(p) for p in status()["selected"]}, "FileManager1 native reveal selection")
        capture("finder-filemanager1-reveal")
        assert api.closeWindow(timeout=3)
        until(lambda: int(manager.windowCount(timeout=3)) == window_count, "close revealed window")
        file_manager.ShowFolders([artwork.as_uri()], "", timeout=3)
        until(lambda: int(manager.windowCount(timeout=3)) == window_count + 1, "FileManager1 ShowFolders")
        until(lambda: local(status()["url"]) == str(artwork), "FileManager1 folder opened")
        assert api.closeWindow(timeout=3)
        until(lambda: int(manager.windowCount(timeout=3)) == window_count, "close opened folder")
        forwarded = subprocess.run([str(args.binary.resolve()), "--select", str(notes)], env=env,
                                   capture_output=True, text=True, timeout=10)
        assert forwarded.returncode == 0, forwarded.stderr
        until(lambda: int(manager.windowCount(timeout=3)) == window_count + 1, "CLI forwards to existing process")
        until(lambda: str(notes) in {local(p) for p in status()["selected"]}, "CLI reveal selection")
        assert api.closeWindow(timeout=3)
        until(lambda: int(manager.windowCount(timeout=3)) == window_count, "close CLI revealed window")
        record("fileManager1AndCLISingleInstanceForwarding")

        for path in (source, notes, image_path, pdf_path):
            assert hashlib.sha256(path.read_bytes()).hexdigest() == initial_hashes[path.name], path
        record("originalFixtureContentsUnchanged")
    capture("finder-final")
    result = {"state": "passed", "checks": checks, "initial": initial, "final": status(),
              "fixtureOnlyWrites": True, "isolatedWayland": True, "smoke": args.smoke,
              "skipViewOptions": args.skip_view_options}
    (out / "verification.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)
except Exception:
    failure = {"state": "failed", "checks": checks, "exception": traceback.format_exc()}
    if api:
        try:
            failure["diagnostics"] = status()
            failure["topLevels"] = top_levels()
            api.capture(str(out / "failure.png"), timeout=3)
        except Exception:
            pass
    (out / "failure.json").write_text(json.dumps(failure, indent=2) + "\n")
    (out / "verification.json").write_text(json.dumps(failure, indent=2) + "\n")
    raise
finally:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    log.close()
    fixture_scope.cleanup()
