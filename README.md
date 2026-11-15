# macos-finder-linux

A native Qt 6/KDE file manager inspired by Finder, with Icons, List, real Miller
Columns, Gallery, multi-file Quick Look, local tags, recursive filename search,
and native KIO file operations.

This is a Linux approximation inspired by Apple Liquid Glass. It does not
contain Apple's Finder or renderer, and exact macOS pixel parity is not
established. The file pane is opaque; suitable chrome requests genuine KWin
compositor blur. Ubuntu Medium and visible right-side window controls are the
current default preferences.

Implemented features include tabs/history, inline rename, native conflict
handling, copy/move, Trash/Undo, file watching, device bookmarks/eject, saved
folder views, image/PDF/text previews, and explicit-play audio/video. Linux
`user.xdg.tags` preserves custom tags. Local filename search runs on a bounded,
cancelable worker without background indexing.

Portable CMake build and validation details are being added in the next
commits. The implementation has passed 32 isolated Wayland integration checks,
with temporary fixtures and no automated input on the working desktop.

## License

Original application code is MIT licensed. Qt, KDE Frameworks and external icon
themes retain their own licenses. No Apple font, UI kit or proprietary image
bundle is included.
