# Binary Lab (`ra9fael.binarylab`)

> **English** · [简体中文（默认）](README.md)
>
> Win顺 ships an English UI, but the default documentation is Chinese.
> `README.md` is the Chinese original; this file is its English counterpart.
> The two are kept in sync and link to each other.

A WinShun resident plugin for embedded work: parse `hex` / `bin` / `oct` strings,
visualize bit operations, and compute C-like struct layout. Everything is a
bounded, pure in-memory computation. The result rows and `preview.text` are
rendered by the host, and the optional **interactive bit editor** opens its own
window (only when you trigger it) so you can click bits, drag-paint, and copy
the edited value in any base.

## Install

1. Download `ra9fael.binarylab-<version>.wsplugin` from the
   [latest release](https://github.com/ra9fael/winshun-binarylab/releases/latest).
   Optionally compare its SHA-256 with the `.sha256` file next to it
   (`Get-FileHash <file> -Algorithm SHA256`).
2. In Win顺 open **Settings → Plugins → Installed → Add plugin →
   Archive (.zip / .wsplugin)** and pick the file. In the Chinese UI the same
   path reads **设置 → 插件 → 已安装 → 添加插件 → 压缩包（.zip / .wsplugin）**.
3. A plugin installed from a local package is **not enabled automatically**.
   Turn it on in the list; enabling it is how you confirm its permissions
   (see [Permissions](#permissions-and-data)).

**Update:** install the newer `.wsplugin` the same way. Win顺 sees the same id,
asks before replacing the old version, and keeps the plugin's data folder.

**Uninstall:** remove it from **Settings → Plugins → Installed**
(**设置 → 插件 → 已安装** in the Chinese UI).

This plugin is a native exe, so it is not listed in Win顺's in-app plugin
library (the community index accepts script plugins only). The releases of this
repository are the only distribution channel.

## Build from source

Requirements: Windows 10/11 x64, CMake 3.21+, and either Visual Studio 2022 or
later (MSVC) or MinGW-w64 GCC 13+. Python 3.10+ for packaging and the
integration tests.

```powershell
cmake -S . -B build -A x64                 # MSVC; for MinGW use -G "MinGW Makefiles"
cmake --build build --config Release       # writes binarylab.exe next to plugin.json
ctest --test-dir build -C Release --output-on-failure
python tools/package.py                    # dist/ra9fael.binarylab-<version>.wsplugin
```

To run a local build inside Win顺 without packaging, and for tests and the
release process, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Commands

| Keyword | Example | What it does |
|---|---|---|
| `bits` | `bits u8 0xA5 & 0x0F` | HEX / BIN / OCT / DEC rows, `~ & \| ^ << >> rol ror`, field `get/set/clear/toggle`, bit grid in the hover preview |
| `bytes` | `bytes hex u16le 34 12 78 56` | Normalize a byte stream, decode words by type and endianness, hexdump with offset and ASCII, IEEE-754 breakdown for floats |
| `struct` | `struct pack=4 struct Frame { u8 sof; u32 seq; u16 len; u8 data[3]; };` | `sizeof` / `alignof`, member offsets, padding rows, a byte map, and copyable `static_assert` text |

### `bits` syntax

```
bits [u8|u16|u32|u64|i8|i16|i32|i64|f16|f32|f64] EXPRESSION
     operators: ~ + - (unary) · ** · * / % · + - · << >> rol ror · & ^ | · parentheses
     raw bits : @0xFF          (reinterpret a bit pattern, e.g. i8 @0xFF = -1)
     fields   : get|set|clear|toggle VALUE HIGH:LOW [VALUE]
     literals : 0x… 0b… 0o… decimal / float (1.5, 1e6, Inf, NaN), _ separators, unit suffixes
```

Expressions evaluate with C-style precedence, tightest first: unary → `**`
(right-associative) → `* / %` → `+ -` → shifts/rotates → `&` → `^` → `|`,
with arbitrary parentheses; the classic three-token form
(`bits u8 0xA5 & 0x0F`) still works. There are two evaluation modes (the
`evalMode` setting; the workbench settings page can switch it per session):

- **fixed width** (default): every step is masked to the width and wrapping is
  reported in the detail.
- **C mode**: integer promotions follow C, the result type is derived and
  shown in the status bar; signed overflow gives `E_RANGE` and an
  out-of-range shift count gives `E_SHIFT`.

An explicit type prefix always evaluates at that fixed width (prefix wins).
Unsigned right shift is logical, signed right shift is arithmetic, and
`rol` / `ror` reduce the count modulo the width. A shift count `>= width` is
an error; rotations are not. Literals are range-checked against the width and
are never silently truncated.

### `bytes` syntax

```
bytes [hex|bin|oct] [TYPE] DATA...
TYPE : u8 i8 u16[le|be] i16[le|be] u32[le|be] i32[le|be]
       u64[le|be] i64[le|be] f16[le|be] f32[le|be] f64[le|be]
```

`u16` / `u32` / `u64` / `f32` / `f64` without a suffix use the `defaultEndian`
setting; an explicit `le` / `be` always wins. Separators (`space , : ; _ -`)
and `0x` / `0b` / `0o` prefixes are accepted. A compact hex token must have an
even number of digits, a `bin` token exactly 8 bits, an `oct` token `000..377`.
Input is capped at 16 KiB and 4096 decoded bytes. A trailing incomplete word is
reported as such and is never given a fabricated value.

### `struct` syntax

```
struct [pack=0|1|2|4|8|16] struct Name { MEMBER; ... };
MEMBER : TYPE name [N];
TYPE   : u8 i8 u16 i32 u64 f32 f64 | a previously defined named type
       | struct [Name] { ... } name;   |  union [Name] { ... } name;
       | struct Name name;             (reference to an earlier definition)
```

`pack=0` means natural alignment; the last definition in the query is the one
laid out. Layout uses `align_up(offset, min(natural_align, pack))` for members
and pads the tail to the struct's effective alignment. Bitfields, `int`,
`long`, pointers, `size_t`, empty aggregates, duplicate member names,
forward references and recursive types are rejected rather than guessed: those
are ABI-dependent and would produce a confidently wrong answer.

## Actions

Actions are declared in a fixed order; **order**, not the `key` string, decides
the keyboard mapping. For `bits` rows the editor is available, so `copyPrimary`
and `openEditor` share Enter / Ctrl+Enter and `copyDetail` is reached from the
host's action menu. The split is chosen by the `bitsEnter` setting.

| Command | Enter | Ctrl+Enter | Shift+Enter | Menu (no key) |
|---|---|---|---|---|
| `bits` (default `bitsEnter=copy`) | `copyPrimary` — copy the row's main value | `openEditor` — open the interactive bit editor | `chainQuery` — fill the input box with a follow-up | `copyDetail` — copy full text |
| `bits` (`bitsEnter=editor`) | `openEditor` | `copyPrimary` | `chainQuery` | `copyDetail` |
| `bytes` / `struct` | `copyPrimary` (struct summary expands members) | `copyDetail` | `chainQuery` | — |

Exception: the **first result row** is a dedicated workbench entry. Regardless
of `bitsEnter`, its Enter is `openEditor` and Ctrl+Enter is `copyPrimary`.
Typing just `bits ` (keyword plus a space) produces the same first row, so
Enter opens a blank workbench immediately.

On a `struct` summary row Enter expands into one row per member/padding via
`effects.replace` instead of copying.

### Interactive bit editor

Triggered with Enter on the first result row; typing just `bits ` and pressing
Enter works too and opens a blank expression box at `defaultWidth`. The other
base rows follow their own mapping (Ctrl+Enter by default, or Enter when
`bitsEnter=editor`). The window is owned by the plugin and runs under the same
strict sandbox as the rest of the plugin. Inside it:

| Input | Effect |
|---|---|
| Click a bit cell | Toggle that bit |
| Drag across cells | Paint them to the value of the first cell |
| Arrow keys | Move the focus cell (Left/Right ±1, Up/Down ±8); the focused cell shows a yellow border |
| Space | Toggle the focused bit |
| Ctrl+C / B / D / O | Copy the value as HEX / BIN / DEC / OCT to the clipboard |
| Enter (in the expression box) | Re-evaluate and submit the expression to history |
| Up / Down | Browse submitted history; Down past the newest restores the draft |
| `▾` at the right of the expression row | Open the last 12 entries; click one to recall and evaluate |
| Esc | Close (closes the dropdown first if it is open) |

The result chips show the four bases live as you edit. The window lives on its
own GUI thread; WinShun tears it down through the plugin destructor on
shutdown. The expression box is focused when the window opens. The settings
page also has a session-local eval-mode (fixed / C) toggle; in C mode the
width and signedness toolbar is disabled because the type comes from the
expression.

#### Unit suffixes (pure ASCII)

| Family | Prefixes | Multiplier | Example |
|---|---|---|---|
| SI decimal | `k`/`K`, `M`, `G`, `T` | 10^3 / 10^6 / 10^9 / 10^12 | `4M` = 4,000,000 |
| IEC binary | `Ki`, `Mi`, `Gi`, `Ti` | 2^10 / 2^20 / 2^30 / 2^40 | `4Mi` = 4,194,304 |
| Byte spellings | `KB`/`MB`/`GB`, `KiB`/`MiB`/`GiB`/`TiB` | Follow the two rows above | `4MB` = 4,000,000, `4MiB` = 4,194,304 |
| SI fractional | `m`, `u`/`micro`, `n`, `p` | 10^-3 / 10^-6 / 10^-9 / 10^-12 | `1u` = 10^-6 |

- A suffix must touch the number and is stripped only from decimal literals:
  the `B` in `0xAB` and the letters in `0xDEADBEEF` stay valid hex digits.
  `E` is reserved for exponents (`1E6` = 1e6); `M` and `m` are distinct.
- `u` is micro (10^-6), **not** C's unsigned suffix; `micro` and `1e-6` are
  equivalent. Typing `µ` or `μ` yields `E_BAD_UNIT` with a hint naming those
  three ASCII spellings.
- Bit indices and field values reject units (`get 0xA5 7K:4` gives
  `E_UNIT_ON_INDEX`); a space between number and suffix gives `E_UNIT_ATTACH`.
- Conversions are never silent: detail shows
  `4Mi = 4 x 2^20 = 4194304`, the status bar shows `4Mi -> 4194304`, and a
  value outside the width still fails with `E_RANGE`.

#### Float bit patterns

`1.5`, `.5`, `1e6`, `1_000.25`, `Inf`, `-Inf` and `NaN` encode into IEEE-754
bit patterns: `f64` (binary64) by default, `f32` for binary32 and `f16` for
binary16 (round-half-to-even; the detail notes when a value leaves the
container and becomes ±Inf or 0). All three containers come from one table;
unimplemented spellings such as `f128` or `bf16` give `E_FLOAT_TYPE` instead of
a misleading digit error. HEX / OCT / BIN
show the encoded bits, the FLOAT chip (replacing DEC in float mode) shows the
shortest round-tripping decimal text, and a purple / blue / green band marks
S / E / M at the top of each bit cell.

**Float bar**: the "float bar" pill at the right end of the top bar opens a
row under the grid with proportional S | E | M blocks. Each block shows its
bit count and raw field value and copies that field on click (e.g. `0x7F`).
Pointing at a block puts its bit range, unbiased exponent and so on in the
caption line and highlights its bits in the grid; pointing at a bit lights its
block. An integer result at width 16 / 32 / 64 shows a dimmed "ghost-read as
f16/f32/f64" bar; 8 bits has no container and shows a grey band with a hint.
The window grows by 34 px only when the pill is switched, never because of the
result type. The `floatBar` setting picks the initial state.

**Float on the toolbar**: the "Type" row next to the widths reads
`unsigned u | signed i | float f`. In float mode the width buttons read
`f16 / f32 / f64` and 8 is disabled (no container). Switching containers
re-encodes the **value** (`f32 1.5` → `f64 1.5`, rounding or range loss in the
detail); switching between integer and float keeps the **bits** and changes the
reading (`u32 0x3FC00000` ⇄ `f32 0x3FC00000`, the float bar's ghost reading).
A signed reading is written `i32 @0x…`, because a hex literal with the top bit
set is out of signed range and `@` marks raw bits.

**A plain decimal under a float prefix is a value**: `f32 88` = 88.0 =
`0x42B00000`, `f16 65504` = `0x7BFF`. Write raw patterns as `0x…` / `0b…` /
`0o…` or `@88`. Operands of a bit operation are unchanged
(`f32 0x3FC00000 | 1` still ORs the pattern).

There is no float arithmetic: a float **literal** must stand alone
(`1.5 & 0xFF` gives `E_FLOAT_OP`, `u32 1.5` gives `E_FLOAT_TYPE`). To apply bit
operations to an encoded pattern, write the raw bits, e.g.
`f32 @0x3FC00000 & 0x007FFFFF`. Fractional units (`1m` / `1u`) take the same
path.

#### Expression history

Only expressions submitted with Enter and successfully evaluated are recorded;
live evaluation does not flood the list. History is appended as JSONL to the
host's `dataDir/history.jsonl`, capped at 200 entries with adjacent duplicates
removed. If the strict sandbox denies disk writes, it degrades to session-only
and the Settings tab says so. "Clear history" empties both memory and file.
There is deliberately **no sixth tab**: history is an input assistant and the
dropdown stays usable while the bit grid is visible.

Queries have no side effects: they never delete files, start programs, change
the clipboard, or open a window. Copy and `setQuery` effects are returned only
when the user triggers an action. With `bits ` and no arguments, the first row
is a **valid** "Open binary workbench" entry (Enter opens a blank workbench),
followed by three example rows (`valid: false`, Tab fills the example).
`bytes ` / `struct ` with no arguments still show only the examples. A parse
failure shows one `valid: false` row with an error code and a corrected example.

## Settings

| Key | Type | Default | Meaning |
|---|---|---|---|
| `defaultWidth` | choice | `32` | Width for `bits` when no type is written |
| `defaultEndian` | choice | `le` | Endianness for types without an explicit `le` / `be` |
| `defaultPack` | choice | `0` | `pack` for `struct` when `pack=N` is omitted |
| `bitsEnter` | choice | `copy` | Which of copy / editor takes Enter on `bits` rows. `copy` (default) = Enter copies, Ctrl+Enter opens the editor; `editor` swaps them. |
| `evalMode` | choice | `fixed` | Expression evaluation mode. `fixed` (default) keeps every step inside the width and reports wrapping; `c` applies C integer promotions and derives the result type. An explicit type prefix always evaluates fixed. |
| `floatBar` | choice | `off` | Whether the workbench opens with the float bar shown; the in-window pill toggles it for that window only. |

Settings are read from `initialize` and refreshed on `settingsChanged`; no
secrets are involved.

## Permissions and data

`runtime.sandbox` is `strict`. Measured with `wsbench`: the plugin process runs
at **LOW integrity, not elevated, inside a job object**. Since 0.2.0 the
manifest declares two permissions, both required by the interactive bit editor
and nothing else:

| Permission | Why | Enforceable? |
|---|---|---|
| `ui` | The editor window (created only when the user triggers the `openEditor` action). | Partially — without it the job blocks desktop/USER handles, so the window could not exist. |
| `clipboard:write` | The editor's copy button writes the edited value directly. The protocol has no plugin→host copy tool, so a host-mediated `copy` effect is not available from inside a window. | Yes. |

Text-only use needs neither: queries and all other actions keep zero side
effects (no files, no clipboard, no window). No `network`, `files:*`,
`clipboard:read`, `processes`, `notifications`, `elevated`, `contentSearch`,
`ai.expose` or `hostHotkeys` capability is declared.

The plugin writes nothing to disk and does not use `WS_DATA_DIR`. If presets
are added later, they belong in `WS_DATA_DIR` with atomic writes. Secrets must
use the host's `password` setting type; none are used here. No user data is
uploaded or sent anywhere: there are no network calls.

## Build and runtime dependencies

- Windows, WinShun with the C++ resident-plugin protocol (JSON-RPC 2.0, one
  UTF-8 message per line on stdio).
- A C++17 compiler. MSVC (static CRT, `/utf-8`) or MinGW-w64 (static
  `-static` link) both work. No Qt, no third-party libraries, no runtime DLL.
- `winshun.h` is copied byte-for-byte from `WinShun/src/plugins-sdk/winshun.h`
  so the plugin is self-contained.
- Build output goes straight into this folder because `plugin.json` names
  `binarylab.exe` as `runtime.program`. Build commands: [Build from source](#build-from-source).

## Verification

See [TESTPLAN.md](TESTPLAN.md) for the full matrix and the measured results.
Summary: 215/215 pure-logic assertions pass (including GCC `#pragma pack`
`sizeof` / `alignof` / `offsetof` oracles), and the built plugin was driven
through the real `WinShunPluginHost.exe` runner with `wsbench` for all three
commands, empty input, invalid input, settings overrides and all three
actions.

What has **not** been verified: the actual WinShun GUI message log and hover
panel, monospace alignment of `preview.text`, keyboard feel, and importing a packaged `.wsplugin` into a fresh profile.

## Failure handling

Parse errors, out-of-range literals, unsupported ABI constructs and size-cap
violations return a single non-actionable row with a stable error code and a
corrected example; no partial or invented result is returned. Missing
dependencies do not exist (there are none), and there is no network to fail.
All diagnostics go to stderr (`plugins.log`); stdout carries only JSON-RPC
lines. `timeoutMs` is 1500; the slowest measured query at the 4096-byte cap
was ~81 ms p95.

## Distribution and versioning

`runtime.program` is a native executable. The community repository's
`plugin.schema.json` restricts `runtime.program` to interpreters and an
official whitelist, and `CONTRIBUTING.md` excludes exe/DLL submissions, so
this plugin is distributed from its **own repository as a `.wsplugin` package**
that users import into WinShun (see [Install](#install)). Do not open a source
PR against `WinShun-plugins`. `tools/package.py` builds the package from the
git-tracked files plus the built `binarylab.exe`, so the `build/` tree, logs and
anything untracked never ship. Bump `version` for every release; see
[CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT, see [LICENSE](LICENSE). Release history: [CHANGELOG.md](CHANGELOG.md).
---

[← 简体中文（默认）](README.md) · English
