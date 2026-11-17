# Finder interaction and compatibility review

Reviewed and exercised on 2026-11-16. This application is a Linux approximation
inspired by Finder. The review checks predictable actions, selection ownership,
input distance, completion/error handling and native integration. It does not
establish a complete 1:1 macOS experience or physical-device feel.

Apple's current public [Finder guide](https://support.apple.com/guide/mac-help/organize-your-files-in-the-finder-mchlp2605/mac),
[keyboard guide](https://support.apple.com/guide/mac-help/keyboard-shortcuts-mchlp2262/mac),
and [scroll-speed guide](https://support.apple.com/guide/mac-help/change-your-mouse-or-trackpads-response-speed-mchlp1138/mac)
were consulted. They describe behaviors and settings; they do not publish
Finder's exact acceleration, friction or wheel-easing constants.

| Interaction | Handling in this application | Compatibility boundary |
| --- | --- | --- |
| Wheel and touchpad scrolling | Mouse-wheel distance is bounded independently of icon size; Settings → Advanced → Mouse wheel speed defaults to 50%. Pixel input and native momentum move directly without an added tail. Reversal, clicks, keys and navigation cancel wheel animation. | No native elastic overscroll. Physical touchpad feel and macOS acceleration are unverified. |
| Selection and four views | Icons, expandable List, Miller Columns and Gallery retain URL identity, primary selection and Meta/Shift behavior. Search shares one selection model across all four presentations. | Search Columns presents flat hits with location details, rather than a directory hierarchy. |
| Files, clipboard and Undo | KDE KIO performs real copy/move/link, Trash, rename, conflicts and Undo. Text-focused Cut/Copy/Paste/Undo stays with the editor. Batch rename captures its selection and reports partial outcomes. | Batch Undo is per successful rename step, including cycle temporaries. Linux links/tags/filesystem metadata differ from macOS aliases, Finder flags and APFS services. |
| Navigation and windows | Back/forward, tabs, overview, detaching, enclosing-folder windows and saved state use actual folder/search state. Native Qt/KWin move, resize and fullscreen restore preserve the previous window state. | Wayland activation restrictions apply. macOS Spaces, Mission Control and Stage Manager are not reproduced. |
| Get Info and Inspector | File menu, Quick Look and Inspector open the same guarded properties host. Comments/extension preferences share the same editor; explicit saves and custom renames preserve host lifetime. KDE handles permissions and Open With. | The properties layout is KDE's presentation. Comments and extension preferences use Linux xattrs, not Apple's metadata representation. |
| Quick Look and actions | Images, bounded text, PDF navigation/search/selection, media playback, slideshow, printing and export use real decoders and native output handling. | Installed codecs, thumbnailers, FFmpeg and encoders determine coverage. PDF combination/printing uses raster fallbacks; OCR and structural PDF editing are absent. |
| Sidebar and devices | KFilePlacesModel retains bookmarks, mounted devices, eject/mount, native menus and drop handling behind the sidebar presentation. Tags search actual discovered metadata. | Linux protocols and devices replace Apple locations. Apple iCloud, AirDrop and account services are absent. |
| Search and responsiveness | Local scans run with budgets/cancellation. This Computer queries a compatible existing Baloo index and reports coverage. Visible thumbnail batches and metadata caches bound background work. | Existing indexes can be stale or incomplete. Folder-size and metadata scans are bounded; blocked filesystem calls cannot be forcibly interrupted. Large native sorts remain synchronous. |

The wheel issue was a real implementation fault: multiplying system wheel lines
by the native tile-sized scrollbar step made large icons travel excessively per
notch. The new wheel-only step cap leaves keyboard/page/scrollbar behavior
intact. Another inconsistency was corrected: Get Info from Quick Look and
Inspector now includes the same comment/extension editor and save guards as
the File menu route.

Eight scrolling checks, 16 native UX checks, 17 windowing checks and 34 expanded
feature checks passed on separate private Wayland screens. Their coverage
overlaps; counts are not a percentage of Finder parity. Targeted scroll,
appearance, gesture and Get Info component suites passed. See
[validation](validation.md), [design and limits](design.md), and the reproducible
[scrolling](../tests/verify-scrolling.py), [native UX](../tests/verify-finder-ux.py),
[windowing](../tests/verify-windowing.py) and [feature](../tests/verify-features.py)
harnesses. All input automation and screenshots stayed off the working desktop.


The Recents update corrects a provider-routing bug: thumbnails could resolve a
real target while Open received a synthetic `recentlyused:` address that the
worker cannot stat. Opening, previews, properties and native clipboard/drag
now use the explicit target and preserve the listing's model identity. Empty,
filtered, hidden-only, loading and failed locations receive distinct accessible
text/vector feedback. Rounded errors and glossy traffic lights are original
Linux adaptations; native KDE conflicts/authentication and the existing window
session behavior remain authoritative. Real-worker and staged-binary tests
are documented in [validation](validation.md).
