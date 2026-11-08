# Changelog

[简体中文](CHANGELOG.md)

## [Unreleased]

## [0.4.0] - 2026-11-09

### Changed
- Separate `gdi` / `qml` branches, tags and packages; a shared plugin ID enables replacement installs.
- One multi-resolution icon across the plugin list, executable and windows.
- Validate version, bilingual notes and required binaries before packaging; exclude stale build files.
- Build into an isolated output folder; CI builds the configured edition and publishes a matching tag with SHA256.
- QML remains a preview. Add executable, Qt application and caption icons; require BinaryLabApp.dll. Pin Qt 6.12.0 and the WinShun controls commit.
- Known limitations: bytes / struct / settings window pages remain placeholders; query engines work.
- Full pre-split development notes remain in [the archive](docs/development-history-0.4.0.en.md); they are not a QML page completion checklist.
