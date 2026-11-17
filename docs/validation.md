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


## Finder v7 portable UX and lifecycle verification (2026-11-16)

All 19 CTest targets passed after the metadata lifecycle corrections. A later
full rerun printed all preview assertions as PASS but timed out during process
shutdown; its exact recheck passed in 7.13 seconds. The rebuilt metadata
consumers passed their component checks. The new
native Get Info host test occupies both metadata workers, admits a comment
save, attempts OK, Cancel, window close and embedded Escape, then releases the
workers and verifies the real xattr. The host remains alive during saving and
rename-busy guarding. File-details tests also cover hidden-page write delivery,
retargeting, drafts, failure and standalone/embedded close behavior.

The private Wayland integration suite passed 32 checks, the expanded feature
suite passed 34, and ordinary windowing passed 17. These overlap; their counts
must not be added as unique functionality. All 15 checks in the final v7 UX
harness passed, including post-rename General-page/name coherence. It additionally
covers real native view/tag/sidebar/new-folder shortcuts, native batch format
and per-step Undo, Get Info comments/extensions, safe path completion and tilde
navigation, enclosing-folder windows, native sidebar bookmarks, ancestor path
copy, slideshow reuse, all four search views, bounded repeated search Rename,
Escape, native properties reload and tab drag-out. The harness is
[tests/verify-finder-ux.py](../tests/verify-finder-ux.py); run it on a private
Wayland screen with `KWIN_WAYLAND_NO_PERMISSION_CHECKS=1`.

A warm tmpfs diagnostic exercised 1,000- and 10,000-file directories with 88
operations. Comparing native modification-time seconds directly replaced
repeated date conversions: the largest 10,000-file sort dispatch fell from
about 758 ms to 140 ms in single before/after runs, and the largest whole-case
10 ms heartbeat gap fell from 761 ms to 148 ms. These busy-machine samples are
not cold-disk throughput, a distribution-wide guarantee or macOS speed parity.
Large sorts remain synchronous. The diagnostic executable
`finder-browser-performance` is not a timing-based CTest assertion.
It measures the offscreen Browser/KIO components without compositor presentation
or a connected Preview controller. Dispatch timings are synchronous API durations;
whole-case heartbeat gaps include correctness scans.

All rendering, input and filesystem fixtures remained private. The scoped
production installation was verified on 2026-11-16: installed/staged executable
hashes match, Finder is the directory default, the running service owns both
Finder and FileManager1 interfaces, and the private test interface is absent.
Only executable bytes changed; existing launch routes were retained. Component
and native tests do not prove Apple pixel equality, physical touchpad feel or
access to Apple-only services.

## Wheel sensitivity and everyday UX verification (2026-11-16)

A private Wayland baseline with three system wheel lines moved one notch by
354px in 64px Icons, 69px in List, 297px in Gallery and 568px in 256px Icons.
The view's native `singleStep` was multiplied by the system line count, making
tile size affect wheel sensitivity. Capping the wheel step at 24px and applying
the default 50% app speed yields 36px, 34/35px, 36px and 36px respectively.
With four system lines, matching the user's desktop setting, the updated
measurements are 48px, 46px, 48px and 48px. Fine-angle fractions are retained;
keyboard and scrollbar steps remain native. These are measured Linux event
distances, not macOS input-device feel or physics comparisons.

The scroll component test passed bounded/fractional/phase input, immediate
reversal, endpoints, live preference defaults/validation, reduced motion, view
replacement, click/drag takeover and zero idle animation callbacks. Eight
private Wayland checks cover every Browser and search mode, large icons,
direct pixel/momentum input, live speed adjustment, identical distance with
easing disabled and cancellation on tab navigation. Reproduction uses
`tests/verify-scrolling.py --wheel-lines 4` inside the private virtual screen.

All 16 native UX checks passed, including Get Info from Quick Look and Inspector
using the same metadata editor/guarded host. Existing checks cover actual Meta
shortcuts, selection, folder creation, batch rename/conflicts/Undo, safe pathname
entry, sidebar bookmarks, enclosing-folder windows, slideshow, search in all
four modes, bounded rename preflight and tab detaching/overview. Appearance,
gesture and Get Info component checks passed. All 17 windowing checks passed:
native move/resize, fullscreen restoration, visible controls, text-focused
Copy/Cut/Paste/Undo, pinch, Quick Look ownership and tab opening/closing.
The expanded feature harness also passed all 34 checks for native file
operations, archive outputs/Undo, search, tab state and session restoration.
These suites overlap; their counts must not be added as unique functionality.
The reviewed behavior/provider boundaries are recorded in
[UX compatibility](ux-compatibility.md).

All automation remains on private offscreen/Wayland sessions. No physical
device or macOS side-by-side latency test was performed. Apple-only services,
Finder extension providers, native elasticity and exact macOS gesture physics
remain absent or unverified; these results establish the exercised Linux
behavior.

The scoped installer applied the tested update on 2026-11-16 with no open Finder
windows. Installed/staged hashes and the running executable match; native
directory defaults and FileManager1 methods pass verification, and the private
test interface remains absent. Only the executable changed. Existing app wheel
speed preferences are retained; missing values use the new 50% default.


## Recents routing and presentation update (2026-11-17)

The real `recentlyused` worker was exercised with a private WAL-mode activity
history and a private native image handler. All nine checks passed: empty
states in four modes; virtual listing identity and explicit target; all-mode
preview/clipboard/history routing; real image decoding; native Get Info;
clipboard targets; native Open launching the actual file; a rounded missing
original alert; and dismissal/navigation recovery. The installer-ready staged
binary was tested separately because CMake rewrites its build RPATH.
Reproduce `tests/verify-recents-presentation.py` inside the private Wayland
screen with `--binary` and `--output`.

All three new component suites pass (`finder-browser-targets-test`,
`finder-directory-state-test`, `finder-alerts-controls-test`). They cover native
URL MIME, provider filenames/locality, source identity, empty/filter/hidden
states, failed-listing recovery, empty child Columns, teardown, alert details,
Escape/default action, native ask-user interfaces, bounded diagnostics and
control painting. Existing Browser metadata/arrangement, Preview, Archive and
Quick Actions suites pass, including real native conflict and Undo checks.
These checks establish exercised Linux behavior, not literal macOS parity.

All 16 native UX checks and 17 windowing checks pass. The compositor material
check passes: 168 transparent-corner samples have zero changed pixels outside
the contour; all 45 partially covered corner samples receive material blur;
straight boundaries and the header seam retain their existing geometry. The
screenshot locator now matches opaque gradient control pixels because a color
threshold alone shifts on backdrop-dependent antialiased rims. Native input,
test windows, captures and fixtures stayed in isolated sessions.

Source/build checks are warning-clean for the changed application and new
components. Platform/provider startup diagnostics are retained in test logs;
physical-device feel and literal Apple material/gesture parity remain untested.
Installation status is tracked separately from these passing tests.

## Hosted build and compact layout, 2026-11-17

The [GitHub-hosted run](https://github.com/MSedgeMC/macos-finder-linux/actions/runs/44107329914)
built source `76f7f1af7310d5b65d271127fcb0f70e092d1480` in Ubuntu 26.04
and passed all 23 CTest suites. The artifact includes the executable, test
executables, source hash, package/linkage inventory, JUnit results and test log.
The downloaded binary SHA256 is
`77a3f0d502b303be1f499efc433a85979a4b9d224edbe3bf1c9747bc30b8632f`.
Its hash matches the artifact evidence and its dependencies resolve on the
originating Ubuntu 26.04 desktop. No local compilation was used for this update.

The downloaded compact-layout test passed locally through the private
offscreen wrapper. It exercises the 111px icon pitch, 20px native List rows,
column order, rounded real and empty stripes, nested directory selection,
all-mode selection, grouping, larger text, appearance and native glass-button
Space activation. Final 914×433 private Wayland Icons/List captures were
inspected directly. The synthetic files differ from the user's reference,
and the captures do not establish pixel parity.

The same downloaded binary passed all 16 native UX checks and 17 windowing
checks in isolated Wayland sessions. Existing tests cover native file rename
and Undo, metadata, search, tabs, Quick Look, resize/move, fullscreen restoration,
text editing and host-dispatched pinch. These are exercised Linux behaviors;
physical trackpad feel and Apple's platform services remain unverified.

Native compositor material checks passed at 1× and 1.5× application scale.
All 168 / 428 fully transparent corner samples had zero blur-induced changes
outside the contour; all 45 / 68 partially covered corner samples received the
material. Backdrop-detail ratios were 0.056 / 0.146, and toolbar-capsule blur
deltas were 31.76 / 31.64. Changing the live patterned backdrop changed the
glass while leaving the opaque file pane unchanged. A fractional-scale
locator correction samples around visible control symbols. Test processes,
input and captures stayed on private screens; expected private portal/software
fallback diagnostics remain in the logs. Genuine refraction and Apple's
renderer are not implemented. Installation is recorded separately after the
scoped installer verifies the production copy.

The scoped installer applied and independently verified that exact remote
binary on 2026-11-17. Installed bytes match the artifact, Finder is the folder
default and active FileManager1 owner, zero windows were open, and the private
test interface was absent. Only the executable changed among the nine
integration files. The prior layout settings were backed up before applying
the 166px sidebar and replacing the previous default spacing/List size;
custom icon sizes, saved columns and file/session state were retained. No
working-screen input or compositor restart was performed.
