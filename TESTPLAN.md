# Test Plan and Verification Record

Status of this document: v1 (`0.1.0`) implementation and verification,
2026-11-05. "Measured" means a command was actually run in this workspace;
"not verified" means it still needs a human with the WinShun GUI.

## L0 — Manifest / packaging

| Check | Result |
|---|---|
| `$schema` points at `WinShun-plugins/schema/plugin.schema.json` | Measured: present in `plugin.json` |
| `id` matches the folder name (`ra9fael.binarylab`), three-part version, MIT license | Measured |
| `permissions: []`, `sandbox: strict`, `start: lazy`, `timeoutMs: 1500`, `memoryMB: 64`, `encoding: utf-8` | Measured |
| Host accepts the manifest and resolves the native `runtime.program` | Measured: `wsbench` loaded `resident, 3 command(s)`; the host's `Manifest.cpp` is the authoritative parser |
| Editor-side `$schema` validation via Python `jsonschema` | Not run: the module is unavailable offline in this workspace (`pip` found no distribution). The `$schema` key is present for editor use; host-side validation passed |
| `winshun.h` is byte-identical to `WinShun/src/plugins-sdk/winshun.h` | Measured: both SHA-256 `9ab4079d…d1984` |
| Package contains no venv, secrets or logs | Not verified: no `.wsplugin` package was produced yet |

Note: `runtime.program` is a native exe. The community schema's whitelist does
not allow it, so the community `python tools/check.py` path does not apply;
this plugin is meant for an own-repository `.wsplugin` import.

## L1 — Pure logic (`tests/test_logic.cpp`, 139 assertions)

| Group | Cases | Result |
|---|---|---|
| `bits` normal | `0xA5 & 0x0F = 0x05`, `~`, `\|`, `^`, `<<`, `>>`, `rol`, `ror`, `u64` wide shifts, field `get/set/clear/toggle`, `@` reinterpretation, settings-derived width, chain text | Pass |
| `bits` invalid / boundary | empty, whitespace, type only, `0x`, `0b102`, `0o8`, `u8 256`, `i8 -129`, `i8 128`, negative unsigned, `shift >= width`, bad op, wrong arity, bad field range, field value too wide, 16 KiB cap | Pass |
| `bits` property | 512 pseudo-random `u64` values round-trip through the hex literal parser | Pass |
| `bytes` normal | hex tokens, compact hex, separators and `0x`, bin 8-bit tokens, oct tokens, `u16le/be`, default endian, `u32le`, `f32le/be`, `f64le`, `i8` signed, trailing bytes, no complete word, hexdump layout | Pass |
| `bytes` invalid | empty, no data, unknown format, odd hex, wrong bin width, oct above 377, bad digit, 4096-byte cap, 16 KiB cap | Pass |
| `struct` hand-computed | natural / `pack=1` / `pack=4` frame, default pack from settings, union, arrays, named nested reference, inline nested, named inline nested, chain text, `static_assert`, byte map | Pass |
| `struct` invalid | `int`, bitfield, pointer, duplicate member, empty struct, zero-length array, unknown type, forward reference, bad pack, truncated definition, oversized layout, 16 KiB cap | Pass |
| `struct` compiler oracle | GCC/MinGW `#pragma pack(push, 1/2/4/8/16)` plus natural, nested struct and packed union: `sizeof`, `alignof`, `offsetof` compared against `evalStruct` | Pass |

Command: `cmake --build build && build/tests/binarylab_tests.exe` →
`binarylab logic tests: 139/139 passed`, exit 0.

## L2 — Raw JSON-RPC (direct pipe, supplementary)

A Python pipe drove `binarylab.exe` directly. This is **not** a substitute for
the host round trip in L3; it only checks fields that `wsbench`'s lossy
`itemToJson` printer omits.

| Check | Result |
|---|---|
| `initialize` → `{protocolVersion:"1", name:"ra9fael.binarylab"}` | Measured |
| `preview.text`, `icon`, `actions` are emitted on stdout | Measured: `color:#185FA5`, 3 actions in order `copyPrimary / copyDetail / chainQuery` with keys `Enter / Ctrl+Enter / Shift+Enter` |
| Empty `text` → 3 `valid:false` help rows | Measured: `help.bits`, `help.bytes`, `help.struct` |
| Invalid input → 1 `valid:false` error row | Measured: `error.E_BAD_LITERAL` |
| `settingsChanged` takes effect on the next query | Measured: `defaultWidth=16` changed the result of `bits 0xFFFF` |
| `defaultEndian=be` and `defaultPack=4` from `initialize` are honored | Measured |
| `cancel` / `settingsChanged` notifications produce no stdout reply | Measured |
| `shutdown` → `{"result":{}}`, process exits 0 | Measured |
| stdout contains only JSON-RPC lines; diagnostics on stderr | Measured: stderr had only `cancel 2` and `settingsChanged` lines |
| Effects for the three actions | Measured: `copy+notify+hide`, `copy+notify+hide`, `setQuery+keep` |

## L3 — Real host round trip (`wsbench` + `WinShunPluginHost.exe`)

Run from the built WinShun tree with the Qt runtime on `PATH`:

```powershell
$env:PATH = "D:\tools\Qt\6.12.0\msvc2022_64\bin;" + $env:PATH
wsbench --plugin <plugin-dir> --query "bits u8 0xA5 & 0x0F" --runs 5
```

| Case | Result |
|---|---|
| `bits u8 0xA5 & 0x0F` | 5 rows, `0x05 / 0b00000101 / 0o005 / 5`, bit grid in `preview.text` |
| `bytes hex u16le 34 12 78 56` | 4 rows, `0x1234 @0`, `0x5678 @2`, hexdump `34 12 78 56` |
| `struct pack=4 struct Frame {…}` | 4 rows, `sizeof=16 align=4`, offsets `0/4/8/10`, byte map `A...BBBBCCDDD...`, `static_assert` text |
| `bits ` (empty argument) | 3 invalid example rows with `autocomplete` |
| `bits u8 0xGG` | 1 invalid `error.E_BAD_LITERAL` row |
| `--setting defaultEndian=be` | `bytes hex u16 12 34` decoded as `u16be` → `0x1234` |
| `--setting defaultWidth=8` | `bits 0xFF` → `0xFF / 0b11111111 / 0o377 / 255` |
| `--setting defaultPack=1` | `struct struct S { u8 a; u32 b; };` → `sizeof=5 align=1` |
| `--action copyDetail` | `effect copy`, `effect notify`, `effect hide` |
| `--action chainQuery` | `effect setQuery bytes hex 05`, `effect keep` |
| `--action copyPrimary` on a struct summary | `effect replace` (member rows), `effect keep` |
| Sandbox | `integrity LOW (4096), elevated no, in a job yes` on every run |

`--query "bits"` (no trailing space) is rejected by `wsbench` itself with
`"bits" triggers no command of this plugin`; that is a bench harness rule, not
a plugin failure. `--query "bits "` reaches the plugin and produces the empty
state.

## L4 — Performance

| Input | Cold start | First round trip | Steady median | p95 |
|---|---|---|---|---|
| `bits u8 0xA5 & 0x0F` | 72.3 ms | 2.1 ms | 1.68 ms | 1.88 ms |
| `bytes hex u16le 34 12 78 56` | 67.6 ms | — | — | — |
| `bytes hex u8` + 4096 bytes (12,300 chars) | 132.2 ms | 62.5 ms | 65.18 ms | **81.26 ms** |

The 4096-byte case initially measured ~413 ms p95 because the full per-word
detail text was duplicated across result rows. Capping the copyable detail to
64 words (while keeping the complete normalized bytes and full hexdump) brought
it under the 100 ms target. The `timeoutMs` budget of 1500 ms is not
approached.

## L5 — Keyboard and failure injection (partly unverified)

| Case | Result |
|---|---|
| All three commands reachable by keyword + text, no mouse required | Partly verified: the protocol exposes `autocomplete` and three ordered actions; GUI keyboard feel is unverified |
| Enter / Ctrl+Enter / Shift+Enter | Verified at protocol level via `--action`; real key events unverified |
| Fast typing / `cancel` while a query is in flight | Not verified: queries are synchronous and bounded, so the host discards stale `queryId`s; `wsbench` cannot interleave |
| Missing exe, corrupt settings, no VC runtime, strict low integrity | Partly verified: LOW integrity confirmed; a missing exe and MSVC-runtime absence were not injected |
| Network unavailable | N/A: no network calls exist |
| No fabricated results on failure | Verified: error and trailing-byte rows are `valid:false` and carry no value |

## Remaining work before calling it "done"

1. Open WinShun, enable the plugin, turn on developer mode, and compare the
   message log against the L2/L3 records (`initialize`, `query`, `action`,
   `cancel`, `settingsChanged`, `shutdown`).
2. Look at the real hover preview: `preview.text` is documented only as a
   tooltip. Monospace font, preserved consecutive spaces, newlines and panel
   height are **not** guaranteed. Mitigation is already in place: every key
   number also appears in `title`, `subtitle`, `copy` and `data`, so the grid
   is an enhancement, never the only channel.
3. ~~Build with MSVC~~ Done 2026-11-05: VS 2026 (MSVC 14.51) x64 Release builds
   with `/utf-8` and the static CRT (`dumpbin /dependents`: USER32, GDI32,
   KERNEL32 only); 139/139 pass. The release workflow builds with MSVC.
4. Package a `.wsplugin` and import it into a fresh profile (installed-off by
   default, permission confirmation on enable).
5. Add fuzz/property tests driven from outside the C++ test binary, and a
   real cancellation stress test with the GUI.

## L4 — Empty keyword state (2026-11-06, measured)

`tools/verify/workbench_drive.py` drives the real exe over JSON-RPC. New checks:

| Check | Result |
|---|---|
| `query(command=bits, text="", raw="bits ")` first row is `bits.open.workbench.empty` | Pass |
| First row is valid (no `valid: false`), `actions[0] = openEditor / Enter`, `data.expression = ""`, `data.width = 32` | Pass |
| First row outranks the example rows by score | Pass |
| `query(command="", text="", raw="bits ")` resolves the keyword from `raw` | Pass |
| `openEditor` from that row leaves the EDIT control empty; client height 446 (default u32) | Pass |
| Opening again from the ordinary `bits u32 0xA5 & 0x0F` row keeps its expression | Pass |
| Full chain (chips, bit toggle, parity chips, toolbar, five tabs, error text, screenshots, exit 0) | Pass |
| `cmake --build build` + `ctest --test-dir build` | Pass (139/139) |

Host-side reading (not a host round trip): `WinShun/src/plugin/Catalog.cpp:346-365`
matches a keyword followed by a space with `m.text = ""`; `Host.cpp:1437-1438`
sends `text=""`, `raw="bits "`; `Launcher.cpp:5624` refuses actions on
`valid: false` rows; `Items.cpp:159` clamps scores to 0..100 (101 becomes 100,
still above the 90-point examples).

## L6 — Expression line, units and floats (2026-11-06, measured)

- `tests/test_logic.cpp`: **183/183** assertions (139 original + 44 new). New
  vectors cover SI / IEC / byte spellings, longest-match (`4MiB`), hex-digit
  regressions (`0xAB`, `0xDEADBEEF`, `0x1E6`), detached suffixes
  (`E_UNIT_ATTACH`), unit-on-index (`E_UNIT_ON_INDEX`), U+00B5 / U+03BC
  rejection, and IEEE-754 patterns cross-checked with Python `struct`
  (`f64 1.5` = `0x3FF8000000000000`, `f32 0.1` = `0x3DCCCCCD`,
  `1u` = `0x3EB0C6F7A0B5ED8D`, `+Inf` / `-Inf`).
- `tools/verify/workbench_drive.py`: `ALL CHECKS PASSED`, exit code 0. Beyond
  the existing chain it drives `VK_RETURN` submission, adjacent dedup,
  `history.jsonl` creation in a temporary `dataDir`, Up/Down browsing with
  draft restoration, dropdown recall, `4M` / `4Mi` / `4MB`, `f64 1.5`
  (HEX + FLOAT chips), `1u`, and Settings-tab clear-history.
- Eight screenshots inspected: blank cue, bits (EDIT alignment + dropdown
  button), settings (clear-history + count), help (all five lower lines above
  the status bar), error, hover, float (S/E/M band) and history dropdown.

### Honest note on this level

The drive harness starts `binarylab.exe` directly over a JSON-RPC pipe; it does
**not** run inside the host's LOW-integrity job. The `dataDir` write therefore
proves the persistence code works, **not** that the strict sandbox grants it.
The plugin degrades to session-only and labels that state in the Settings tab
if `CreateFileW` fails. Real-host confirmation is still outstanding (same
caveat as L4).

## L7 — Expression parser and evaluation modes (2026-11-06)

- `tests/test_logic.cpp`: **215/215** assertions (183 previous + ~32 new).
  New vectors cover operator precedence (`u8 1 + 2 * 3` = 7,
  `u8 (1 + 2) * 3` = 9, `u8 1 << 2 + 1` = 8, `u8 0xF & 3 | 4` = 7,
  `u16 2 ** 3 ** 2` = 512 right-associative, `i8 -2 ** 2` = 4,
  `u8 (~0) & 0x0F` = 0x0F), fixed-mode wrapping (`u8 200 + 100` = 0x2C with a
  "wrapped" note, `i8 -128 - 1` = 127), C-mode promotion (`1 + 2` → i32 3,
  `0xFFFFFFFF + 0` → u32, `u8 200 + 100` with `EM::C` stays u8 0x2C —
  prefix wins), C-mode errors (`2147483647 + 1` → E_RANGE,
  `9223372036854775807 + 1` → E_RANGE, `(-9223372036854775807 - 1) / -1` →
  E_RANGE, `2 ** 62` → E_RANGE, `2 ** -1` → E_RANGE, `1 << 40` → E_SHIFT,
  `1 / 0` → E_DIV_ZERO) and C-mode defined unsigned wrap
  (`0xFFFFFFFF + 1` = 0). Bare negative literals now wrap (`u8 -1` = 0xFF,
  `i8 -129` = 0x7F); the old error tests for those were removed.
- `tools/verify/workbench_drive.py` gained sections 7h (precedence through
  the real GUI chips: `u8 1 + 2 * 3` → `0x07`, `u8 (1 + 2) * 3` → `0x09`,
  `u8 1 << 2 + 1` → `0x08`, `7 / 2`, `7 % 2`, `1 << 7`) and 7i (settings-page
  fixed/C switch: `setEvalMode` debug lines, `cmode=` layout flag,
  `200 + 100` → `0x0000012C` in C mode, `u8 200 + 100` → `0x2C`).
- **Status of 7h/7i: written, not yet measured.** The 2026-11-06 evening GUI
  runs collided with live desktop activity — stray IME keystrokes landed in
  the expression box (`'yige'`) and an external clipboard writer replaced
  chip copy results — so the runs were aborted instead of fighting the user's
  desktop. The logic-level expectations above are fully covered by the unit
  tests; the GUI plumbing still needs one clean drive run (or a human pass in
  the real WinShun window).
- The `evalMode` setting itself (fixed vs C from the host) is verified at the
  logic level; host `settingsChanged` round trip is unchanged plumbing
  already covered by L2.

## L8 — f16 and the float composition bar (2026-11-06)

- `tests/test_logic.cpp`: **259/259** assertions. New blocks: container table
  self-consistency, S/E/M segment boundaries for f16/f32/f64,
  `floatSpecialLabel`, 15 binary16 vectors (`1.5` → 0x3E00, `0.1` → 0x2E66,
  `65504.0` → 0x7BFF, `65520.0` → +Inf, smallest normal/subnormal, `-0.0`),
  overflow/underflow notes, round-half-to-even ties and an exhaustive
  round-trip over every finite binary16 pattern; `f128 1.5` → `E_FLOAT_TYPE`.
- `tools/verify/workbench_drive.py` section 7j: bar hidden by default; the
  pill adds exactly 34 px and the grid does not move; S | E | M rects span
  `clientW - 24` in MSB order; `f32 1.5` blocks copy `0x0` / `0x7F` /
  `0x400000`; `f16 1.5` copies `0xF` / `0x200` at the same row height;
  `u32 0x3FC00000` gives a ghost bar that still copies `0x7F`; `u8` leaves no
  clickable segment while the row stays; toggling off restores the height.
  Screenshots `workbench_floatbar.png` (E hovered) and
  `workbench_floatbar_ghost.png`.
- **Measured 2026-11-06: 7h, 7i and 7j pass on an idle desktop**, full drive
  `ALL CHECKS PASSED`, exit code 0. The first run failed in 7i because the
  script read `setmode1` from the bits-page layout, where the settings
  buttons have no geometry (`0,0,0,0`); 7i now switches to the settings tab
  before clicking and back to bits before copying. Screenshots checked by
  eye: the hovered E caption fits the row, E bits 30..23 are tinted in the
  grid, and the ghost bar is visibly dimmed with the "not the current
  result" caption.

## L9 — Float on the toolbar and decimal values (2026-11-06, measured)

- `tests/test_logic.cpp`: **270/270**. New: `f32 88` → 0x42B00000,
  `f16 65504` → 0x7BFF, `f64 1`, `f32 -2`, `f16 0`, `f32 4K` → 4000.0,
  `f32 @88` and `f32 0b1011000` stay raw 0x58, `f16 65536` reports +Inf,
  `u32 88` untouched, `f32 0x3FC00000 | 1` still ORs the pattern.
- Drive section 7k, measured `ALL CHECKS PASSED`, exit 0: `f32 88` chips
  `0x42B00000` / `88`; width f64 rewrites to `f64 88` (`0x4056000000000000`);
  f16 gives `0x5580`; the disabled 8 leaves the expression alone; `kind0`
  gives `u16 0x5580` with DEC 21888; `f32 -1.5` → `sgn1` gives
  `i32 @0xBFC00000` with DEC -1077936128; `kind2` gives `f32 0xBFC00000` and
  FLOAT -1.5. Screenshot `workbench_float_toolbar.png` checked by eye.

## L10 — Record formats: model, rules and TOML loading (2026-11-06, measured)

- `tests/test_logic.cpp`: **378/378** assertions (270 previous + 108 new).
  New blocks cover: `builtinRawFormats` inventory (2 formats, word widths,
  byte order, 3 rules each, `entryFor` lookup); format set semantics
  (`addFormat` same-name overwrite with a note, `builtinFormatSet` copy);
  strict hex parsing (separators `space , : ; _`, `0x` prefixes, odd-digit
  `E_HEX_ODD`, 4096-byte cap); `decodeRecords` record cutting per
  `word_bits`, little-endian field slicing, array expansion, lsb/msb
  whole-record reversal, trailing-bit reporting, 24-record / 4096-bit caps;
  the restricted rule evaluator (arithmetic/comparison/logic operators,
  `any`/`all`/`min`/`max`/`abs`/`len`, generator loops, chained comparisons,
  Python floor-division and modulo semantics, rejection of attributes,
  slices, strings and unknown names at load, AST node / depth / step caps).
- TOML loading (`formats_toml.h`, vendored toml++ v3.4.0 compiled with
  `TOML_EXCEPTIONS=0`): the two hextract fixtures parse and decode
  identically to the built-in definitions; unknown keys, wrong types and
  missing `name` / `word_bits` fail with the file's line number in the
  message; a broken file degrades to one red error entry and the rest of the
  directory still loads; `loadFormatDir` is sorted, non-recursive, capped at
  64 files of 64 KiB, and built-ins win name clashes.
- JSON-RPC smoke over the real pipe: `bytes fmt` (list), `bytes fmt
  blm_rawdata_256 <hex>` (decode with a rule hit) and an unknown format name
  (clear error row) all behave as documented.

## L11 — Byte workbench page (2026-11-06, measured)

`workbench_drive.py` section 8 drives the real window; full run
`ALL CHECKS PASSED`, exit 0:

| Check | Result |
|---|---|
| `bytes hex u16 34 12 78 56` first row is `bytes.open.workbench` with `data.expression` preserved; Enter opens the bytes tab (scalar) | Pass |
| Expression line keeps the scalar text; click on a byte selects it (layout logs `selBegin`); HEX chip copies the active word (`0x5678`) | Pass |
| BE button rewrites the `le`/`be` suffix and `34 12` reads `0x7856` | Pass |
| Drag across bytes 1..2 extends the selection (`selBegin`/`selEnd`) | Pass |
| `→ Bits` hands the active word to the bits page (mode 0, `u16 0x5678`) | Pass |
| `bytes fmt blm_rawdata_256 lsb <hex>` opens the records mode (`bytesMode=records`); format dropdown lists the built-ins plus the fixture folder formats | Pass |
| Click on the `integral0` cell selects record/field (`selField`); cell chip shows the field value | Pass |
| MSB button rewrites the order token in the expression and logs `setOrder` | Pass |
| Screenshots `bytes_scalar.png`, `bytes_records.png`, `bytes_formats.png` checked by eye | Pass |
| `cmake --build build` + `ctest --test-dir build` | Pass (378/378) |

Caveats carried over from L6: the drive harness runs the exe directly over
JSON-RPC, not inside the host's LOW-integrity job, and PostMessage cannot set
`GetKeyState(VK_CONTROL)`, so the Ctrl+C selection copy is exercised manually
rather than by the script.

## Honest notes / corrections

- The earlier design example said `bytes oct 377 040 000 -> FF 40 00`. That
  arithmetic was wrong: `0o40 = 32 = 0x20`. The implemented and tested result
  is `FF 20 00`.
- The design's `p95 < 100 ms` target was a goal, not a measurement. It is now
  measured at 81.26 ms for the 4096-byte case after the optimization above.
- The layout oracle is the compiler's own `#pragma pack` layout. It was first
  measured with GCC/MinGW; on 2026-11-05 the same 139 assertions also passed
  under MSVC 14.51 x64, so MSVC agreement is measured, not inferred. MSVC warns
  C4121 (alignment sensitive to packing) on two oracle structs; that warning is
  the point of those structs.
- `wsbench` prints items through `itemToJson`, which drops `icon`, `preview`
  and `actions`. Those fields were confirmed on the raw protocol pipe (L2) and
  are parsed by `WinShun/src/plugin/Items.cpp:138-153`; their absence in
  `wsbench` output is a printer limitation, not a plugin failure.
- PowerShell console capture in this environment renders Chinese text as
  mojibake (OEM code page). Direct interactive output and the raw JSON pipe
  showed correct UTF-8 Chinese, so this is a capture artifact.
- `steamdb-plugin-design.md` in the workspace root and the modified
  `WinShun/tests/CMakeLists.txt` predate this work and were left untouched.
