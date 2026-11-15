# macos-finder-linux

A native Qt 6/KDE file manager inspired by Finder, with Icons, List, real Miller
Columns, Gallery, multi-file Quick Look, local tags, saved Boolean searches,
and native KIO file operations.

![Icon view](screenshots/icons.png)

This is a Linux approximation inspired by Apple Liquid Glass. It does not
contain Apple's Finder or renderer, and exact macOS pixel parity is not
established. The file pane is opaque; suitable chrome requests genuine KWin
compositor blur. The app now defaults to left traffic-light controls, 64px icons and an
already-installed SF Pro Display font, with Inter/sans-serif fallback. It does
not install fonts or change the desktop cursor.

Implemented features include tabs/history, inline rename, native conflict
handling, copy/move, Trash/Undo, file watching, device bookmarks/eject, saved
folder views, all six grouping choices, optional side previews, Gallery
Information/Show More, editable tag chips, image/PDF/text previews, and
explicit-play audio/video. Native ZIP creation and extraction use conflict handling,
cancellation and Undo. Icons support persistent manual positions, Snap to Grid,
Clean Up and spring-loaded folders. PDF previews support search, selection and
multipage scrolling; media can export a trimmed copy through installed FFmpeg.
Quick Actions provide image conversion/markup and image or rasterized PDF combination.

Settings include a persistent tag registry and favorite slots, light/dark/system
appearance, increased contrast and reduced transparency. Inspector follows the
selection or pins its real metadata. Closed windows and explicitly saved sessions
can restore their tabs and views. Linux `user.xdg.tags` preserves custom tags.
Bounded local search supports names, dates, sizes, tags and Unicode text with
nested Any/All/None criteria. Optional Baloo queries use an existing index and
report its coverage; the app does not start an indexer.

![Gallery view](screenshots/gallery.png)

## Build and run

Requires Linux, Qt 6 and KDE Frameworks 6 development packages. See
[dependencies and build instructions](docs/build.md).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/finder
```

The native implementation passed **32 isolated Wayland integration checks**.
The sidebar also passed 22 boundary/geometry captures at both 1× and 1.5× scale, including
actual tag activation and native resize input. A compositor capture measured
real backdrop blur while the file pane stayed unchanged.
See [validation](docs/validation.md) and [design and platform limits](docs/design.md).

## Optional KDE desktop integration

On the originating Kubuntu/Plasma installation, the scoped installer replaces
existing Dolphin launch routes and the folder default, preserving an original
Dolphin launcher and backups. It expects KDE's `plasma-dolphin.service` and an
existing Dolphin desktop entry. Read [tools/install.py](tools/install.py) before
applying this desktop-specific integration.

```sh
python3 tools/test-install.py
python3 tools/install.py --prepare
python3 tools/install.py --apply /path/printed/by/prepare
python3 tools/install.py --verify
```

Existing file-manager windows must be closed for replacement. The prepared
manifest lists exact writes. Restore with `--restore /path/to/backup`; the last
successful backup path is recorded in `last-backup.txt`. The installer restarts
only the file-manager daemon. It does not restart Plasma, KWin or Spotlight.
CMake's ordinary install installs only the binary.

Space opens Quick Look; Return/F2 renames. Ctrl+1..4 switches views, Ctrl+T opens
a tab, Ctrl+F searches, Ctrl+L opens Go to Folder, and Ctrl+J opens View Options.
Meta equivalents are provided for common Finder shortcuts.

## License

Original application code is MIT licensed. Qt, KDE Frameworks and external icon
themes retain their own licenses. No Apple font, UI kit or proprietary image
bundle is included.
