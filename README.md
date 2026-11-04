# Binary Lab (`renfei.binarylab`)

A WinShun resident plugin for embedded work: parse `hex` / `bin` / `oct` strings,
visualize bit operations, and compute C-like struct layout. Everything is a
bounded, pure in-memory computation. It is a **text-based** plugin: grids,
hexdumps and layout maps are rendered by the host's result list and
`preview.text`, not by a window owned by the plugin.

> The interactive bit editor is a planned v2 feature, not part of v1. See
> [ROADMAP.md](ROADMAP.md).

## Commands

| Keyword | Example | What it does |
|---|---|---|
| `bits` | `bits u8 0xA5 & 0x0F` | HEX / BIN / OCT / DEC rows, `~ & \| ^ << >> rol ror`, field `get/set/clear/toggle`, bit grid in the hover preview |
| `bytes` | `bytes hex u16le 34 12 78 56` | Normalize a byte stream, decode words by type and endianness, hexdump with offset and ASCII, IEEE-754 breakdown for floats |
| `struct` | `struct pack=4 struct Frame { u8 sof; u32 seq; u16 len; u8 data[3]; };` | `sizeof` / `alignof`, member offsets, padding rows, a byte map, and copyable `static_assert` text |

### `bits` syntax

```
bits [u8|u16|u32|u64|i8|i16|i32|i64] VALUE [OP VALUE]
     OP       : & | ^ << >> rol ror
     unary    : ~VALUE
     raw bits : @0xFF          (reinterpret a bit pattern, e.g. i8 @0xFF = -1)
     fields   : get|set|clear|toggle VALUE HIGH:LOW [VALUE]
     literals : 0x… 0b… 0o… decimal, optional - sign for signed types, _ separators
```

Unsigned right shift is logical, signed right shift is arithmetic, `~` and all
results are masked to the selected width, and `rol` / `ror` reduce the count
modulo the width. A shift count `>= width` is an error; rotations are not.
Literals are range-checked against the width and are never silently truncated.

### `bytes` syntax

```
bytes [hex|bin|oct] [TYPE] DATA...
TYPE : u8 i8 u16[le|be] i16[le|be] u32[le|be] i32[le|be]
       u64[le|be] i64[le|be] f32[le|be] f64[le|be]
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

The three actions are declared in this order; **order**, not the `key` string,
decides the keyboard mapping.

| Key | Action | Effect |
|---|---|---|
| Enter | `copyPrimary` | Copy the row's main value. On a `struct` summary row it instead expands into one row per member/padding via `effects.replace`. |
| Ctrl+Enter | `copyDetail` | Copy the full detail text (bit grid, hexdump, layout + `static_assert`). |
| Shift+Enter | `chainQuery` | Put a follow-up query in the input box via `effects.setQuery` and keep the window open. |

Queries have no side effects: they never delete files, start programs, change
the clipboard, or open a window. Copy and `setQuery` effects are returned only
when the user triggers an action. Typing `bits `, `bytes ` or `struct ` with no
arguments shows three example rows (`valid: false`, Tab fills the example), and
a parse failure shows one `valid: false` row with an error code and a corrected
example.

## Settings

| Key | Type | Default | Meaning |
|---|---|---|---|
| `defaultWidth` | choice | `32` | Width for `bits` when no type is written |
| `defaultEndian` | choice | `le` | Endianness for types without an explicit `le` / `be` |
| `defaultPack` | choice | `0` | `pack` for `struct` when `pack=N` is omitted |

Settings are read from `initialize` and refreshed on `settingsChanged`; no
secrets are involved.

## Permissions and data

`permissions` is empty and `runtime.sandbox` is `strict`. Measured with
`wsbench`: the plugin process runs at **LOW integrity, not elevated, inside a
job object**. No `ui`, `network`, `files:*`, `clipboard:*`, `processes`,
`notifications`, `elevated`, `contentSearch`, `ai.expose` or `hostHotkeys`
capability is declared.

Rationale: the host already renders the result list, hover preview, right-click
menu, and copy actions. `ui` controls a plugin opening its own windows, finding
other windows, and switching foreground windows — none of which v1 needs. A
`copy` effect is a host-mediated action, so it does not require
`clipboard:write`.

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
  `binarylab.exe` as `runtime.program`.

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Then enable the plugin in WinShun (`Settings -> Plugins`) and type a keyword.
Plugin installation is off by default; enabling it is the permission
confirmation. Developer mode does not replace that confirmation.

## Verification

See [TESTPLAN.md](TESTPLAN.md) for the full matrix and the measured results.
Summary: 139/139 pure-logic assertions pass (including GCC `#pragma pack`
`sizeof` / `alignof` / `offsetof` oracles), and the built plugin was driven
through the real `WinShunPluginHost.exe` runner with `wsbench` for all three
commands, empty input, invalid input, settings overrides and all three
actions.

What has **not** been verified: the actual WinShun GUI message log and hover
panel, monospace alignment of `preview.text`, keyboard feel, an MSVC build,
and importing a packaged `.wsplugin` into a fresh profile.

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
that users import into WinShun. Do not open a source PR against
`WinShun-plugins`. Package only `plugin.json`, `main.cpp`, `logic.h`,
`winshun.h`, `CMakeLists.txt`, `README.md`, `TESTPLAN.md`, `ROADMAP.md`,
`LICENSE`, `tests/` and the built `binarylab.exe` — never the `build/` tree,
logs, virtual environments, or credentials. Bump `version` for every release.

## License

MIT — see [LICENSE](LICENSE).
