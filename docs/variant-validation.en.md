# Edition distribution validation · 2026-11-09

[简体中文](variant-validation.md)

## Problem

GDI used main, QML used qml, and both produced ambiguous filenames. QML packaged successfully without its DLL. CI lacked Qt / host source setup, and executable / Qt window icons were incomplete.

## Cause

The release setup came from a single-executable template. QML set only its custom caption icon. The host already implements replacement by ID, so no conflict field is needed.

## Changes

Two branches, edition-specific tags, package filenames and display names; one shared installation ID. Isolated builds, shared artwork, pre-packaging validation, pinned QML dependencies, bilingual instructions and branch CI. QML stays a preview.

## Measured

- MSVC 19.51, Windows x64 Release. GDI CTest 2/2; QML CTest 3/3. Both include six packaging regression tests; QML also runs expression specifications.
- Packaging tests cover a missing DLL, wrong-edition / legacy / wrong-version tags, missing English notes, exclusion of stale DLLs, deterministic bytes and SHA256.
- Production `install::inspect/place` installs real packages: fresh GDI → QML → same-version QML → GDI downgrade. Each step leaves one plugin directory and the QML DLL only when appropriate. History and separate settings / hotkey file fixtures remain byte-identical. An executable locked without delete sharing prevents replacement and retains GDI. See [installer output](variant-install.txt).
- Actual Host / Runner (wsbench) executes packaged binaries, queries bits / bytes / struct successfully (6 / 5 / 5 rows per edition), and invokes openEditor for `bits f32 1.5`. LOW integrity, no elevation, in a job, clean exit.
- 75 window checks pass: [machine results](variant-checks.json). Large / small WM_GETICON handles match app.ico pixels (one alpha premultiplication rounding unit allowed for Qt). GDI handles simulated 96 / 120 / 144 / 192 DPI messages with correct sizes and pixels.
- Reviewed actual [GDI](gdi-window.png) and [QML](qml-window.png) screenshots. Captions display the shared icon and edition name.
- Only test-owned processes were started / stopped. Installer temporary profiles clean themselves up. User plugins and settings were untouched. Build logs remain under workspace `build/binarylab-variants-20261109/`; packages under `dist/binarylab-variants-20261109/`.

## Not tested

- Four QML launches requested QT_SCALE_FACTOR values, but native icon checks ran at the actual 96-DPI desktop. This does not establish native DPI transition coverage. Real cross-monitor dragging, taskbar and Alt+Tab visual checks were not automated.
- No repeated GUI installation confirmation or shortcut restoration clicks. Production installer functions test directory replacement; independent file fixtures test retention. Full running-plugin replacement through the GUI is untested; locked-file refusal is measured and stop-before-replacement is based on host source inspection.
- QML bytes / struct / settings pages remain unfinished. Not every calculation edge, Windows version or hardware path was rerun.
- Append cloud build / release outcomes after completion; local checks are not cloud CI.
