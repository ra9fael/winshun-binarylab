# Changelog

[简体中文](CHANGELOG.md)

## [Unreleased]

## [0.5.0] - 2026-11-15

### Changed
- QML is now the only maintained edition. The GDI line ends at 0.3.3 and its branch is archived.
- The package is named `ra9fael.binarylab-<version>.wsplugin` (no `-qml-`) because the plugin-library index accepts only `<id>-<version>.wsplugin`; the library entry now points here, so updates stay on QML.
- QML releases are no longer Pre-release and become the repository's Latest release.
- README, both changelogs and the variant design notes describe the single maintained line, including the GDI upgrade and downgrade paths.

### Notes
- The plugin ID, settings, shortcuts and the `history.jsonl` format do not change. 0.5.0 replaces GDI 0.3.3 in place and keeps its data.
- On hosts below 0.22.0 the manifest's `minWinShun` blocks the update, so they keep running GDI 0.3.3 instead of installing a package they cannot run.

## [0.4.0] - 2026-11-09

### Changed
- Separate `gdi` / `qml` branches, tags and packages; a shared plugin ID enables replacement installs.
- One multi-resolution icon across the plugin list, executable and windows.
- Validate version, bilingual notes and required binaries before packaging; exclude stale build files.
- Build into an isolated output folder; CI builds the configured edition and publishes a matching tag with SHA256.
- QML remains a preview. Add executable, Qt application and caption icons; require BinaryLabApp.dll. Pin Qt 6.12.0 and the WinShun controls commit.
- Known limitations: bytes / struct / settings window pages remain placeholders; query engines work.
- Full pre-split development notes remain in [the archive](docs/development-history-0.4.0.en.md); they are not a QML page completion checklist.
