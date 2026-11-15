# Building on Linux

The portable build uses CMake 3.22 or newer, a C++20 compiler, Qt 6.8 or newer,
and KDE Frameworks 6.2 or newer. The tested development environment uses Qt
6.10.2 and KDE Frameworks 6.24.0. Linux is required for mount metadata and native
`user.xdg.tags` support.

## Dependencies

Install your distribution's development packages for Qt Core, Gui, Widgets,
D-Bus, Network, XML, PDF, PrintSupport and Multimedia/MultimediaWidgets; KDE KIO,
CoreAddons, ItemViews, Bookmarks, WidgetsAddons, JobWidgets, Config,
WindowSystem, I18n, Service, Solid, FileMetaData and Archive; and their dependencies.

On a recent Ubuntu/Kubuntu release with Qt 6 and KDE Frameworks 6 available:

```sh
sudo apt install build-essential cmake ninja-build gettext \
    qt6-base-dev qt6-base-dev-tools qt6-pdf-dev qt6-multimedia-dev \
    libkf6kio-dev libkf6coreaddons-dev libkf6itemviews-dev \
    libkf6bookmarks-dev libkf6widgetsaddons-dev libkf6jobwidgets-dev \
    libkf6config-dev libkf6windowsystem-dev libkf6i18n-dev \
    libkf6service-dev libkf6solid-dev libkf6filemetadata-dev libkf6archive-dev
```

Package names and available versions vary by distribution. The packages'
CMake configurations supply required transitive dependencies. The build links
the exported `Qt6::` and `KF6::` targets, including `KF6::FileMetaData`; it does
not hardcode include paths, Qt tool paths, or library SONAMEs.

## Configure, build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For a compiler without Ninja installed, omit `-G Ninja`. CMake automatically
generates all Qt meta-object sources, including the `main.moc` and `search.moc`
included by the implementation. Run `build/finder` after the build to open the
file manager.

`BUILD_TESTING` is on by default. CTest exercises search, native tags, scrolling,
preview and editing outputs, ZIP operations, Smart Folders, browser metadata and
arrangement, search editing/results in four modes, Inspector and appearance. Every executable uses
temporary fixtures, private XDG directories and a private session bus. Widget
tests use Qt offscreen and never connect to the working display. The
tag fixture needs a local writable filesystem supporting `user.*` extended
attributes. Use `-DBUILD_TESTING=OFF` when packaging without unit executables.

The CTest suite does not visually validate the application. GUI rendering and
input tests must use a separate headless Wayland compositor, private session
D-Bus/runtime/config/data directories, and screenshots saved to files. Never
send automated input to the working desktop or restart its compositor for a
test.

## Installation

To install the built executable into a chosen prefix:

```sh
cmake --install build --prefix "$HOME/.local"
```

This CMake installation installs the binary only. It does not change the
default file manager, MIME associations, session services, font, cursor or
desktop theme. The repository's separate scoped installer handles desktop
integration and backups; review that workflow before applying it. An icon
theme is supplied by the desktop and is not downloaded by the build.

## A separate dependency prefix

If you maintain compatible Qt/KDE development packages in another prefix,
point CMake at those prefixes without changing the source files:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/opt/qt6;/opt/kde6"
```

The prefix must contain the matching headers, CMake package configurations,
imported libraries and host tools such as `moc`. A directory containing only
headers extracted from development packages is not a complete prefix: imported
targets may also refer to runtime libraries and helper executables. Use a
consistent Qt version for all modules and tools. No machine-specific prefix is
stored in this repository.

Optional media trim needs installed `ffmpeg`, `ffprobe` and compatible encoders.
Indexed content search needs `baloosearch6` and an existing readable Baloo 6 index.
These providers are detected at runtime; the app starts neither an indexer nor
a background whole-disk scan.
