# GDI / QML branches and distribution

[简体中文](variants-design.md)

> **Status (2026-11-15):** QML is the only maintained edition; GDI ends at 0.3.3 and
> the `gdi` branch is archived. This document keeps the original variant design and
> migration rationale as history. Current release rules are in README and
> CONTRIBUTING: tag `qml-vX.Y.Z`, package `<id>-<version>.wsplugin`, published as Latest.

## Verdict

One repository, long-lived `gdi` / `qml` branches and the shared ID `ra9fael.binarylab`. Installing either edition replaces the other. Names, tags and filenames identify the edition. Approved by the user on 2026-11-09.

## Grounded facts

Source inspection: `PluginSettings::installFrom` asks by ID, `updates::replace` stops the old process, and `install::place` replaces the entire directory with rollback on copy failure. Data lives separately. Only the QML bits window is implemented; the other pages are placeholders.

## Architecture and core logic

GDI is a static-CRT executable. QML uses a static-CRT launcher, `BinaryLabApp.dll` and host Qt 6.12.0 (same major/minor, minimum patch). Pin the WinShun controls source commit. Keep the calculation core pure C++; port shared fixes as separate commits.

`release.json` declares variant, prerelease and QML build dependencies. `plugin.json` owns the version and runtime contract. Only QML is published now: tag `qml-vX.Y.Z`, package `<id>-<version>.wsplugin` (the variant segment is gone since 0.5.0).

## Interaction and icons

Names identify GDI / QML; the QML window says Preview. The same icon supplies the manifest, executable, windows and captions. Eight ICO sizes and matching Qt PNGs. Command IDs, keywords and action IDs remain stable.

## Permissions and data

No new permissions. Keep settings, shortcuts and separate data under the same ID. Share the history JSONL format and back up before future breaking migrations. Development links cannot be replaced. Updates are manually selected; a future feed must distinguish editions.

## Failure matrix

| Condition | Handling |
|---|---|
| Wrong variant or version tag | Reject before writing a package |
| Missing QML DLL, icon or translated release notes | Reject before writing |
| Stale QML DLL beside GDI build | Excluded by explicit runtime file list |
| Development link target | Host refuses replacement |
| Copy failure during replacement | Host restores the old directory |
| Incompatible host Qt | Host preflight rejects startup |
| Incomplete QML pages | Document as preview, never stable |

## Deliverables

Two source branches, shared packaging and workflow, bilingual documentation, matching icons, packages and SHA256. QML remains a prerelease and never becomes GitHub latest. GitHub CLI operations are serial, with no frequent polling.

## Tests L0–L5

- L0: manifest, tags, runtime files, icon resources, deterministic archives and checksums.
- L1: existing logic tests and QML expression specification tests.
- L2: packaging failure cases and protocol initialize/query/action/shutdown.
- L3: actual host loading; production installer checks for both directions, downgrade and data retention.
- L4: isolated window screenshots, large/small icons and DPI; record uncovered paths.
- L5: clean commit builds, CI and Release asset verification.

## Honest notes and delivery report

Source inspection is not an installation test. Record actual results and limitations in `docs/variant-validation.en.md`. Finishing every QML workbench page is outside this preview distribution task.
