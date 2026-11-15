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
| Recursive search | 21 | Boolean criteria, bounded Unicode content, keyword parsing, existing-index provider scope, hidden behavior, bounds, cancellation and responsiveness |
| Linux tags | 9 | Existing metadata, custom/Unicode tags, unrelated xattrs, invalid inputs, idempotent removal and storage/path errors |
| Installer | 14 | Offline prepare/apply/restore recovery cases using fake external commands and a fake session bus; no production writes in these cases |

The earlier search suite also passed under sanitizers. The current worker adds
Boolean criteria and optional existing-index coverage; a private Baloo 6.24
fixture verified real PDF/text terms, name/root queries and stale link/pipe
exclusion while leaving index contents unchanged. The private global-menu
registrar check confirmed native File/Edit/View/Go/Window menu export through
`com.canonical.dbusmenu`, including navigation, view modes and file actions.

The scoped production installation was subsequently verified on the originating
desktop: Finder was the folder default, the windowless manager daemon owned
both application and FileManager1 service names, and the private input/render
test interface was absent. This is a deployment observation on that machine,
not a claim that installation changes are exercised against every distribution.

## Standalone portable build

The exported Qt/KDE-target CMake configuration and complete Release build of
this standalone repository passed on 2026-11-15. All 14 CTest targets passed: search, tags, scrolling, previews, Quick Actions,
Smart Folder loading, browser metadata, icon arrangement/spring loading,
archives, criteria editing, Inspector, appearance four-mode search results and native gestures. A binary-only staged installation
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
The latest staged and installed update passed all 22 boundary captures.
The installed copy also passed the compositor blur check (detail ratio 0.119)
with an unchanged opaque file pane. Its hash matched the staged executable,
Finder remained the folder default and the private test interface was absent
from the production daemon. Only the executable content changed; the update
restarted the windowless file-manager daemon and retained a scoped backup.
All 14 offline installer cases passed, including preservation of existing
MIME-file permissions.

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


## Expanded native features and review regressions

A further **34 native feature checks passed** on a private Wayland screen.
They cover archive integrity and native Undo; aliases and original reveal;
Trash Put Back; four search result modes with shared exact-URL selection;
actual decoded text/image Gallery content; primary-item changes and clearing/
reselection; independent tab searches; search-tab detachment restoring the
source folder; Smart Folder criteria; Inspector; per-mode/folder view options;
window tiling; closed-window restoration; hidden-window session restoration;
and light/contrast/transparency controls. The harness is
[tests/verify-features.py](../tests/verify-features.py).

A source review found three integration issues, all fixed and exercised by
these checks: preserving the previous Gallery item instead of the new primary,
source search state after a tab moved, and visibility lost when a hidden window
was serialized. Light toolbar glyphs now refresh with appearance changes.
These establish the checked Linux interactions; they do not establish Apple's
scroll physics, overall speed or full Finder equality.

The latest material verification held tint constant while toggling native blur.
Its detail ratio was **0.118**, the live backdrop color delta was **28.83**, and
the opaque file pane remained unchanged. The sidebar boundary suite passed
**22 captures at 1× and 22 at 1.5×**. Component totals overlap and must not be
added together as a count of unique functions.


The installed KDE 6.24 directory watcher can discard a DELETE received within
the same timestamp second. Plain native KDirModel reproduced that fault. The
app now observes loaded local directories with Qt's event-driven watcher and
coalesces changes into the public KIO incremental directory update. Native KIO
retains row/selection changes; no idle polling is added. Six repeated external
create/select/delete cycles with pinned directory timestamps passed, alongside
the metadata and arrangement/spring-loading suites.


## Final large-folder sample

The final staged Release executable listed **5,000 files in 858 ms** on the
originating machine, then measured **0.0% process CPU** in one-second settled
idle samples in all four views. Resident memory was approximately **195 MiB**.
The same harness's earlier baseline took 1,246 ms and used approximately
192 MiB. These are single local samples, not a macOS comparison or universal
performance guarantee. Diagnostic-call P95 was 1.84–5.79 ms across views; that
measures the test D-Bus response and must not be reported as frame time. Builds
and other test sessions were stopped for this measurement.


The final scoped update was installed and verified on 2026-11-15. The installed
executable matched the privately verified staged hash; Finder remained the
folder default, the windowless daemon owned FileManager1, and the private test
interface was absent from the production session. The installer retained the
previous executable and its scoped restore manifest. No working-desktop test
windows, automated input or compositor restart were used.

## Rounded chrome, windowing and touch follow-up

On 2026-11-15, all 14 isolated CTest targets passed, the native integration suite passed 32 checks, and the extended feature suite passed 34 checks. A new private Wayland window/input suite passed 17 checks: actual edge resize and title drag through the compositor, resize cursor restoration, maximize/full-screen/restore, green primary action, retained controls/sidebar with hidden toolbar, sidebar action consistency, dialogs, text Paste/Cut/Undo, host-dispatched pinch sizing, Quick Look hosting, activation preserving maximization, and tabs.

Real compositor material captures checked all four corners at 1x (168 fully transparent pixels) and 1.5x application scale (428 pixels): toggling native blur/contrast changed zero pixels outside the app's alpha contour. The header/sidebar shadow line is absent. Native backdrop detail was reduced to about 5% of the unblurred sample; changing the backdrop changed the glass while the opaque file pane remained unchanged. These captures use a private patterned layer-shell background, not wallpaper substitution.

A private session loaded the installed Rounded Corners plugin with copied desktop settings. Its duplicate outline was reproduced, then excluded only for Finder. A toggle of that plugin produced identical Finder pixels after exclusion. Screenshots were inspected from files. Gesture tests dispatch public Qt native/touch events and cover cancellation, phases, selection/wheel compatibility and lifecycle; they do not establish physical trackpad/touchscreen feel or Apple gesture physics.

The final staged binary has no private-prefix RPATH and the scoped desktop installer prepares nine integration files with only the executable bytes changed. The originating desktop verified matching installed hash, folder default, application/FileManager1 owner, zero manager windows and absence of the private test interface. No working-screen input or compositor restart was performed.


### Quick Look and border correction, 2026-11-15

A fresh private compositor reproduced a stale Rounded Corners exclusion on an
existing Finder window. General KWin reconfigure left the outline in place;
effect-specific reconfigure refreshed settings but retained the cached window
exclusion. Recreating only that effect removed the line: the opaque toolbar
boundary and its adjacent row both returned to #1e1e1e. The helper now refreshes
loaded effects even when the exclusion is already present and preserves Breeze
keys changed by the upstream reload.

The client and blur region now use matching antialiased raster coverage,
including the final coating-alpha rounding. Tests check the partial edge as well
as transparent pixels and long straight boundaries. At 1x, all 45 partial
corner samples received the material and all 168 transparent corner samples
remained unaffected. At 1.5x, the corresponding counts were 68 and 428. This
corrects the sharp backdrop fringe that the previous outside-alpha test missed;
a native binary blur region still does not provide Apple's coverage renderer
or refraction.

The private Wayland Quick Look suite verifies its one-row 40px header, opaque
content over changing patterned backdrop, unclipped controls at 480x320 and
620x420, multi-file navigation and fullscreen restoration. The preview,
appearance and gesture CTest targets passed. Screenshots were inspected from
files. These checks establish the repaired Linux behavior, not literal Apple
pixel or gesture parity.
