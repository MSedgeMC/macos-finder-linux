# Validation

The originating Kubuntu desktop build passed the following checks on
2026-11-15. These results establish tested Linux behavior, not macOS platform or
pixel parity. The complete rebuild was verified from this standalone repository. A different
dependency set or desktop should run the available checks again.

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
| Recursive search | 13 | Hidden behavior, result/entry/time bounds, canceled generations, replacement/destruction and UI responsiveness |
| Linux tags | 9 | Existing metadata, custom/Unicode tags, unrelated xattrs, invalid inputs, idempotent removal and storage/path errors |
| Installer | 14 | Offline prepare/apply/restore recovery cases using fake external commands and a fake session bus; no production writes in these cases |

The earlier 11-case search suite also passed under sanitizers; the current
13-case suite adds real tag filtering and preferred-subtree bounds. The private global-menu
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
13 recursive-search cases and 9 tag cases. A binary-only staged installation
contained exactly `bin/finder`. Matching development dependencies were supplied
through a private extracted prefix; these checks installed no system packages,
opened no GUI windows and changed no production desktop files. The staged binary has no private development-prefix RPATH and passed its own
isolated boundary and Gallery action tests before deployment.

## Actual blur and Gallery actions

A private compositor capture compared the same patterned backdrop with native
KWin blur enabled and disabled. Sidebar high-frequency detail fell from 70.87
to 8.73 (ratio 0.123); changing the live backdrop changed its visible sidebar
color by 28.71. The opaque file pane was identical between blur states. This
verifies genuine compositor backdrop blur, not a wallpaper replacement, an
alpha flag alone or Apple's proprietary refraction. The material harness is
[tests/verify-material.py](../tests/verify-material.py) and requires private
KWin, PyQt6 QtQuick, KDE layer-shell, Spectacle, python-dbus and Pillow.

Eleven focused native Gallery checks passed on the final staged executable:
Show More metadata expansion/collapse; standard and custom tag add/remove;
invalid input, permission errors and retry; symlink mutation disabled; full
original-resolution rotation with occupied destination names; color-profile
and DPI-axis preservation; and two native Undo cycles. Source file hashes,
existing tags and unrelated metadata remained unchanged. These checks used
only disposable files and a private Wayland session.

The complete operation harness is
[tests/verify-integration.py](../tests/verify-integration.py). It uses the same
private screen isolation and accepts `--binary` and `--output-dir`.

## Repeating GUI checks safely

### Sidebar boundary regression

On 2026-11-15, the original capture contained a fully transparent pixel column
at x256 across all 620 body rows. The corrected layout eliminates this strip
and keeps the content-side boundary opaque. Twenty-two isolated native Wayland
captures passed at both 1× and 1.5× scaling: all four views, wider/narrower
sidebar drags, minimum/restored window sizes, sidebar hide/show, a visible tab
bar, search results, all six grouping choices, side Preview, actual tag
results with fixture selection and Escape, and maximize/restore. Native resize events changed the
sidebar width while preserving adjoining panes. The final screenshots also
measured the 73×36 navigation and 142×36 view capsules; no working-desktop input
or GUI previews were used.

These are boundary and geometry checks, not proof of complete macOS parity.
The generic-kit comparison and remaining differences are described in
[design and platform scope](design.md).

The previous installed boundary fix passed 14 captures and nine smoke checks.
The latest staged update passed all 22 boundary captures. All 14
offline installer cases passed, including preservation of existing MIME-file
permissions. Deployment changed only the executable and restarted only the
windowless file-manager daemon; staged hashes, folder-default and FileManager1
routes were verified afterward.

The regression harness is [tests/verify-sidebar.py](../tests/verify-sidebar.py).
It needs Python PyQt6, python-dbus and Pillow, and must be launched by the
private Wayland runner described below (or an equivalent environment supplying
its isolation markers and source configuration paths). Pass `--binary` and
`--output`; it deliberately refuses the working display. Repeat with
`QT_SCALE_FACTOR=1.5` to exercise fractional scaling. Its input bridge is
available only when the application has both `--test-mode` and the private
screen marker.

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
