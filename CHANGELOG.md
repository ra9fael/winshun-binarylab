# Changelog

All notable changes to this plugin. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). The version in `plugin.json` is the
single source of truth; release tags are `v<version>`. Design decisions and
future phases live in [ROADMAP.md](ROADMAP.md).

## [Unreleased]

### Added
- Full-precedence integer expressions in `bits` and in the workbench
  expression line: unary `~ + -`, right-associative `**`, `* / %`, `+ -`,
  `<< >> rol ror`, `& ^ |`, and parentheses (`u8 1 + 2 * 3` = 7,
  `u8 (1 + 2) * 3` = 9, `u8 1 << 2 + 1` = 8). The classic three-token form
  keeps its strict per-literal range checks.
- Two evaluation modes behind the new `evalMode` setting and a session toggle
  in the workbench settings page: `fixed` (default; every step is masked to
  the width and wrapping is reported in the detail) and `c` (C integer
  promotions; the result type is derived and shown in the status bar; signed
  overflow is `E_RANGE`, an out-of-range shift count is `E_SHIFT`). An
  explicit type prefix always evaluates at that fixed width.
- Bash-style editing keys in the expression line: Ctrl+A / E (home / end),
  Ctrl+U / K (kill to start / end), Ctrl+W and Ctrl+Backspace (word kill),
  Ctrl+Y (yank), Ctrl+L (clear), Esc (clear, then close).
- Syntax errors name the offending token (`... near '('`).
- The workbench focuses the expression box when the window opens.
- IEEE-754 binary16: `f16 1.5` = `0x3E00`, round-half-to-even, and `f16` words
  in `bytes`. Float containers come from one table (`FloatSpec`), so the
  encoder, FLOAT text, S/E/M breakdown and special-value labels share it.
  Values that leave their container say so (`+Inf` / `0` notes).
- Float composition bar in the workbench: a top-bar pill opens a fixed 34 px
  row of proportional S | E | M blocks under the grid, each copying its field;
  bar and grid share one hover highlight; integer results at 16 / 32 / 64 bits
  get a dimmed ghost reading, 8 bits a grey band. New `floatBar` setting
  (`off` / `on`) picks the initial state.

- Float on the workbench toolbar: the sign row becomes a type row
  `unsigned u | signed i | float f`; in float mode the width buttons read
  `f16 / f32 / f64` (8 disabled). Containers switch by re-encoding the value,
  integer <-> float switches keep the bits.

### Changed
- **Behaviour change:** a plain decimal under a float prefix is encoded as a
  value: `f32 88` = `0x42B00000` (was the raw pattern `0x58`), `f16 65504`
  = `0x7BFF` (was `0xFFE0`). Raw patterns use `0x`/`0b`/`0o` or `@`.
  Operands of bit operations are unchanged.
- A long status-bar message takes the width it measures instead of being
  clipped at its left edge.
- Unimplemented float spellings (`f128`, `bf16`, `f8`, ...) now fail with
  `E_FLOAT_TYPE` naming `f16 / f32 / f64` instead of "invalid digit".
- Bare negative literals now wrap to the type instead of erroring
  (`u8 -1` = `0xFF`, `i8 -129` = `0x7F`), consistent with the fixed-width
  rule that every step stays inside the width and is reported.
- The workbench keeps the last good result when a new expression fails to
  evaluate. The status bar shows the derived result type (prefixed with
  `C · ` in C mode) and the width / signedness toolbar is disabled in C mode,
  where the type comes from the expression.
- `README.md` is now the Chinese (default) documentation; `README.en.md` holds
  the full English text. Both link to each other. The English install steps now
  use Win顺's real English UI labels. No runtime change.

### Fixed
- The caret in the workbench expression line sat above the characters. A
  single-line EDIT centres its glyphs in a client area taller than the text
  line but pins the caret to the top of the line box, so the previous
  "line height + 4 px" control left the bar 2 px high; the control is now
  exactly one measured text line tall, so caret and glyphs share one box and
  the band centring positions both.

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
- Expression history: `Enter` re-evaluates and submits a confirmed expression,
  `Up`/`Down` browse it and the `▾` button opens a dropdown of the last 12
  entries. History is JSONL in the host `dataDir`, capped at 200 entries with
  adjacent duplicates removed; the Settings tab shows its state and offers
  "Clear history". Deliberately not a sixth tab: history is an input assistant
  and must stay usable alongside the bit grid. Live `EN_CHANGE` evaluation
  remains, so typing never needs Enter and the history records only intentional
  expressions.
- Pure-ASCII unit suffixes on decimal literals: SI `k/K/M/G/T` (10^n), IEC
  `Ki/Mi/Gi/Ti` (2^n), byte spellings `KB/MB/GB` (SI 1000) and
  `KiB/MiB/GiB/TiB` (IEC), plus fractional `m/u/micro/n/p`. `u` means micro
  (10^-6), not C's unsigned suffix; `µ`/`μ` produce `E_BAD_UNIT` with an
  ASCII hint. Suffixes are never stripped from `0x`/`0b`/`0o` literals or from
  bit indices, and a detached suffix (`4 MB`) gets `E_UNIT_ATTACH`.
- IEEE-754 bit-pattern literals: `1.5`, `.5`, `1e6`, `1_000.25`, `Inf`,
  `-Inf`, `NaN`, with `f32`/`f64` type prefixes. The result is the encoded bit
  pattern; a FLOAT chip shows the shortest round-tripping decimal text, status
  row 1 names the float type, and the bit grid carries a purple/blue/green
  S/E/M band. No float arithmetic: a float literal must stand alone
  (`E_FLOAT_OP`), and an integer prefix cannot take one (`E_FLOAT_TYPE`).
- Unit conversion notes (`4Mi = 4 x 2^20 = 4194304`) in the result detail,
  with a compact form (`4Mi -> 4194304`) in the status bar.

### Changed
- Plugin id is now `ra9fael.binarylab` (was `renfei.binarylab`) and the author
  is `ra9fael`. Settings from a local `renfei.binarylab` install do not carry
  over.
- Settings and Help replace the content area instead of covering it, so a tab
  click is never swallowed by an open panel.
- Host settings in the Settings tab are read-only: the SDK provides no setter.
- The `bits` command hint in `plugin.json` mentions that Enter opens the
  workbench.
- The expression EDIT is 20px instead of 24px with an 8px left inset, so its
  text is vertically centred and left-aligned; the freed 40px hosts the
  history dropdown button. The empty-input hint now also advertises units and
  floats.

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
