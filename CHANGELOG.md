# Changelog

[English](CHANGELOG.en.md)

All notable changes to this plugin. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). The version in `plugin.json` is the
single source of truth; release tags are `<variant>-v<version>`. Design decisions and
future phases live in [ROADMAP.md](ROADMAP.md).

## [Unreleased]

## [0.4.0] - 2026-11-09

### 变更
- 整理 `gdi` / `qml` 双分支，相同插件 ID 实现替换安装；包名和标签区分版本类型。
- 插件管理列表和程序窗口使用统一多尺寸图标。
- 打包校验版本、双语说明和必需运行文件，排除构建残留；独立目录构建避免覆盖运行中的程序。
- CI 按分支配置构建，标签匹配后发布 `.wsplugin` 和 SHA256。
- QML 标记为预览版；补齐 EXE、Qt 应用和标题栏图标，要求 BinaryLabApp.dll。固定 Qt 6.12.0 和 WinShun 共享控件源码提交。
- 已知限制：bytes / struct / 设置窗口页仍是占位页；查询计算核心可用。
- 拆分前的完整开发记录保留在 [历史记录](docs/development-history-0.4.0.md)，不作为 QML 页面完成清单。


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

[Unreleased]: https://github.com/ra9fael/winshun-binarylab/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/ra9fael/winshun-binarylab/releases/tag/v0.4.0
[0.3.0]: https://github.com/ra9fael/winshun-binarylab/releases/tag/v0.3.0
