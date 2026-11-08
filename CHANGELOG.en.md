# Changelog

[中文](CHANGELOG.md)

## [Unreleased]

## [0.3.3] - 2026-11-09

### Fixed

- Require WinShun 0.22.0 for installation consent for unreviewed native plugins.
- Keep the GDI edition independent of Qt, with new-tag and manual release entry points and the existing logic and packaging tests.

## [0.3.2] - 2026-11-09

### Changed
- Separate `gdi` / `qml` branches, tags and packages; a shared plugin ID enables replacement installs.
- One multi-resolution icon across the plugin list, executable and windows.
- Validate version, bilingual notes and required binaries before packaging; exclude stale build files.
- Build into an isolated output folder; CI builds the configured edition and publishes a matching tag with SHA256.
- GDI remains independent of Qt; bump to 0.3.2.



## [0.3.1] - 2026-11-09

### Fixed

- Embed the executable icon and include the ICO and resource script in the package. Assign large and small window icons explicitly, refresh them for DPI changes and release owned handles. Include the existing workspace fix for MSVC iswalnum compilation.

### Notes

- Local tag and package only. See docs/icon-validation.en.md for the measured coverage; no GitHub release. Restart the plugin process after upgrading.
