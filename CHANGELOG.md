# Changelog

All notable changes to this plugin. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). The version in `plugin.json` is the
single source of truth; release tags are `v<version>`. Design decisions and
future phases live in [ROADMAP.md](ROADMAP.md).

## [Unreleased]

## [0.3.0] - 2026-11-05

First public release. Versions 0.1.0 and 0.2.0 were developed locally and
never published; they are listed below for context.

### Added
- Unified workbench window: mode tabs, an expression line evaluated live,
  clickable per-base copy chips (HEX / OCT / BIN / DEC / all) and a status bar.
- A width / signedness toolbar (`8|16|32|64`, `u|i`) above the bit grid; the
  grid widens to fill the window.
- Live popcount and even-parity bit in the status bar.

### Changed
- Plugin id is now `ra9fael.binarylab` (was `renfei.binarylab`) and the author
  is `ra9fael`. Settings from a local `renfei.binarylab` install do not carry
  over.

### Fixed
- The BIN and DEC copy chips copied each other's value.

### Known limitations
- The bytes and struct tabs of the workbench are placeholders; use the `bytes`
  and `struct` keywords in the launcher. They are planned for 0.3b / 0.3c.

## [0.2.0] - 2026-11-05 (not published)

### Added
- Interactive bit editor window (click and drag-paint bits) under the strict
  sandbox; `ui` and `clipboard:write` permissions.
- `bitsEnter` setting: Enter = copy and Ctrl+Enter = editor by default.

### Fixed
- Complete CJK text in the editor (per-language fonts, measured sizes).
- The process stays alive while the editor window is open.

## [0.1.0] - 2026-11-05 (not published)

### Added
- `bits`, `bytes` and `struct` commands: bit operations and base conversion,
  hex / bin / oct byte-stream decoding, and C-style struct layout with
  `#pragma pack`.

[Unreleased]: https://github.com/ra9fael/winshun-binarylab/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/ra9fael/winshun-binarylab/releases/tag/v0.3.0
