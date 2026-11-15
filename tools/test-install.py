#!/usr/bin/env python3
"""Exercise scoped deployment and interrupted rollback in a temporary HOME.

All desktop commands and D-Bus endpoints are fakes. No live session is touched.
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("finder_installer", HERE / "install.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class BusError(Exception):
    pass


class FakeDesktop:
    def __init__(self):
        self.calls = []
        self.active = False
        self.window_count = 0
        self.endpoint_failures = 0
        self.test_owner = False
        self.owner_mismatch = False
        self.fail_command = None
        self.finder_owner = True

    def run(self, *command):
        self.calls.append(command)
        if command == self.fail_command:
            raise RuntimeError("injected desktop command failure")
        if command[:2] == ("xdg-mime", "query"):
            if installer.MIME.exists() and b"inode/directory=org.local.finder.desktop;" in installer.MIME.read_bytes():
                return installer.DESKTOP
            return "org.kde.dolphin.desktop"
        if command == ("kbuildsycoca6", "--noincremental"):
            return ""
        if command[:3] == ("systemctl", "--user", "show"):
            if "ExecStart" in command:
                return "{ path=" + str(installer.HOME / ".local/bin/finder") + " ; argv[]=" + str(installer.HOME / ".local/bin/finder") + " --daemon ; }"
            return "ActiveState=" + ("active" if self.active else "inactive") + "\nSubState=" + ("running" if self.active else "dead") + "\nMainPID=" + ("1234" if self.active else "0")
        if command[:2] == ("systemctl", "--user"):
            if command[2] in ("start", "restart"):
                self.active = True
            elif command[2] == "stop":
                self.active = False
            elif command[2] != "daemon-reload":
                raise AssertionError(command)
            return ""
        raise AssertionError(command)

    def name_has_owner(self, name):
        if name == "org.local.FinderTest":
            return self.test_owner
        if name == "org.local.Finder":
            return self.active and self.finder_owner
        return self.active

    def get_name_owner(self, name):
        return ":1.12" if name != "org.freedesktop.FileManager1" or not self.owner_mismatch else ":1.13"

    def get_object(self, name, path):
        if path not in {"/Finder", "/org/freedesktop/FileManager1"}:
            raise AssertionError((name, path))
        return self

    def windowCount(self, **_):
        return self.window_count

    def Introspect(self, **_):
        if self.endpoint_failures:
            self.endpoint_failures -= 1
            raise BusError("registration race")
        return '<node><interface name="org.freedesktop.FileManager1">' + "".join('<method name="' + name + '"/>' for name in ("ShowFolders", "ShowItems", "ShowItemProperties")) + '</interface></node>'


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="finder-installer-test-")
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        home = root / "home"
        home.mkdir()
        globals_ = {
            "ROOT": root / "source", "HOME": home, "DATA": home / ".local/share",
            "CONFIG": home / ".config", "STATE": home / ".local/state/kubuntu-customization",
            "SOURCE": root / "source/build/finder", "EXECUTABLE": home / ".local/share/finder-v2/bin/finder",
            "MIME": home / ".config/mimeapps.list",
        }
        for key, value in globals_.items():
            p = patch.object(installer, key, value)
            p.start()
            self.addCleanup(p.stop)
        self.old_umask = os.umask(0o022)
        self.addCleanup(os.umask, self.old_umask)
        self.write(installer.SOURCE, b"verified test binary\n", 0o755)
        self.write(installer.HOME / ".local/bin/dolphin", b"old dolphin wrapper\n", 0o750)
        self.original_mime = b"# Keep this comment\n[Default Applications]\nx-scheme-handler/https=thorium.desktop;\nx-scheme-handler/t3code=t3code.desktop;\n\n[Added Associations]\ntext/plain=org.kde.kate.desktop;\n"
        self.write(installer.MIME, self.original_mime, 0o640)
        self.write(installer.DATA / "applications/mimeapps.list", b"[Added Associations]\ninode/directory=org.kde.dolphin.desktop;\n", 0o644)
        self.write(installer.DATA / "applications/org.kde.dolphin.desktop", b"[Desktop Entry]\nType=Application\nName=Dolphin\nName[hi]=old translated name\nExec=dolphin %u\nTryExec=dolphin\nDBusActivatable=true\nX-DBUS-ServiceName=org.kde.dolphin\nStartupWMClass=dolphin\nX-DocPath=dolphin/index.html\nCategories=System;FileManager;\nMimeType=inode/directory;\nActions=new-window;\n\n[Desktop Action new-window]\nName=Old action\nExec=dolphin --new-window\n", 0o644)
        self.originals = {str(path): (path.read_bytes(), path.stat().st_mode & 0o777) if path.exists() else None for path in installer.deployment_paths()}
        self.desktop = FakeDesktop()
        for p in (patch.object(installer, "run", self.desktop.run),
                  patch.object(Path, "iterdir", autospec=True, side_effect=lambda path: iter(()) if path == Path("/proc") else self.real_iterdir(path)),
                  patch.dict(sys.modules, {"dbus": types.SimpleNamespace(SessionBus=lambda: self.desktop, DBusException=BusError)})):
            p.start()
            self.addCleanup(p.stop)
        self.output = io.StringIO()
        p = contextlib.redirect_stdout(self.output)
        p.__enter__()
        self.addCleanup(p.__exit__, None, None, None)

    real_iterdir = Path.iterdir

    @staticmethod
    def write(path, data, mode):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)

    def assert_originals(self):
        for name, original in self.originals.items():
            path = Path(name)
            if original is None:
                self.assertFalse(path.exists(), name)
            else:
                self.assertEqual(path.read_bytes(), original[0], name)
                self.assertEqual(path.stat().st_mode & 0o777, original[1], name)

    def test_roundtrip_preserves_other_associations_and_default_service(self):
        state = installer.prepare()
        self.assert_originals()
        installer.apply(state)
        report = installer.verify(state)
        self.assertTrue(report["passed"])
        self.assertEqual(installer.MIME.stat().st_mode & 0o777, 0o640)
        self.assertEqual(report["window_count"], 0)
        text = installer.MIME.read_text()
        self.assertIn(self.original_mime.decode().split("[Added Associations]")[0], text)
        self.assertIn("text/plain=org.kde.kate.desktop;", text)
        desktop = (installer.DATA / "applications/org.local.finder.desktop").read_text()
        self.assertIn("Name=Finder\n", desktop)
        self.assertIn("StartupWMClass=Finder\n", desktop)
        self.assertNotIn("Name[", desktop)
        self.assertNotIn("X-DBUS-ServiceName", desktop)
        self.assertNotIn("Desktop Action", desktop)
        self.assertIn("Exec=/usr/bin/dolphin %U", (installer.DATA / "applications/org.local.dolphin-original.desktop").read_text())
        installer.restore(state)
        self.assert_originals()
        self.assertFalse(self.desktop.active)
        self.assertEqual(json.loads((state / "manifest.json").read_text())["state"], "restored")
        self.assertTrue(all(command[0] in {"xdg-mime", "systemctl", "kbuildsycoca6"} for command in self.desktop.calls))
        for command in self.desktop.calls:
            if command[0] == "systemctl" and command[2] in {"stop", "start", "restart"}:
                self.assertEqual(command[3], installer.UNIT)

    def test_apply_uses_staged_binary_if_build_changes(self):
        state = installer.prepare()
        snapshot = installer.SOURCE.read_bytes()
        installer.SOURCE.write_bytes(b"later unverified build\n")
        installer.apply(state)
        self.assertEqual(installer.EXECUTABLE.read_bytes(), snapshot)
        self.assertEqual(installer.verify(state)["source_hash"], installer.digest(installer.EXECUTABLE))

    def test_changed_live_file_blocks_apply_before_any_writes(self):
        state = installer.prepare()
        installer.MIME.write_bytes(self.original_mime + b"# user edit\n")
        with self.assertRaisesRegex(RuntimeError, "changed since preparation"):
            installer.apply(state)
        self.assertFalse(installer.EXECUTABLE.exists())
        self.assertFalse(any(command[2] == "restart" for command in self.desktop.calls if command[0] == "systemctl"))

    def test_changed_staged_mode_or_backup_blocks_apply(self):
        for kind in ("stage", "backup"):
            with self.subTest(kind=kind):
                state = installer.prepare()
                manifest = json.loads((state / "manifest.json").read_text())
                if kind == "stage":
                    Path(manifest["proposed"][0]["staged"]).chmod(0o644)
                else:
                    Path(next(item["backup"] for item in manifest["files"] if item["existed"])).write_bytes(b"damaged")
                with self.assertRaisesRegex(RuntimeError, "changed"):
                    installer.apply(state)
                self.assert_originals()

    def test_kill_between_replacement_and_completion_is_recoverable(self):
        state = installer.prepare()
        real = installer.atomic_write
        def kill_after_replace(path, content, mode):
            real(path, content, mode)
            if path == installer.EXECUTABLE:
                raise KeyboardInterrupt("simulated process kill")
        with patch.object(installer, "atomic_write", side_effect=kill_after_replace):
            with self.assertRaises(KeyboardInterrupt):
                installer.apply(state)
        manifest = json.loads((state / "manifest.json").read_text())
        self.assertEqual(manifest["state"], "installing")
        self.assertEqual(len(manifest["write_intents"]), 1)
        self.assertEqual(manifest["installed"], [])
        self.assertTrue(installer.EXECUTABLE.exists())
        installer.restore(state)
        self.assert_originals()

    def test_restore_resumes_after_restoring_file_before_journal(self):
        state = installer.prepare()
        installer.apply(state)
        real = installer.atomic_write
        def kill_after_restore(path, content, mode):
            real(path, content, mode)
            if path == installer.HOME / ".local/bin/dolphin":
                raise KeyboardInterrupt("simulated process kill")
        with patch.object(installer, "atomic_write", side_effect=kill_after_restore):
            with self.assertRaises(KeyboardInterrupt):
                installer.restore(state)
        self.assertEqual(json.loads((state / "manifest.json").read_text())["state"], "restoring")
        installer.restore(state)
        self.assert_originals()

    def test_newer_user_edits_block_restore_before_service_stop(self):
        state = installer.prepare()
        installer.apply(state)
        installer.MIME.write_bytes(installer.MIME.read_bytes() + b"# user edit\n")
        self.desktop.calls.clear()
        with self.assertRaisesRegex(RuntimeError, "Newer installed edits were preserved"):
            installer.restore(state)
        self.assertTrue(self.desktop.active)
        self.assertFalse(any(command[2] == "stop" for command in self.desktop.calls if command[0] == "systemctl"))

    def test_failed_service_restart_can_be_restored(self):
        state = installer.prepare()
        self.desktop.fail_command = ("systemctl", "--user", "restart", installer.UNIT)
        with self.assertRaisesRegex(RuntimeError, "scoped recovery"):
            installer.apply(state)
        self.desktop.fail_command = None
        self.assertEqual(json.loads((state / "manifest.json").read_text())["state"], "failed")
        installer.restore(state)
        self.assert_originals()

    def test_unexpected_manifest_scope_and_symlink_parents_are_rejected(self):
        state = installer.prepare()
        manifest = json.loads((state / "manifest.json").read_text())
        manifest["proposed"][0]["path"] = str(installer.HOME / ".config/kdeglobals")
        installer.save_manifest(state, manifest)
        with self.assertRaisesRegex(RuntimeError, "Unexpected deployment scope"):
            installer.apply(state)
        self.assert_originals()
        actual = installer.HOME / "elsewhere"
        actual.mkdir()
        (installer.HOME / ".local/share/finder-v2").symlink_to(actual)
        with self.assertRaisesRegex(RuntimeError, "symlink"):
            installer.prepare()
        self.assertFalse((actual / "bin/finder").exists())

    def test_daemon_window_count_prevents_restart_and_rechecks_before_restart(self):
        self.desktop.active = True
        self.desktop.window_count = 1
        with self.assertRaisesRegex(RuntimeError, "open windows"):
            installer.prepare()
        self.desktop.window_count = 0
        state = installer.prepare()
        real = self.desktop.run
        def opening_window(*command):
            result = real(*command)
            if command == ("systemctl", "--user", "daemon-reload"):
                self.desktop.window_count = 1
            return result
        with patch.object(installer, "run", side_effect=opening_window):
            with self.assertRaisesRegex(RuntimeError, "scoped recovery"):
                installer.apply(state)
        self.assertFalse(any(command[2] == "restart" for command in self.desktop.calls if command[0] == "systemctl"))
        self.desktop.window_count = 0
        installer.restore(state)
        self.assert_originals()
        self.assertTrue(self.desktop.active)

    def test_introspection_waits_for_registration_and_refuses_other_owner(self):
        state = installer.prepare()
        self.desktop.endpoint_failures = 2
        installer.apply(state)
        self.assertEqual(self.desktop.endpoint_failures, 0)
        self.desktop.owner_mismatch = True
        with patch.object(installer.time, "monotonic", side_effect=[0, 6]):
            with self.assertRaisesRegex(RuntimeError, "Another file manager"):
                installer.verify(state)

    def test_active_original_dolphin_without_window_count_is_not_activated_or_stopped(self):
        self.desktop.active = True
        self.desktop.finder_owner = False
        with patch.object(self.desktop, "get_object", side_effect=AssertionError("read-only check activated a service")):
            with self.assertRaisesRegex(RuntimeError, "cannot report its window count"):
                installer.prepare()
        self.assertFalse(any(command[2] in {"stop", "restart"} for command in self.desktop.calls if command[0] == "systemctl"))

    def test_mime_insertion_and_replacement_preserve_neighboring_lines(self):
        cases = (
            (b"", b"[Default Applications]\ninode/directory=org.local.finder.desktop;\n"),
            (b"[Added Associations]\ntext/plain=kate.desktop;", b"[Added Associations]\ntext/plain=kate.desktop;\n[Default Applications]\ninode/directory=org.local.finder.desktop;\n"),
            (b"[Default Applications]\n# keep\ninode/directory=old.desktop;\ntext/plain=kate.desktop;\n", b"[Default Applications]\n# keep\ninode/directory=org.local.finder.desktop;\ntext/plain=kate.desktop;\n"),
        )
        for original, expected in cases:
            with self.subTest(original=original):
                installer.MIME.write_bytes(original)
                self.assertEqual(installer.mime_contents(), expected)

    def test_existing_finder_preferences_keep_mode_and_reject_added_permissions(self):
        self.write(installer.MIME, b"[Default Applications]\ninode/directory=org.local.finder.desktop;\n", 0o664)
        state = installer.prepare()
        manifest = json.loads((state / "manifest.json").read_text())
        proposal = next(item for item in manifest["proposed"] if item["path"] == str(installer.MIME))
        self.assertEqual(proposal["mode"], 0o664)
        self.assertEqual(Path(proposal["staged"]).read_bytes(), installer.MIME.read_bytes())
        installer.apply(state)
        self.assertEqual(installer.MIME.stat().st_mode & 0o777, 0o664)
        proposal["mode"] = 0o666
        installer.save_manifest(state, manifest)
        with self.assertRaisesRegex(RuntimeError, "Unexpected prepared contents"):
            installer.load_state(state)


if __name__ == "__main__":
    unittest.main(verbosity=2)
