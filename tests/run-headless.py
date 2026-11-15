#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run a native unit executable without access to the working GUI/session."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path
with tempfile.TemporaryDirectory(prefix="finder-headless-") as temporary:
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
    raise SystemExit(subprocess.call(sys.argv[1:], env=env))
