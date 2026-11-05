# Changelog

All notable changes to this plugin. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). The version in `plugin.json` is the
single source of truth; release tags are `v<version>`. Design decisions and
future phases live in [ROADMAP.md](ROADMAP.md).

## [Unreleased]

### Changed
- `README.md` is now the Chinese (default) documentation; `README.en.md` holds
  the full English text. Both link to each other. The English install steps now
  use Win顺's real English UI labels. No runtime change.

## [0.3.0] - 2026-11-05

First public release. Versions 0.1.0 and 0.2.0 were developed locally and
never published; they are listed below for context.

### Added
- Unified workbench window: mode tabs, an expression line evaluated live,
  clickable per-base copy chips (HEX / OCT / BIN / DEC / all) and a status bar.
- A width / signedness toolbar (`8|16|32|64`, `u|i`) above the bit grid; the
  grid widens to fill the window.
- Five top-bar tabs: bits / bytes / struct, plus Settings and Help as real
  content panels (two-column layout, no floating overlays).
- A dedicated first result row whose `Enter` opens the workbench; every base
  row below keeps `Enter` = copy.
- A two-row status bar: four labelled clickable parity chips (ones, parity,
  even-bit, odd-bit) with hover definitions, and full untruncated parse
  errors.
- Typing `bits ` (keyword plus a space, no expression) returns a valid first
  row whose Enter opens the workbench with a blank expression at the default
  width. The three example rows remain below it as autocomplete suggestions.
  `bytes ` / `struct ` keep the example-only behaviour for now.
- A grey hint is painted inside the empty expression input. The native
  `EM_SETCUEBANNER` is ignored without visual styles, so the subclassed EDIT
  draws it on `WM_PAINT`.

### Changed
- Plugin id is now `ra9fael.binarylab` (was `renfei.binarylab`) and the author
  is `ra9fael`. Settings from a local `renfei.binarylab` install do not carry
  over.
- Settings and Help replace the content area instead of covering it, so a tab
  click is never swallowed by an open panel.
- Host settings in the Settings tab are read-only: the SDK provides no setter.
- The `bits` command hint in `plugin.json` mentions that Enter opens the
  workbench.

### Fixed
- The BIN and DEC copy chips copied each other's value.
- The empty-keyword state returned only `valid: false` example rows; the host
  refuses actions on invalid rows, so Enter did nothing.
- `openEditor` now honours an explicit empty `data.expression` instead of
  falling back to the row's copy value, and tolerates `text` that still
  includes the command keyword.

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
