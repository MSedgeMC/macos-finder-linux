#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run a native unit executable without access to the working GUI/session."""
import os
import subprocess
import sys
import tempfile
import signal
import shutil
from pathlib import Path
with tempfile.TemporaryDirectory(prefix="finder-headless-", ignore_cleanup_errors=True) as temporary:
    root = Path(temporary)
    env = os.environ.copy()
    for key in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "SESSION_MANAGER"):
        env.pop(key, None)
    for key, name in (("XDG_RUNTIME_DIR", "runtime"), ("XDG_CONFIG_HOME", "config"),
                      ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state")):
        directory = root / name
        directory.mkdir(mode=0o700)
        env[key] = str(directory)
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["XDG_CONFIG_DIRS"] = str(root / "config")
    env["XDG_DATA_DIRS"] = "/usr/local/share:/usr/share"
    # Session activation helpers can outlive the executable. Exit with the
    # test result, then terminate only this private session's process group.
    process = subprocess.Popen(["dbus-run-session", "--", *sys.argv[1:]], env=env, start_new_session=True)
    try:
        result = process.wait()
    finally:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        # A private document portal can leave a disconnected FUSE mount after
        # its bus exits. Unmount only this test's runtime/doc before cleanup.
        document_mount = root / "runtime" / "doc"
        if document_mount.exists() or document_mount.is_mount():
            fusermount = shutil.which("fusermount3") or shutil.which("fusermount")
            if fusermount:
                subprocess.run([fusermount, "-u", "-z", str(document_mount)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    raise SystemExit(result)
