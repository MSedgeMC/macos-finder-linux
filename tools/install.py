#!/usr/bin/env python3
"""Prepare, install or restore the scoped Finder file-manager integration."""
import argparse
import configparser
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
HOME = Path.home()
DATA = HOME / ".local/share"
CONFIG = HOME / ".config"
STATE = HOME / ".local/state/kubuntu-customization"
SOURCE = ROOT / "build/finder"
EXECUTABLE = DATA / "finder-v2/bin/finder"
UNIT = "plasma-dolphin.service"
MIME = CONFIG / "mimeapps.list"
DESKTOP = "org.local.finder.desktop"


def run(*command):
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save_manifest(state, manifest):
    atomic_write(state / "manifest.json", (json.dumps(manifest, indent=2) + "\n").encode(), 0o600)


def atomic_write(path, contents, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix="." + path.name + ".finder-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(contents)
            output.flush()
            os.fchmod(output.fileno(), mode)
            os.fsync(output.fileno())
        temporary = Path(name)
        temporary.replace(path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        Path(name).unlink(missing_ok=True)


def mime_contents():
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.optionxform = str
    original = MIME.read_text() if MIME.exists() else ""
    if original:
        config.read_string(original)
    # Preserve comments, browser associations and every other line verbatim.
    lines = original.splitlines(keepends=True)
    output, section, found_section, found_key = [], "", False, False
    entry = "inode/directory=" + DESKTOP + ";\n"
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if section == "Default Applications" and not found_key:
                if output and not output[-1].endswith("\n"):
                    output[-1] += "\n"
                output.append(entry)
                found_key = True
            section = stripped[1:-1]
            if section == "Default Applications":
                if found_section:
                    raise RuntimeError("Duplicate Default Applications sections require review")
                found_section = True
        if section == "Default Applications" and stripped.partition("=")[0].strip() == "inode/directory":
            if not found_key:
                output.append(entry)
                found_key = True
            continue
        output.append(line)
    if output and not output[-1].endswith("\n"):
        output[-1] += "\n"
    if not found_section:
        output.append("[Default Applications]\n")
    if not found_key:
        output.append(entry)
    return "".join(output).encode()


def desktop_entry():
    # Keep MIME/category integration from Dolphin, but remove its application
    # activation and translated name/Exec entries so they cannot bypass Finder.
    original = DATA / "applications/org.kde.dolphin.desktop"
    if not original.is_file():
        original = Path("/usr/share/applications/org.kde.dolphin.desktop")
    lines = original.read_text().splitlines()
    output = []
    section = ""
    for line in lines:
        if line.startswith("["):
            section = line
        if section == "[Desktop Entry]" and (line.startswith("Name=") or line.startswith("Name[")
                or line.startswith("Exec=") or line.startswith("TryExec=")
                or line.startswith("DBusActivatable=") or line.startswith("Actions=")
                or line.startswith("X-DBUS-ServiceName=") or line.startswith("StartupWMClass=")
                or line.startswith("X-DocPath=")):
            continue
        if section.startswith("[Desktop Action"):
            continue
        output.append(line)
    # Put the explicit entry fields in the first section, before any later one.
    header = ["Name=Finder", "Exec=" + str(HOME / ".local/bin/finder") + " %U",
              "TryExec=" + str(HOME / ".local/bin/finder"), "DBusActivatable=false", "StartupWMClass=Finder"]
    index = output.index("[Desktop Entry]") + 1
    output[index:index] = header
    return ("\n".join(output).rstrip() + "\n").encode()


def deployment_files():
    if not SOURCE.is_file():
        raise FileNotFoundError("Build and verify Finder first: " + str(SOURCE))
    finder_wrapper = ('#!/bin/sh\nexec "' + str(EXECUTABLE) + '" "$@"\n').encode()
    dolphin_wrapper = ('#!/bin/sh\nexec "' + str(HOME / '.local/bin/finder') + '" "$@"\n').encode()
    desktop = desktop_entry()
    service = ("[Service]\nExecStart=\nExecStart=" + str(HOME / ".local/bin/finder") + " --daemon\n").encode()
    dbus_service = ("[D-BUS Service]\nName=org.freedesktop.FileManager1\nExec="
                    + str(HOME / ".local/bin/finder") + " --daemon\nSystemdService=" + UNIT + "\n").encode()
    fallback = ("[Desktop Entry]\nType=Application\nName=Dolphin (original)\n"
                "GenericName=File Manager\nIcon=system-file-manager\nExec=/usr/bin/dolphin %U\n"
                "Terminal=false\nCategories=System;FileTools;FileManager;\n").encode()
    return {
        EXECUTABLE: (SOURCE.read_bytes(), 0o755),
        HOME / ".local/bin/finder": (finder_wrapper, 0o755),
        HOME / ".local/bin/dolphin": (dolphin_wrapper, 0o755),
        DATA / "applications/org.local.finder.desktop": (desktop, 0o644),
        DATA / "applications/org.kde.dolphin.desktop": (desktop, 0o644),
        DATA / "applications/org.local.dolphin-original.desktop": (fallback, 0o644),
        CONFIG / "systemd/user/plasma-dolphin.service.d/60-finder-native.conf": (service, 0o644),
        DATA / "dbus-1/services/org.kde.dolphin.FileManager1.service": (dbus_service, 0o644),
        MIME: (mime_contents(), 0o644),
    }


def deployment_paths():
    """The complete allowed scope, independent of mutable source files."""
    return {
        EXECUTABLE: 0o755,
        HOME / ".local/bin/finder": 0o755,
        HOME / ".local/bin/dolphin": 0o755,
        DATA / "applications/org.local.finder.desktop": 0o644,
        DATA / "applications/org.kde.dolphin.desktop": 0o644,
        DATA / "applications/org.local.dolphin-original.desktop": 0o644,
        CONFIG / "systemd/user/plasma-dolphin.service.d/60-finder-native.conf": 0o644,
        DATA / "dbus-1/services/org.kde.dolphin.FileManager1.service": 0o644,
        MIME: 0o644,
    }


def checked_path(path, base):
    path, base = Path(path), Path(base)
    if not path.is_absolute() or not path.is_relative_to(base) or ".." in path.parts:
        raise RuntimeError("Unexpected path: " + str(path))
    for candidate in (path, *path.parents):
        if candidate.is_symlink():
            raise RuntimeError("Refusing symlink path: " + str(candidate))
        if candidate == base:
            break
    return path


def path_state(path):
    checked_path(path, HOME)
    if path.exists() and not path.is_file():
        raise RuntimeError("Expected regular file: " + str(path))
    return {"path": str(path), "existed": path.exists(),
            "hash": digest(path) if path.exists() else None,
            "mode": path.stat().st_mode & 0o777 if path.exists() else None}


def service_state():
    result = run("systemctl", "--user", "show", UNIT, "-p", "ActiveState", "-p", "SubState", "-p", "MainPID")
    return dict(line.split("=", 1) for line in result.splitlines() if "=" in line)


def restart_safety():
    state = service_state()
    # Never interrupt visible file-manager windows. A running daemon must offer
    # the explicit read-only window count endpoint before it can be restarted.
    processes = []
    for process in Path("/proc").iterdir():
        if not process.name.isdigit():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            command = (process / "cmdline").read_bytes().split(b"\0")
            executable = Path(command[0].decode()).name if command and command[0] else ""
            if executable in {"dolphin", "finder"}:
                processes.append((int(process.name), [c.decode(errors="replace") for c in command if c]))
        except (OSError, ValueError):
            continue
    if any("--daemon" not in command for _, command in processes):
        raise RuntimeError("A file-manager window is running; close it before switching the file-manager service")
    if state.get("ActiveState") == "active" or processes:
        import dbus
        try:
            bus = dbus.SessionBus()
            if not bus.name_has_owner("org.local.Finder"):
                raise RuntimeError("Running file-manager daemon cannot report its window count; service was not interrupted")
            # Resolve the existing unique owner to avoid D-Bus activation during
            # this read-only check if the service exits while it is queried.
            proxy = bus.get_object(bus.get_name_owner("org.local.Finder"), "/Finder")
            count = int(proxy.windowCount(dbus_interface="org.local.Finder"))
        except dbus.DBusException as error:
            raise RuntimeError("Running file-manager daemon cannot report its window count; service was not interrupted") from error
        if count:
            raise RuntimeError("Finder still has open windows; service was not interrupted")
    return state


def validate_snapshot(items):
    for item in items:
        actual = path_state(Path(item["path"]))
        if any(actual[key] != item[key] for key in ("existed", "hash", "mode")):
            raise RuntimeError("File changed since preparation: " + item["path"])


def validate_backups(state, manifest):
    for item in manifest["files"]:
        if not item["existed"]:
            continue
        backup = checked_path(item["backup"], state)
        if not backup.is_file() or digest(backup) != item["hash"] or backup.stat().st_mode & 0o777 != item["mode"]:
            raise RuntimeError("Backup changed: " + str(backup))


def prepare():
    os.umask(0o077)
    files = deployment_files()
    stamp = datetime.datetime.now().strftime("finder-v2-%Y%m%d-%H%M%S-%f")
    state = STATE / stamp
    checked_path(state, HOME)
    snapshots = [path_state(path) for path in files]
    initial_service = restart_safety()
    # Preserve the lower-priority MIME file as evidence without changing it.
    extra_mime = path_state(DATA / "applications/mimeapps.list")
    state.mkdir(parents=True)
    for item in snapshots + [extra_mime]:
        if item["existed"]:
            backup = state / "before" / Path(item["path"]).relative_to(HOME)
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(item["path"], backup)
            item["backup"] = str(backup)
    # Stage concrete contents for review and keep apply independent of sources
    # changing while a long GUI verification is in progress.
    proposed = []
    for path, (content, mode) in files.items():
        staged = state / "proposed" / path.relative_to(HOME)
        staged.parent.mkdir(parents=True, exist_ok=True)
        staged.write_bytes(content)
        staged.chmod(mode)
        proposed.append({"path": str(path), "staged": str(staged), "hash": digest(staged), "mode": mode})
    manifest = {"schema": 2, "state": "prepared", "created_at": datetime.datetime.now().astimezone().isoformat(),
                "files": snapshots, "proposed": proposed, "extra_mime": extra_mime,
                "mime_default_before": run("xdg-mime", "query", "default", "inode/directory"),
                "service_before": initial_service,
                "source_hash": next(item["hash"] for item in proposed if item["path"] == str(EXECUTABLE)),
                "write_intents": [], "installed": []}
    save_manifest(state, manifest)
    (state / "ROLLBACK.md").write_text("Restore the previous file-manager integration:\n\n```sh\npython3 "
        + str(Path(__file__).resolve()) + " --restore " + str(state) + "\n```\n")
    print(state)
    return state


def load_state(state):
    state = checked_path(Path(state).absolute(), STATE)
    if state.parent != STATE or not state.name.startswith("finder-v2-"):
        raise RuntimeError("Expected this installer's private backup directory")
    checked_path(state / "manifest.json", state)
    manifest = json.loads((state / "manifest.json").read_text())
    if manifest.get("schema") != 2:
        raise RuntimeError("Unsupported backup manifest schema")
    allowed = {str(path): mode for path, mode in deployment_paths().items()}
    for key in ("files", "proposed"):
        paths = [item["path"] for item in manifest[key]]
        if len(paths) != len(set(paths)) or set(paths) != set(allowed):
            raise RuntimeError("Unexpected deployment scope in " + key)
    for item in manifest["files"]:
        path = checked_path(item["path"], HOME)
        if item["existed"]:
            expected = state / "before" / path.relative_to(HOME)
            if Path(item["backup"]) != expected:
                raise RuntimeError("Unexpected backup location")
            checked_path(expected, state)
    for item in manifest["proposed"]:
        expected = state / "proposed" / Path(item["path"]).relative_to(HOME)
        if Path(item["staged"]) != expected or item["mode"] != allowed[item["path"]]:
            raise RuntimeError("Unexpected prepared contents location or mode")
        checked_path(expected, state)
    proposals = {item["path"]: item for item in manifest["proposed"]}
    for key in ("write_intents", "installed"):
        seen = set()
        for item in manifest.get(key, []):
            if item["path"] in seen or item["path"] not in proposals:
                raise RuntimeError("Unexpected deployment journal entry")
            expected = proposals[item["path"]]
            if any(item[field] != expected[field] for field in ("hash", "mode")):
                raise RuntimeError("Deployment journal does not match prepared contents")
            seen.add(item["path"])
    if manifest["extra_mime"]["path"] != str(DATA / "applications/mimeapps.list"):
        raise RuntimeError("Unexpected MIME evidence location")
    if manifest["source_hash"] != proposals[str(EXECUTABLE)]["hash"]:
        raise RuntimeError("Prepared binary does not match its source record")
    return state, manifest


def refresh_and_start():
    run("kbuildsycoca6", "--noincremental")
    run("systemctl", "--user", "daemon-reload")
    # Only this file-manager daemon is restarted. KWin, Plasma and Spotlight
    # keep their existing processes and settings.
    restart_safety()
    run("systemctl", "--user", "restart", UNIT)


def apply(state):
    state, manifest = load_state(state)
    if manifest["state"] != "prepared":
        raise RuntimeError("This preparation was already applied; inspect its manifest instead of repeating it")
    validate_snapshot(manifest["files"])
    validate_snapshot([manifest["extra_mime"]])
    if run("xdg-mime", "query", "default", "inode/directory") != manifest["mime_default_before"]:
        raise RuntimeError("Default file manager changed since preparation")
    restart_safety()
    for item in manifest["proposed"]:
        staged = Path(item["staged"])
        if not staged.is_file() or digest(staged) != item["hash"] or staged.stat().st_mode & 0o777 != item["mode"]:
            raise RuntimeError("Prepared contents changed: " + str(staged))
    validate_backups(state, manifest)
    manifest["state"] = "installing"
    save_manifest(state, manifest)
    try:
        for item in manifest["proposed"]:
            # Persist intent before replacement, so a killed process remains
            # recoverable even between the replacement and completion record.
            record = {key: item[key] for key in ("path", "hash", "mode")}
            manifest["write_intents"].append(record)
            save_manifest(state, manifest)
            atomic_write(Path(item["path"]), Path(item["staged"]).read_bytes(), item["mode"])
            manifest["installed"].append(record)
            save_manifest(state, manifest)
        manifest["state"] = "files_installed"
        save_manifest(state, manifest)
        refresh_and_start()
        report = verify(state)
        manifest.update(state="applied", applied_at=datetime.datetime.now().astimezone().isoformat(), verification=report)
        save_manifest(state, manifest)
        (ROOT / "last-backup.txt").write_text(str(state) + "\n")
    except Exception as error:
        manifest.update(state="failed", error=str(error))
        save_manifest(state, manifest)
        raise RuntimeError("Finder deployment needs scoped recovery: " + str(state / "manifest.json")) from error
    print(json.dumps({"applied": True, "backup": str(state), "verification": report}, indent=2))


def verify(state=None):
    if state is None:
        state = Path((ROOT / "last-backup.txt").read_text().strip())
    state, manifest = load_state(Path(state))
    for item in manifest["proposed"]:
        path = Path(item["path"])
        if not path.is_file() or digest(path) != item["hash"] or path.stat().st_mode & 0o777 != item["mode"]:
            raise RuntimeError("Installed file mismatch: " + str(path))
    default = run("xdg-mime", "query", "default", "inode/directory")
    if default != DESKTOP:
        raise RuntimeError("Directory MIME handler is not Finder: " + default)
    current = service_state()
    if current.get("ActiveState") != "active":
        raise RuntimeError("Finder file-manager service is not active")
    command = run("systemctl", "--user", "show", UNIT, "-p", "ExecStart", "--value")
    if str(HOME / ".local/bin/finder") not in command:
        raise RuntimeError("File-manager service is running a different executable")
    import dbus
    bus = dbus.SessionBus()
    deadline = time.monotonic() + 5
    while True:
        try:
            if not bus.name_has_owner("org.local.Finder") or not bus.name_has_owner("org.freedesktop.FileManager1"):
                raise RuntimeError("Native Finder bus names did not finish starting")
            owner = bus.get_name_owner("org.local.Finder")
            if owner != bus.get_name_owner("org.freedesktop.FileManager1"):
                raise RuntimeError("Another file manager owns the desktop file-manager interface")
            xml = str(bus.get_object(owner, "/org/freedesktop/FileManager1").Introspect(
                dbus_interface="org.freedesktop.DBus.Introspectable"))
            methods = ET.fromstring(xml).find("./interface[@name='org.freedesktop.FileManager1']")
            names = {method.get("name") for method in methods.findall("method")} if methods is not None else set()
            if not {"ShowFolders", "ShowItems", "ShowItemProperties"}.issubset(names):
                raise RuntimeError("Native file-manager methods did not finish starting")
            finder = bus.get_object(owner, "/Finder")
            window_count = int(finder.windowCount(dbus_interface="org.local.Finder"))
            if bus.name_has_owner("org.local.FinderTest"):
                raise RuntimeError("Test-only Finder interface is exposed in production")
            break
        except (dbus.DBusException, RuntimeError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)
    return {"passed": True, "files_match_staged": True, "directory_default": default,
            "service": current, "filemanager1_methods": True, "window_count": window_count,
            "source_hash": manifest["source_hash"]}


def restore(state):
    state, manifest = load_state(state)
    if manifest["state"] not in {"installing", "files_installed", "failed", "applied", "restoring"}:
        raise RuntimeError("No applied or partial installation to restore")
    installed = {item["path"]: item for item in manifest.get("write_intents", []) + manifest.get("installed", [])}
    validate_backups(state, manifest)
    for item in manifest["files"]:
        if item["path"] not in installed:
            continue
        actual = path_state(Path(item["path"]))
        original_matches = all(actual[key] == item[key] for key in ("existed", "hash", "mode"))
        installed_matches = actual["existed"] and all(actual[key] == installed[item["path"]][key] for key in ("hash", "mode"))
        if not original_matches and not installed_matches:
            raise RuntimeError("Newer installed edits were preserved: " + item["path"])
    restart_safety()
    manifest["state"] = "restoring"
    save_manifest(state, manifest)
    try:
        run("systemctl", "--user", "stop", UNIT)
        for item in manifest["files"]:
            if item["path"] not in installed:
                continue
            path = Path(item["path"])
            if item["existed"]:
                atomic_write(path, Path(item["backup"]).read_bytes(), item["mode"])
            else:
                path.unlink(missing_ok=True)
        validate_snapshot([item for item in manifest["files"] if item["path"] in installed])
        run("kbuildsycoca6", "--noincremental")
        run("systemctl", "--user", "daemon-reload")
        if manifest["service_before"].get("ActiveState") == "active":
            run("systemctl", "--user", "start", UNIT)
        default = run("xdg-mime", "query", "default", "inode/directory")
        if default != manifest["mime_default_before"]:
            raise RuntimeError("Previous MIME default did not restore: " + default)
        manifest.update(state="restored", restored_at=datetime.datetime.now().astimezone().isoformat())
        save_manifest(state, manifest)
    except Exception as error:
        manifest["restore_error"] = str(error)
        save_manifest(state, manifest)
        raise
    print(json.dumps({"restored": True, "directory_default": default, "backup": str(state)}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    options = parser.add_mutually_exclusive_group(required=True)
    options.add_argument("--prepare", action="store_true")
    options.add_argument("--apply", type=Path)
    options.add_argument("--verify", nargs="?", const="last")
    options.add_argument("--restore", type=Path)
    args = parser.parse_args()
    if args.prepare:
        prepare()
    elif args.apply:
        apply(args.apply)
    elif args.restore:
        restore(args.restore)
    else:
        print(json.dumps(verify(None if args.verify == "last" else Path(args.verify)), indent=2))


if __name__ == "__main__":
    main()
