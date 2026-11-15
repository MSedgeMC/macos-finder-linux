# Repository workflow

For authorized work on this repository, commit each completed logical change
promptly, including small corrections. Start its Git push and continue
independent coding, documentation or tests while the upload runs. Serialize
pushes to the same branch and verify remote completion before final handoff.
Do not batch unrelated completed changes into one end-of-task commit.

Use public Qt/KDE APIs and preserve native file operations, error/conflict
handling and Undo. Material painting must remain separate from file/session
logic. Read the scoped installer before changing deployment behavior.

Keep GUI test windows and automated input off the working desktop. Use a
private headless Wayland compositor with its own socket, session bus, runtime
and writable XDG directories. Save screenshots to files; do not open viewers
on the working desktop or restart its compositor for verification.

Describe this application as a Linux approximation inspired by Finder and
Apple Liquid Glass. Do not claim Apple's renderer, platform services or exact
pixel equality without comparative evidence. Do not include proprietary Apple
UI-kit files, fonts or images. Keep local paths, credentials, caches and build
outputs out of Git.
