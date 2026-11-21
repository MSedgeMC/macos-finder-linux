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

## Reference-driven material correction, 2026-11-17

The user's live capture exposed a backdrop-ownership error missed by the prior
blur-only assertions: transparent holes under the toolbar capsules sampled
the desktop behind the opaque file pane. The correction gives the toolbar a
continuous dark content surface and neutral grouped controls. Only the sidebar
and its header segment receive native blur. Sidebar tint is RGB 21/20/18 at
232/255 alpha in normal dark mode, with reduced backdrop saturation. Saved
sidebar width is reapplied after the initial native window layout.

The settled layout uses a 166px sidebar, 52px header, 111px default icon pitch,
73×36 navigation and 158×36 four-view capsules. Wider filename space permits
middle elision on the second line. Single-tab windows hide the New Tab corner
control; visible multi-tab bars retain it. The user's selected Ubuntu Medium
font and persistent window-control symbols are deliberate platform choices.
The Recents title describes its location instead of exposing the worker path.

Functional source `4608a7303c29a65edda9155756669b7f24e98af2` passed all 23
component suites in GitHub Actions and then 16 native UX, 17 windowing and 22
sidebar captures using the downloaded binary in private Wayland. Material
checks at 1× / 1.5× measured zero toolbar changes under blur or backdrop-color
toggles. All 168 / 426 transparent corner samples stayed unchanged outside the
contour, and all 47 / 71 partial corner samples received material. Blank
sidebar blur detail ratios were 0.023 / 0.025; foreground text/icons were
excluded because they remain sharp. The final artifact verification below also
covers the later sidebar tint and shared-users/location-title changes.

Static reference-region comparison established exact RGB 30/30/30 equality
in sampled toolbar and file-pane backgrounds. This limited comparison does
not establish whole-window pixel parity: files, thumbnails, font, icons,
compositor and backdrop differ. The added user-selected Tahoe KDE source was
reviewed at `162022f8df33cdfb37bc8f11ff331be9abc6725d`; its Acrylic Glass
KWin implementation supplies genuine compositor-source edge optics, not a
Qt Widgets drop-in. No project code, assets, installer or compositor change
was imported. Genuine refraction and Apple's renderer remain unavailable in
this application. The final artifact and installation were verified as follows.


## Final hosted correction and deployment, 2026-11-17

[GitHub Actions run 44117017974](https://github.com/MSedgeMC/macos-finder-linux/actions/runs/44117017974)
built functional source `601b9b78ad28eff846796962b7dfef1b561b3b44` and passed
all 23 component suites. The downloaded and installed binary SHA256 is
`fd9ec51a349016d0e499fb86acc796465265f36bd93b75b1cc520f145921d008`.
Artifact source/hash evidence, JUnit results and local runtime linkage were
verified before installation. All compilation remained on GitHub-hosted CPUs.

That exact binary then passed 16 native UX checks, 17 windowing checks,
9 Recents/empty-folder/error checks and 22 sidebar boundary/layout captures
in separate private Wayland sessions. The Recents suite uses an isolated real
KIO activity-history fixture and exercises real-file preview/Open/Get Info,
clipboard routing and missing-original feedback. Input, screenshot capture and
test windows stayed off the working desktop, and their processes were removed.

Native material checks passed at 1× and 1.5× scale: toolbar blur and backdrop
color deltas were both zero, the opaque file pane stayed unchanged, and all
168 / 426 fully transparent corner samples had zero changes outside the
contour. All 47 / 71 partially covered corner samples received material.
Blank sidebar detail ratios were 0.0226 / 0.0253. Foreground text and icons are
excluded from those samples and remain crisp. Logs retain expected private
portal/software-backend diagnostics; physical device feel is not measured.

914×433 Icons/List client captures were inspected directly. The 166px sidebar,
52px header, 158×36 view capsule and 111px icon pitch match the measured
reference geometry. Three uniform reference regions have mean absolute RGB
error zero: file pane and header RGB 30/30/30, empty sidebar RGB 21/20/18.
These are limited static client-surface measurements, not whole-window pixel
equality, live composited-material equality or proof of Apple's renderer.
File images, names, selected Ubuntu Medium font, platform icons and backdrop
differ. Real refraction is not implemented by these Qt Widgets controls.

The scoped installer applied and independently verified the artifact on
2026-11-17 with zero Finder/Quick Look windows open. Installed bytes match the
staged release; Finder remains the directory default and FileManager1 owner;
the daemon is active and the private test interface is absent. Only the binary
changed among nine integration files. Layout preferences were separately
backed up before replacing the erroneous 228px sidebar width with 166px,
including the last-closed session width, and hiding Home/Recent Locations to
match the supplied sidebar hierarchy. Other view, session and metadata values
were preserved. Only the file-manager daemon restarted; the working compositor
and desktop shell were not restarted.


## Sidebar frost and color tuning, 2026-11-18

[GitHub Actions run 44326241620](https://github.com/MSedgeMC/macos-finder-linux/actions/runs/44326241620)
built source `f5052318857cf538f4f12ce453ed13a87d92805d`, passing all 23
component suites. Downloaded artifact SHA256 is
`714cca7af4883a1a6a3e75a1853506fbe5b207998b631695f9604a1b1faba49b`;
source/hash/JUnit evidence and local runtime linkage were verified. There was
no local compilation.

Dark sidebar frost increases backdrop transmission from 9.02% to 16.86%,
retaining RGB 21/20/18 and native saturation 0.35. Light transmission increases
from 5.88% to 9.02%, with saturation 0.7. Text/glyph contrast checks include
both white and black backdrops; high contrast and reduced transparency keep
opaque fallbacks. Sidebar glyph blue and neutral header/selected control
colors were tuned against the supplied Finder capture. Resting toolbar fills
and file-pane colors remain neutral and content-backed. Original bevel
painting uses a quiet middle and top/bottom highlights; there are no new
captures, shaders, idle timers or stacked compositor blur layers.

The exact downloaded binary passed native material checks at 1× and 1.5×.
Toolbar blur and backdrop-color deltas are zero. The sidebar's patterned
backdrop color delta is 3.457/3.931 RGB, with blurred/sharp detail ratios
0.0144/0.00777. All 168/426 transparent corner samples stayed unchanged outside
the contour; all 47/71 partial samples received material. The opaque file pane
stayed unchanged. Dark/light before/after native captures over a private copy
of the desktop wallpaper were inspected directly. A screenshot locator was
corrected after the capture helper deactivated the native traffic lights;
this was a test-harness failure, not an application crash. All 22 sidebar
boundary/layout captures passed, including modes, grouping, resize and search.

914×433 Icons/List captures retain reference geometry. Sampled resting
control fill, file surface and header background retain zero RGB error.
A static matching view-control body/bevel profile improves from mean absolute
RGB error 3.27 to 2.72. These small sampled comparisons do not establish
whole-window pixel equality, genuine refraction or Apple's renderer.
The selected font, icons, fixture names/images and backdrop differ. Current
Apple Liquid Glass and adoption guidance was refreshed alongside the pinned
community-source review; its AppKit/SwiftUI APIs are not Qt implementations.

The scoped installer applied and independently verified that exact artifact on
2026-11-18 after Finder and Quick Look were closed. Installed bytes match the
staged GitHub build; Finder remains the directory default and FileManager1
owner; the daemon is active and the private test interface is absent. Only the
executable changed among nine integration files. The previous executable and
integration routes were backed up. The Finder preferences file retained its
exact SHA256 through deployment. Only the file-manager daemon restarted; no
working-screen input or compositor restart was performed.

### Finder/Preview completion pass, 2026-11-18

All compilation runs on GitHub Actions. Run
[44359283840](https://github.com/MSedgeMC/macos-finder-linux/actions/runs/44359283840)
built e7bf3be and passed all 25 component suites, including standalone Preview
and its scoped installer. Its exact artifact passed 34 private-Wayland feature
checks, six presentation groups, 17 native windowing checks and three
standalone image/PDF/text launch captures. The viewer did not acquire Finder
or FileManager1 services. These counts overlap and do not measure macOS parity.

The broad suite initially exposed a Browser teardown crash during session
restoration. A native debugger traced KCoreDirLister's destructor cancellation
to a callback using destroyed private state. Child callbacks now disconnect
before that state is released. The component regression dispatches
cancellation during child teardown and destroys pending listings. Both the
minimal two-window restart and full hidden-window search/chrome restoration
pass on the fixed artifact.

Native captures also established a duplicated selected-thumbnail label.
The final viewer-only correction paints each label once while retaining
selection, keyboard focus and filename elision. Run
[44360372769](https://github.com/MSedgeMC/macos-finder-linux/actions/runs/44360372769)
built 2b24f23 and passed all 25 component suites. Its exact artifact passed
three private-Wayland image/PDF/text launches; saved compositor captures were
inspected directly. The thumbnail divider has no painted black strip, long
filenames elide without horizontal scrolling, and selected labels paint once.

The release was installed on 2026-11-18 using the existing zero-open-window
Finder installer. Installed and running binary SHA256 are both
`5c1a98bc6f5c5ba7759eae69b322b688e83fc0209ace695e7634594eb3e89afb`.
Finder and FileManager1 share the expected active service owner; native methods
are present and the test interface is absent. Finder preferences retained their
exact pre-install hash. Only the executable changed among the nine Finder
integration files.

The separate Preview installer backed up and wrote its launcher, desktop entry
and MIME defaults. PDF, PNG, JPEG, SVG, WebP, BMP and TIFF resolve to Preview;
text remains with Kate and office documents remain with LibreOffice Writer.
The installer verified staging hashes/defaults and retained MIME permissions.
Scoped rollback records exist for both deployments. No working-screen input,
preview display, compositor restart or local C++ compilation was performed.

### Current Preview reference correction, 2026-11-20

Apple’s current macOS page, stable release listing, Preview guide and Design
Resources were refreshed: macOS 27 Golden Gate, stable 27.2.1; 27.3 beta is
listed separately. Current image/PDF screenshots guide the viewer hierarchy.
KDE/Flutter source and MacRumors discussions were also reviewed. Old viewer
screenshots serve code/feature comparison only; Reddit returned access errors.

The standalone viewer now uses a leading document title/page-or-scale subtitle,
grouped icon controls, adaptive thumbnails and on-demand PDF search. Quick Look
retains its own hosted header. Application menus export to Plasma with a local
menu/F10 fallback. Get Info/rotated-copy actions have native handlers; Share
uses actual URL-copy/email choices. User typography and right-side controls
remain explicit differences. Apple rendering, PDF editing/signatures, HDR,
pressure acceleration and other unimplemented current features are not claimed.

[GitHub run 44755587528](https://github.com/MSedgeMC/macos-finder-linux/actions/runs/44755587528)
built f1308e0 and passed all 25 component suites. Its exact artifact also passed
seven private Wayland image/PDF/text and appearance launches and six Finder
presentation groups. Saved screenshots were inspected directly. Component checks
cover functional grouped zoom, PDF title/search, thumbnail preference/geometry,
menu fallback, narrow layout, native MIME, action updates and destruction.
These overlapping checks establish local correctness, not full macOS parity.
No compiler warnings were found; private native helpers reported software-rendering
fallback/portal warnings. No working-screen automation or local compilation occurred.

Artifact SHA256 is
`caf77ef46073884d6f06a3eb31efeb5f81266a668b8d9349be1e0a23af9ee698`.
That artifact was initially staged while the open-window guard prevented
installation. It is superseded by the configurable-controls release installed
and verified on 2026-11-21 below, which includes the Preview corrections.

### Configurable controls, sidebar translucency and mouse focus, 2026-11-21

[GitHub run 44779867877](https://github.com/MSedgeMC/macos-finder-linux/actions/runs/44779867877)
built source `6dfe4987e780a37e3f2bdba6ee590998ea36ab90` and passed all 25
isolated component suites. All C++ compilation ran on GitHub Actions. The
downloaded source record and SHA256 were checked, and local runtime linkage
resolved without missing libraries. Binary SHA256 is
`9e5cb1eba55649829003662d5368ad3323cc3f6fd3ef86811dfe242ba8b0e6a4`.

Finder General Settings and standalone Preview Settings share immediate
Glossy/Flat, Left/Right, Always visible/On hover and 0–40% sidebar-translucency
controls. Defaults preserve glossy right-side controls with visible symbols
and dark/light 17/9% backdrop contribution. Hover reveals the whole control
group; keyboard focus and high contrast retain identification. Accessibility
overrides keep the coating opaque without discarding the slider preference.
Native button painting suppresses mouse-origin focus/default outlines without
changing Enter/default actions, pressed feedback or keyboard focus indication.
Atomic settings replacements refresh separate Preview processes through native
file/directory signals; there is no new polling or capture loop.

The exact final artifact passed 11 private native-Wayland appearance groups
and six presentation groups. These cover live ordering, glossy/flat rendering,
real group hover, mouse focus, Quick Look placement/close, dark/light alpha,
opaque document content, transparent contours, accessibility, narrow Settings,
standalone controls/lifecycle and composited Preview frost. Six presentation
groups cover view options, all Settings pages, Gallery, search and image/text
Quick Look. Saved captures were inspected directly, including the absence of
the unwanted blue Other Folder button outline. An earlier artifact at
`33a04c6` passed 17 windowing checks; those were not rerun after the final
two-line default-button painting correction. Test counts overlap and do not
establish macOS parity. Native test helpers reported software-rendering and
private-session warnings; they did not affect the passing checks.

After the user confirmed closure and private tests finished, the unchanged
zero-open-window installer prepared a rollback backup and installed the exact
artifact on 2026-11-21. Independent verification established artifact/staged/
installed/running hash equality, the active Finder daemon, directory default,
shared Finder/FileManager1 ownership and native methods, and absence of the
private test interface. Only the executable changed among nine integration
files. Finder preferences retained their exact pre-install hash. Existing
Preview launcher and PDF/image MIME routes match their current expected bytes;
text remains with Kate and ODT with LibreOffice Writer. No separate Preview
route rewrite was needed. Only the file-manager service restarted; the working
compositor and desktop shell were not restarted. Private test processes and
runtime directories were removed, with no working-screen input automation.

Current audited standard-window geometry and colors inform the original
traffic-light drawing. The glossy shading is a requested Linux approximation
inspired by Finder and Apple Liquid Glass; exact macOS pixels or Apple's
renderer are not claimed. Sidebar frost uses real region-limited KWin blur,
not wallpaper substitution or a claimed refraction texture.

### Compact sidebar, stripes and shared popups, 2026-11-21

[GitHub run 44872981530](https://github.com/MSedgeMC/macos-finder-linux/actions/runs/44872981530)
built source `c650bca3ae0487f691aaf077fb028a30865e03fd` and passed all 25
isolated component suites. All C++ compilation ran on GitHub Actions. The
source record, archive hash, executable hash and local linkage were checked.
Final executable SHA256 is
`bfd0ae7b9bf41058b36356979e7aa3f427ec86f3c0e34c34f5545268384209b4`.

The exact artifact passed six focused private Wayland UX groups, six
presentation groups and 17 windowing checks. The focused checks exercise
24/32/40px live sidebar rows, no orphan headings, the functional leading
sidebar toggle, stripe removal/restoration without file or selection changes,
a real pointer-opened compact Actions menu with Settings first, F10 access,
dialog close and narrow Settings. Presentation checks cover all four browser
view options, all Settings pages, Gallery, search and hosted image/text Quick
Look. Windowing checks cover real move/resize, fullscreen restoration,
controls, text-focused editing, pinch and tabs. The preceding artifact at
`8416e2c` passed all 34 broader file/search/session feature groups; those were
not repeated after the final selector-only neutral-button correction.
Counts overlap and must not be summed as unique functionality.

The shared-dialog component regression checks alpha corners/opaque centers,
unchanged parent modality, one header/margin after reopening, resize and
control placement, Enter acceptance exactly once, close rejection and
immediate cancellation without queued resurrection. Menus retain native
keyboard action dispatch. An initial top-level graphics-effect contour failed
the alpha assertion; the passing implementation paints a child surface and
adopts chrome during Polish before the native window is shown. Saved native
captures were inspected, including compact menus, neutral Other Folder,
Settings, View Options and image/document previews. Native helper logs report
private-session/software-rendering limitations; physical-device feel and
macOS pixel equality were not measured.

With no open Finder windows, the unchanged scoped installer prepared a
rollback backup and installed the exact final artifact. Independent checks
confirmed artifact/staged/installed/running hash equality, directory default,
Finder/FileManager1 shared ownership and methods, and absence of the private
test interface. Only executable bytes changed among nine integration paths.
Finder preferences retained their exact pre-install hash; Preview launcher
and PDF/image routes still match their expected bytes. Private processes and
runtime directories were removed. No working-screen input automation or
working compositor restart occurred.

Current Apple menu guidance and the locally audited standard sidebar/menu
presets informed density and hierarchy. This remains a Linux approximation
inspired by Finder and Apple Liquid Glass. Popup material is an opaque rounded
fallback; existing sidebar frost uses region-limited KWin blur. External
portal/provider windows retain their own presentation. Neither Apple's
renderer nor complete macOS appearance/feature equality is established.
