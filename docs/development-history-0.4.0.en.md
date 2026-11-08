# Archived 0.4.0 development notes

[简体中文](development-history-0.4.0.md)

These pre-split notes also describe the inherited GDI workbench. They are historical records, not a QML completion checklist. Only the QML bits page is implemented; see README for current limitations.

## [0.4.0] - 2026-11-07

### Added
- An experimental QML workbench prototype on the `qml` branch: `binarylab.exe`
  becomes a Qt-free stub that loads `BinaryLabApp.dll`, built on Win顺's Qt 6,
  which serves the plugin protocol on a worker thread and runs the QML window
  on the GUI thread (the same split as Win顺's own capture plugin). The
  prototype reuses Win顺's shared QML controls (Theme, TitleBar, InputBox,
  FlatButton) and covers the bits page; the byte, struct, settings and help
  pages are placeholders until their port slices land.
  `binarylab.exe --bits [--app-dir <folder>] [expression]` opens the prototype
  without the host. Building the branch needs a Qt 6, found through
  `WINSHUN_DIR` (a WinShun checkout, default `../WinShun`).
- Distribution through Win顺's in-app plugin library as an **unreviewed
  native package**: `minWinShun` is now `0.22.0` and the manifest declares
  Win顺's shared Qt contract (`runtime.qt: "host"`, `qtVersion: "6.12"`),
  so the host sets up the Qt DLL paths and QML import paths for the stub and
  the package carries no Qt runtime of its own. Win顺 asks for confirmation
  on installation and on every update. The reused Win顺 controls also touch
  `QtQuick.Shapes` and `QtQuick.Effects`, which sit outside the contract's
  guaranteed set — Win顺's own UI depends on both, so they ship with the host.
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
  Ctrl+Y (yank), Ctrl+L (clear), Esc (clear, then close). The history
  dropdown deletes single entries (✕ per row) as well as clearing all.
- Syntax errors name the offending token (`... near '('`).
- The workbench focuses the expression box when the window opens.
- IEEE-754 binary16: `f16 1.5` = `0x3E00`, round-half-to-even, and `f16` words
  in `bytes`. Float containers come from one table (`FloatSpec`), so the
  encoder, FLOAT text, S/E/M breakdown and special-value labels share it.
  Values that leave their container say so (`+Inf` / `0` notes).
- Float composition bar in the workbench: in a float reading a row of
  proportional S | E | M blocks stays open under the grid, each copying its
  field; set bits wear their field's colour and the hovered field's bits
  lighten; bar and grid share one hover highlight. Integer results show no bar
  by default; the new `floatBar` setting (`off` / `on`) opts integer mode into
  a dimmed ghost reading at 16 / 32 / 64 bits, 8 bits a grey band.

- Float on the workbench toolbar: the sign row becomes a type row
  `unsigned u | signed i | float f`; in float mode the width buttons read
  `f16 / f32 / f64` (8 disabled). Containers switch by re-encoding the value,
  integer <-> float switches keep the bits.
- Byte workbench page (the second content tab): a scalar mode with an
  offset / hex / ASCII grid — click, drag, arrow keys and wheel select bytes
  with scroll following; the active word decodes per the expression's type
  token and endianness into HEX / DEC|FLOAT / BIN chips; toolbar and LE / BE
  buttons rewrite the type token and the `le` / `be` suffix; Ctrl+C copies the
  selection as contiguous hex; "→ Bits" hands the active word to the bits page.
- `bytes fmt` record decoding: `bytes fmt` lists formats and
  `bytes fmt <name> [lsb|msb] <hex...>` decodes hextract TOML formats —
  records cut per `word_bits`, fields bit-sliced in declaration order
  (arrays expanded per element), rule hits tinted. Two BLM formats are built
  in (`blm_rawdata_256`, `blm_interlock_512`); more load from the new
  `formatDir` folder setting (up to 64 `*.toml` files, read-only, behind the
  new `files:read` permission) via the vendored toml++ v3.4.0 with a
  schema-checked reader that reports file line numbers. A restricted
  expression evaluator runs the rule `when` subset with hard caps
  (256 AST nodes, depth 32, 4096 steps per record).
- Records mode in the byte workbench: format dropdown, records table with
  column layout, record / field selection with a value chip, LSB / MSB order
  buttons that rewrite the expression, Shift+wheel horizontal column scroll.
- Blank workbench entries for every content tab: `bytes ` and `struct ` (the
  keyword plus a space) now show a valid first row that opens the bytes /
  struct tab with an empty expression box, matching `bits `; a plain `struct`
  query also gains a dedicated `struct.open.workbench` entry row.
- The bytes expression accepts the hex keyword being omitted: a leading type
  token (`u32 0x88775545`) or bare hex data (`deadbeefdeadbeef`, run together)
  is treated as hex, defaulting to `u8` when no type is given.

### Changed
- The byte workbench hands-off button is now labelled「送到位运算页」/ "Send to
  Bits" so its purpose is explicit; the bytes-page placeholder and empty-state
  text name the bare-hex and `fmt <name> <hex...>` forms.
- The float-bar pill (layout, drawing, click) exists on the bits page only.
- The HEX / DEC|FLOAT / BIN chips and the type buttons on the byte workbench
  are measured from their worst-case labels (u64 bits, localized button text),
  wrap the BIN chip to a second row when the window is narrow, and feed the
  minimum window width, so no value is clipped any more.
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
- The workbench could die with an access violation or a fail-fast while it
  was being used. The manifest asked the strict sandbox for a 128 MB commit
  budget, but the Qt Quick workbench itself sits at ~145 MB before the user
  types anything, so the renderer ran out of memory on the first resize and
  the window went blank or took the process down with it. The budget is now
  512 MB (the host's default), the window no longer refits on every content
  change but after a short pause, and the bit grid reads one segment at a
  time instead of materialising a whole float breakdown per mouse move.
- The caret in the workbench expression line sat above the characters. A
  single-line EDIT centres its glyphs in a client area taller than the text
  line but pins the caret to the top of the line box, so the previous
  "line height + 4 px" control left the bar 2 px high; the control is now
  exactly one measured text line tall, so caret and glyphs share one box and
  the band centring positions both.

