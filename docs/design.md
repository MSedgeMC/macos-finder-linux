# Design and platform scope

Finder for Linux is a native Qt 6/KDE Frameworks file manager inspired by
Finder's organization and Apple Liquid Glass. It is an approximation on Linux.
It does not contain Apple's renderer, reproduce macOS services, or establish
literal pixel parity with any macOS release.

## Window and views

The client uses a unified 52 logical pixel toolbar and an initially 256 pixel,
resizable sidebar. Back/forward, the folder title, four view controls, sorting,
tags, actions and search share that toolbar. At narrow widths the view controls
collapse into a popup. The file pane stays opaque; glass is reserved for the
header and sidebar. A continuous outer contour is approximated locally.

The sidebar and content adjoin without a reserved splitter strip. The native
resize handle overlaps the panes, retaining its grab area without exposing the
desktop between them. The content stack has an opaque base across both folder
and search pages. Navigation and four-view capsules measure 73×36 and 142×36
logical pixels respectively, matching the audited generic window-kit targets.

Icons, List, Columns and Gallery are separate native views. List exposes real
sortable metadata columns and expandable directories. Columns retains a real
folder hierarchy with keyboard navigation. Gallery combines a large preview,
a right information panel and a horizontal filmstrip. Icon and Gallery cells
use cached native file thumbnails, a neutral rounded icon selection and a blue
filename selection. A two-line filename and the inline rename editor occupy
the area below each icon.

![Icons with native filename selection](../screenshots/icons.png)

![Gallery with a right information panel](../screenshots/gallery.png)

These images are captures of this Linux implementation using disposable
fixtures, not Apple reference images. They are cropped below the content to
remove local filesystem breadcrumbs and status information.

The originating desktop's deliberate preferences are Ubuntu Medium (500),
slight hinting and RGB smoothing, a Windows-style cursor, and right-side
minimize/maximize/close controls with visible symbols. The application preserves
the desktop font and cursor rather than installing Apple assets. These choices
are intentional differences from macOS typography and traffic-light controls.

## Material and native behavior

The header and sidebar request region-limited KWin blur of the actual compositor
backdrop. They retain a readable tinted fallback when blur is unavailable.
Wallpaper is not substituted for windows behind the application. The coating
and directional edges approximate a glass appearance; genuine backdrop
refraction and Apple's proprietary material renderer are not implemented.
Foreground text, icons and file content remain crisp.

KDirModel, KIO, KFilePlacesModel and KFilePlacesView retain native filesystem
watching, bookmarks, storage locations, context menus, keyboard navigation,
thumbnails, transfers, conflict handling, Trash and Undo. Material rendering is
separate from these actions. Tabs and history keep independent folder state;
folder settings retain view, sorting, icon size and List column visibility.

Quick Look uses bounded, cached native image and PDF rendering. PDF pages are
composited on white paper, text previews are read-only and limited to 512 KiB
with horizontal scrolling, and audio/video begins only after Play. Unsupported
or remote formats offer metadata and their normal default-application route.

Recursive filename search runs on a worker and ignores obsolete requests after
cancellation. Its defaults are 2,000 results, 250,000 entries and 15 seconds;
the UI reports a reached limit. Hidden entries are optional. Symlink recursion,
virtual mounts and remote search roots are excluded. Search actions operate on
the visible result selection. Search is not semantic or full-content indexing.

Colored and custom tags use KDE-compatible Linux `user.xdg.tags` metadata.
Changes preserve custom tags and unrelated metadata. Metadata writes are
serialized; pending reads disable stale tag menu state. Unsupported storage,
symlinks and permission failures produce errors rather than a hidden tag
database.

The user-level installer scopes changes to the file-manager routes, desktop
entries, FileManager1 activation and folder MIME preference. It retains a
Dolphin fallback launcher and journals backups/restore. It does not replace a
manager with open windows or restart Plasma, KWin or Spotlight.

## References and limitations

Geometry and hierarchy were reviewed against Apple's public
[Design Resources](https://developer.apple.com/design/resources/) and the
[Finder views](https://support.apple.com/guide/mac-help/change-folders-displayed-finder-mac-mchldaafb302/mac),
[toolbar](https://support.apple.com/guide/mac-help/customize-the-finder-toolbar-on-mac-mchlp3011/mac),
and [Quick Look](https://support.apple.com/guide/mac-help/preview-a-file-mh14119/mac)
guides. Generic window-kit dimensions do not establish every Finder default.
The macOS 27 design resources were reviewed during development in November
2026; proprietary kit files are not distributed in this repository.

Linux material design also consulted
[Liquid Glass KDE Widgets](https://github.com/jaxparrow07/liquidglass-kde-widgets)
and [liquid_glass_widgets](https://github.com/sdegenaar/liquid_glass_widgets).
Their platform shaders and rendering APIs are references, not compatible Qt
assets or evidence of equality with current macOS.

iCloud, AirDrop, macOS sharing services, Finder extensions, complete Quick
Actions, full grouping behavior and all Finder preview formats are not
implemented. Search results currently use a metadata List. Native remote
directory support depends on installed KIO workers, and blur depends on KWin.
No lossless comparison with identical files, geometry, scale, typography and
backdrop has established pixel parity.

A component comparison on 2026-11-15 confirmed the 52-pixel toolbar,
256-pixel initial sidebar, content origin at (256, 52), the capsule dimensions
above, 32-pixel medium sidebar rows, and `#1e1e1e` opaque dark content. These
matches concern generic kit primitives. Section headers retain native KDE hit
geometry (31 pixels with the tested font versus the kit's 18-pixel allocation),
and the documented typography, controls, material and platform differences
remain. This comparison does not establish whole-window 1:1 Finder equality.

This repository does not bundle Apple fonts, Apple kit images or proprietary
icon assets. Qt/KDE dependencies and any separately installed icon theme retain
their own licenses and authorship. The screenshots show an existing desktop
icon theme; the theme itself is not included.
