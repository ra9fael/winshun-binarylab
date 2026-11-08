# Icon fix 0.3.1

[中文](icon-validation.md)

## Problem

Embedding an ICO alone can leave native windows using a generic icon.

## Cause

Executable resources do not assign window icons. IDI_ICON1 is a string resource name. Qt windows need an explicit application icon resource.

## Changes

Embed the executable icon and include the ICO and resource script in the package. Assign large and small window icons explicitly, refresh them for DPI changes and release owned handles. Include the existing workspace fix for MSVC iswalnum compilation.

## Measured

2026-11-09: built a committed snapshot with MSVC 19.51 / x64 Release; CTest 1/1 passed. Resource and real plugin process checks: 54/54 passed (docs/icon-checks.json), including the named resource, eight sizes, protocol initialization and clean shutdown. Actual large/small window pixels match app.ico. Checks at 96/120/144/192 DPI and 60 repeated changes passed with bounded GDI/USER handles. Query and Enter opened the window in an isolated production WinShun profile; icons passed and the host exited normally.

## Not tested

Physical taskbar/Alt+Tab interaction, every plugin feature, hardware paths, dragging across actual monitors, UAC secure desktop and all Windows versions were not retested. This run creates local tags and packages only; nothing is published.
