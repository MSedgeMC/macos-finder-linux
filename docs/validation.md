# Validation

The originating Kubuntu desktop build passed the following checks on
2026-11-15. These results establish tested Linux behavior, not macOS platform or
pixel parity. They preceded extraction into this standalone repository; a
different dependency set or desktop should run the available checks again.

## Isolated native integration

All **32 integration checks passed** on a private native Wayland screen with
its own compositor, socket, session bus, runtime and writable XDG directories.
Applications and test input never connected to the working desktop. Disposable
fixtures were placed on the home filesystem so native KIO Trash could be
tested; settings, clipboard and Trash belonged to the private session.

The suite covered:

- Four distinct views preserving real multi-file selection, three-level native
  Columns navigation, independent tabs/history and minimum-width layout.
- Space opening and closing Quick Look, decoded images at useful display size,
  read-only text, multiple-file navigation, and two native PDF pages. Screenshot
  pixels verified actual page content changed, not only the page counter.
- Gallery's large image, right information panel and native filmstrip.
- Recursive nested filename search, Escape restoring the folder, and search
  result Trash/Undo acting on the result while preserving an unrelated browser
  selection. A Space in the location editor remained text.
- Unavailable-folder navigation preserving the current folder, filesystem
  additions/removals, and toolbar tag add/remove preserving an existing custom
  `user.xdg.tags` value.
- View Options saving and restoring view, icon size, sort direction and hidden
  List columns when leaving and returning to a folder.
- Native copy/paste, inline rename with spaces and Undo, new folder and Undo,
  cut/paste and Undo, Trash and Undo, destination-conflict cancellation, and
  duplicates choosing unused names. Original fixture contents retained their
  hashes. The harness allowed each transfer to finish before requesting Undo.
- Standard FileManager1 ShowItems/ShowFolders and a second CLI invocation
  forwarding to the running manager with correct reveal selection.

Each static image and Gallery preview consumed **0.0% process CPU during a
one-second idle sample** on the test machine. This only checks for a sustained
idle refresh loop; it is not a GPU benchmark or a cross-machine performance
guarantee. Screenshots were inspected as files. Fixture directories, test
processes and the private display runtime were removed after completion.

## Focused checks

These are independent checks; their totals overlap with integration coverage
and should not be described as a single count of unique behaviors.

| Component | Passing checks | Coverage |
| --- | ---: | --- |
| Icon/Gallery delegate | 16 | Native listing/selection, rounded icon and two-line filename selection, thumbnail pixels, inline rename placement, real rename/Undo, List delegate retained |
| Browser | 28 | Separate selection models, nested columns/arrow navigation, one open per double click, view transitions, rename/Undo, invalid-name rejection and conflict cancellation |
| Places sidebar | 7 | Native model, compact row/header hit geometry, fixed icon size, accessibility and actual row activation |
| Recursive search | 11 | Hidden behavior, result/entry/time bounds, canceled generations, replacement/destruction and UI responsiveness |
| Linux tags | 9 | Existing metadata, custom/Unicode tags, unrelated xattrs, invalid inputs, idempotent removal and storage/path errors |
| Installer | 13 | Offline prepare/apply/restore recovery cases using fake external commands and a fake session bus; no production writes in these cases |

The 11 search checks also passed under sanitizers. The private global-menu
registrar check confirmed native File/Edit/View/Go/Window menu export through
`com.canonical.dbusmenu`, including navigation, view modes and file actions.

The scoped production installation was subsequently verified on the originating
desktop: Finder was the folder default, the windowless manager daemon owned
both application and FileManager1 service names, and the private input/render
test interface was absent. This is a deployment observation on that machine,
not a claim that installation changes are exercised against every distribution.

## Standalone portable build

The exported Qt/KDE-target CMake configuration and complete Release build of
this standalone repository passed on 2026-11-15. Both CTest programs passed:
11 recursive-search cases and 9 tag cases. A binary-only staged installation
contained exactly `bin/finder`. Matching development dependencies were supplied
through a private extracted prefix; these checks installed no system packages,
opened no GUI windows and changed no production desktop files. The installed
desktop executable remains the separately verified integration build described
above.

## Repeating GUI checks safely

The development workspace used its headless virtual-screen runner for GUI
checks. To repeat them elsewhere, provide a compositor session with private
Wayland, D-Bus, runtime and XDG directories. Never attach test windows
or fake input to the user's display, restart the working compositor, or launch
an image viewer on the working desktop to inspect artifacts. Qt offscreen is
appropriate for focused checks that do not require a compositor.

Read the installer before running it. Review the prepared backup/manifest,
close existing manager windows and retain the printed restore path. Installer
tests with fake commands do not replace verification of the chosen production
integration. Native device eject, live network transfers, all media codecs,
accessibility tools and every unsupported filesystem were not exhaustively
tested by this suite.
