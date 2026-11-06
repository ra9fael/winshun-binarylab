# Roadmap

## Status

- **v1 (0.1.0) — implemented and verified.** Text-based, zero-permission,
  strict-sandbox C++ resident plugin. Three keywords (`bits`, `bytes`,
  `struct`), `preview.text` visualizations, three ordered actions, no window,
  no file writes, no network.
- **v2 (0.2.0) — approved and started.** The user approved on 2026-11-05:
  single plugin (not a split), a `bitsEnter` settings choice moves `openEditor`
  to Enter, and the first text batch adds `reg` + `conv` + `crc`. Order:
  spike → editor MVP → `reg` → field lanes → text UX polish → `conv`/`crc`.
- **v2 spike — passed (2026-11-05).** `editor.h` creates a real window from
  the `openEditor` action. Measured through the real host (`wsbench`,
  WinShunPluginHost): process at integrity LOW, inside the job, window
  created in ~10 ms, protocol round trip unaffected, clean shutdown with the
  GUI thread joined in the destructor. Rendering verified by screenshot: bit
  grid MSB-first with correct 0x05 pattern, base lines correct. Notes: the
  runner strips the plugin's environment, so runtime feature flags cannot come
  from env vars; `represent()` strings carry their own 0x/0b/0o prefixes.
- **v2 editor MVP — implemented (2026-11-05).** The window is now interactive:
  click a cell toggles its bit, dragging paints cells to the first cell's value,
  arrow keys move a focus cell (yellow border), Space toggles the focused bit,
  Ctrl+C / B / D / O copy the value as HEX / BIN / DEC / OCT, Esc closes. The
  four-base header updates live. Also fixed the shortcut mapping: `bits` rows
  default to **Enter = copy, Ctrl+Enter = open editor** (configurable via the
  `bitsEnter` setting; `editor` swaps them), with `copyDetail` on the host's
  action menu. Verified by driving the plugin over JSON-RPC and screenshotting
  the window: a simulated click on bit 7 turned `0x05` into `0x85`, the bit-7
  cell showed `1` with the focus border. (Clipboard write from inside the window
  still needs a real-host confirmation; the sandbox permits the native call but
  it was not exercised in the headless driver.)
- **v0.3 GUI workbench — approved (2026-11-05).** raygui is shelved until every
  feature passes real-user testing; the GDI path continues. Approved decisions,
  from the user:
  1. **One window, three tabs.** A shared shell — top bar (mode tabs, settings
     dropdown, help panel), expression line, result row with per-base mouse
     copy buttons, bottom status bar — with the content area switching between
     bits / bytes / struct. All hint text moves into the help panel.
  2. **Expression-driven bits.** The editor accepts a SpeedCrunch-compatible
     expression; the bit grid operates on its evaluated result.
  3. **Copy stays on Ctrl+C/B/D/O and gains mouse-clickable per-base buttons**
     plus copy-all.
  4. **bytes mode is editable**: click a byte to select it, edit its bits, the
     stream and the decode panel update live; endianness from settings.
  5. **struct mode edits member values**: click a member (ruler block or table
     row), type a new value decoded per the member's type; layout/offsets stay
     read-only.
  Phases: v0.3a shell + bits, v0.3b bytes, v0.3c struct, v0.4 history/ans/
  variables and settings persistence. Top-bar settings: width, signedness,
  endianness, pack, `^` semantics; status bar = mode state | selection/focus |
  transient feedback (copied ✓ green, `E_*` red, fades after ~2 s).
- **v0.3a workbench shell — implemented (2026-11-05).** The unified shell is
  live: three tabs (bits interactive, bytes/struct placeholders), a native
  EDIT expression line re-evaluated through `evalBits()` on every change, the
  result row as mouse-clickable HEX/OCT/BIN/DEC/copy-all chips, the three-part
  status bar, and a settings dropdown (width/signedness active; endianness/
  pack/`^` shown disabled until v0.3b/c). Verified by `workbench_drive.py`
  over raw JSON-RPC: live re-evaluation, the **first successful clipboard
  write from inside the sandboxed window** (HEX chip -> `0x000000F0`), bit
  toggle, width 32->64 window resize, tab switch and back, clean exit 0.
- **v0.3a2 — approved (2026-11-05).** User decisions, all with the recommended
  option: (1) the bit grid keeps 8 columns and widens its cells to fill the
  client width (~40 -> ~70 px at 616 px, byte-group gap every 8 bits);
  (2) a persistent width/signedness toolbar sits directly above the grid —
  segmented `8|16|32|64` buttons plus `u/i`, one click resizes the window,
  synced with the settings panel, and an explicit type prefix in the
  expression still overrides it temporarily; the engine already supports any
  width 1..64, the presets are UI only, and 64 bits is the cap (the evaluator
  is `uint64_t`-based; wider needs a bigint rewrite that embedded-register
  work does not justify); (3) CRC support ships as the common preset set
  computed in one shot - CRC-8(0x07), CRC-16/ARC, CRC-16/MODBUS,
  CRC-16/CCITT-FALSE, CRC-16/XMODEM, CRC-32 - plus even/odd parity with the
  popcount and an additive checksum, all table-driven from one generic
  parameterised kernel and tested against the `"123456789"` standard vectors.
  Surfaces: text commands `crc` (all presets at once, or one via
  `crc crc16m ...`) and `parity <expr>`; a live parity indicator in the bits
  status bar; a checksum panel in the bytes workbench (range = whole stream
  or the selected byte range, each algorithm row individually copyable).
  Order: v0.3a2 grid fill + width toolbar + parity indicator, v0.3b bytes
  mode + checksum panel + `crc`/`parity` commands, v0.3c struct mode, v0.4
  history/ans/variables + persistence.
- **v0.3a2 grid fill + width toolbar + parity — implemented (2026-11-05).**
  Adaptive `cellW = (clientW - 2*16 - 8 + 4) / 8` (72 px at 616, 55 px at
  480) with an 8 px nibble gap. Toolbar `8|16|32|64` + `unsigned u|signed i`
  above the grid; a type change keeps the bit pattern (truncate, or
  sign-extend when the old type was signed) and rewrites a leading type
  prefix in the expression so re-evaluation agrees with the click; the
  settings panel goes through the same `setType` path. The status bar shows
  popcount and the even-parity bit. Fixed a chip mapping bug (BIN and DEC
  chips copied each other). `workbench_drive.py` covers toolbar 8 (shrink,
  truncate), i8 DEC `-15`, i64 sign extension `0xFFFFFFFFFFFFFFF1`, prefix
  rewrite `u32 -> u16`, and settings cycle sync.
- **v0.3a3 — implemented (2026-11-06).** Five top-bar tabs: the first three
  remain content modes, `Settings` and `Help` become real tabs that replace
  the content area with a two-column panel (this column / read-only host
  settings snapshot, and syntax / keys / parity terms). The old floating
  overlays and their "first click only closes the panel" branch are deleted.
  The status bar becomes two rows (`kStatusH` 24 -> 46): row 1 holds context
  plus focus/copy feedback, row 2 holds four labelled clickable parity chips
  (`ones`, `parity`, `even-bit`, `odd-bit`) or a full untruncated parse error;
  hovering a chip swaps the row-1 text for its definition, so no box ever
  covers the bit grid. `kMinClientH = 280` keeps every non-bits tab at one
  fixed height. The first bits result row is now a dedicated workbench entry
  (`Enter` opens, the remaining rows keep `Enter` = copy). Host settings are
  read-only in the window: `winshun.h` exposes `settingsChanged()` but no
  setter. `workbench_drive.py` was rebuilt (the old `build/verify` was wiped)
  and now covers parity-chip copies, all five tabs, direct tab-to-tab
  switching, settings-tab buttons, the full error string and five screenshots.
- **v0.3a4 — implemented (2026-11-06).** Empty keyword state: `bits ` with no
  words reaches `query()` as `command=bits, text=""`. It used to answer with
  only `invalid()` example rows, which the host refuses to action
  (`Launcher::performPlugin` returns on `!p.valid`), so Enter was a no-op.
  Answer with a valid first row (`bits.open.workbench.empty`, `Enter` =
  `openEditor`, score above the examples) that opens a blank workbench at the
  host's default width; examples follow as before. `EditorSpec.blankExpression`
  suppresses the usual `0x0` seed, so clicking a bit is not later undone by a
  stale evaluation. `openEditor` accepts an explicit empty `data.expression`
  and strips a leading command keyword from `text`. `bytes ` / `struct ` keep
  the example-only behaviour until their interactive tabs are built. The drive
  harness covers both query paths, the blank EDIT text and the ordinary
  expression entry.
- **v0.3a5 — implemented (2026-11-06).** Five expression-line requirements,
  all decisions taken with the recommended defaults: (1) history is a dropdown
  plus `Up`/`Down`, **not** a sixth tab; (2) `KB`/`MB`/`GB` follow SI 1000 and
  `Ki`/`Mi`/`Gi` carry the 1024 meaning; (3) floats are IEEE-754 bit patterns
  only, with no arithmetic; (4) `u`, `micro` and `1e-6` are all accepted for
  micro while `µ`/`μ` are rejected with an ASCII hint; (5) fractional units
  ship together with float parsing rather than an extra phase. `Enter` submits
  and records a confirmed expression (JSONL in `dataDir`, cap 200, adjacent
  dedup, Settings-tab clear); the EDIT shrinks to 20px with an 8px left inset.
  `logic.h` gains `UnitSpec`/`splitUnitSuffix`, `LiteralInfo`/`LiteralOptions`,
  `exactFloatText`, `isFloatType`, and a float branch in `parseLiteral` that
  skips integer range checks. Suffixes are stripped only from decimal literals
  whose head is plain digits, by longest-match, case-sensitively, with `E`
  reserved for exponents; bit indices and field values pass
  `LiteralOptions{false,false}`. `BitsEval` carries `isFloat`/`floatText`/
  `unitNote`; the workbench swaps the DEC chip for FLOAT, paints an S/E/M band
  on the grid and names `f32`/`f64` in the status bar. `setType` refuses width
  changes on a float result instead of silently truncating the pattern.
  Verification: 183/183 logic assertions (44 new unit/float vectors) and a
  drive chain covering Enter, dedup, Up/Down, dropdown recall, `4M`/`4Mi`/
  `4MB`, `f64 1.5`, `1u`, clear-history and seven screenshots.
- **v0.3a6 — implemented (2026-11-06).** Full-precedence integer expressions
  and two evaluation modes, from the user's request for "basic arithmetic,
  brackets to keep semantics, and a mode deciding whether evaluation promotes
  like C". Approved decisions: fixed-width stays the default (every step
  masked, wrapping reported in the detail); `c` mode promotes like C with the
  result type derived and shown; an explicit type prefix always wins over
  C mode; bare negative literals wrap instead of erroring, consistent with
  the fixed-width philosophy. `logic.h` gains a recursive-descent parser
  (unary → `**` right-assoc → `* / %` → `+ -` → shift/rotate → `&` → `^` →
  `|`), portable 64-bit overflow checks (no `__int128`, so MSVC still
  builds), and `EvalMode::Fixed` / `EvalMode::C`. The GUI shows the mode and
  the derived result type in the status bar, disables the width/sign toolbar
  and settings rows in C mode, adds an eval-mode row to the settings page and
  an `evalMode` row to the host snapshot, focuses the expression box on open,
  and names the offending token in syntax errors. Bash editing keys
  (Ctrl+A/E/U/K/W/Backspace/Y/L) and measured expression-line centring
  landed earlier in this batch. Verification: 215/215 logic assertions
  (~35 new vectors for precedence, fixed wrap, C promotion/overflow and
  prefix-wins); drive sections 7h/7i (precedence chips, session mode switch)
  are written but their GUI run is still pending a desktop that is not in
  active use.
- **v0.3a7 — implemented (2026-11-06).** Float package from "why only
  f32/f64??" and the approved wireframe B. One IEEE-754 container table
  (`FloatSpec`) drives every float consumer; binary16 is a row like the
  others (frexp/rint encoder, round-half-to-even); unimplemented spellings
  give `E_FLOAT_TYPE`. The workbench gains an in-window composition bar,
  opened by a pill at the free right end of the top bar (rejected: a second
  window glued outside the frame — editor.h:8 history, self-sizing frame,
  DPI/move sync, cross-HWND hover). Hard rule kept: the bar row height
  follows the toggle only, never the result type. Ghost reading for integer
  results at 16/32/64 bits, grey band at 8 bits, bar ↔ grid hover linkage,
  click-to-copy per field, `floatBar` setting for the initial state.
  Verification: 259/259 logic assertions; drive section 7j written, GUI run
  pending with 7h/7i.
- **v0.3a8 — implemented (2026-11-06).** From "浮点模式为什么只能用
  f16/f32/f64前缀切换？？": the sign row became a type row with `float f`,
  width buttons name the containers in float mode, containers re-encode the
  value, integer <-> float keeps the bits. A plain decimal under a float
  prefix is now a value (`f32 88` = 88.0); raw patterns need a base prefix or
  `@`. Status-bar messages are no longer clipped.
- **v0.3b bytes page + record formats — implemented (2026-11-06).** The bytes
  tab is no longer a placeholder. Scalar mode (`hex|bin|oct [type] data`) is an
  offset / hex / ASCII grid with click/drag/arrow/wheel selection, active-word
  chips (HEX / DEC|FLOAT / BIN), a width / signedness toolbar and LE / BE
  buttons that rewrite the expression, Ctrl+C selection copy, and a "→ Bits"
  handoff of the active word's raw bits. Record decoding arrives with
  `bytes fmt`: hextract's TOML schema (vendored toml++ v3.4.0, exception-free,
  SYSTEM include) parsed by a schema-checked reader with file line numbers,
  two built-in BLM formats, and user formats loaded read-only from the new
  `formatDir` folder setting behind a `files:read` permission (up to 64 files
  of 64 KiB, non-recursive, built-ins win name clashes). Rule `when`
  expressions run on a restricted evaluator with hard caps (256 nodes, depth
  32, 4096 steps per record; Python integer semantics; attributes, slices,
  strings and unknown names rejected at load). The records mode shows a
  format dropdown, a records table (arrays expanded per element), rule-tinted
  rows, a record / field value chip and LSB / MSB buttons that rewrite the
  byte-order token. The bytes first result row is a dedicated workbench entry,
  mirroring `bits `. Verification: 378/378 logic assertions and a
  `workbench_drive.py` section 8 covering scalar open, click/drag selection,
  chip copy, endian rewrite, the bits handoff and the full records flow,
  `ALL CHECKS PASSED`.
- **Still deferred in v0.3b:** the CRC kernel, the `crc` / `parity` commands,
  the bytes-page checksum panel, and struct-mode member editing (v0.3c).

The two versions are deliberately separated. v1 keeps `permissions: []`; v2 is
the point at which a real permission review is required, and it must not raise
the permission level of the whole plugin by accident.

---

## v2 goal

A clickable 64-bit bit editor for embedded register work:

- One row of cells per bit, MSB-first, with explicit bit numbers and byte/field
  group boundaries.
- Click a bit to toggle it; drag or shift-click to select a range; set / clear /
  toggle / write a field value.
- Live HEX / BIN / OCT / DEC / signed readouts, endianness toggle, width
  selector (8/16/32/64), and a field definition panel (name, `HIGH:LOW`,
  value, colour).
- Apply the edited value back into the WinShun input box as a `bits` query, or
  copy it, without ever writing a file or launching a program.

Why a window is needed: v1's `preview.text` is a passive tooltip. It cannot
receive clicks, and the host does not promise a monospace font, preserved
spaces, or newlines. Colour highlighting and per-bit hit testing are impossible
through the result-list fields.

---

## v2 architecture (proposed)

### Command surface

Add a fourth command, `bitedit` (keyword), plus an `editBits` action on every
`bits` result row so a user can go straight from a computed value into the
editor. Keep the existing three v1 keywords unchanged. The editor is opened
**only by an explicit user action** — never by a query, never on every
keystroke, never automatically on `initialize`.

Two candidate entry paths, to be decided by measurement:

| Path | How | Consequence |
|---|---|---|
| **A (recommended)** | `action("editBits")` starts the window on a dedicated UI thread and returns `effects` immediately | `query()` stays synchronous and pure; no `queryAsync` needed; no per-keystroke side effects |
| B | Live preview while typing | Needs `queryAsync` (see below); heavier, risks per-keystroke window churn |

### Window

- Win32 only: `CreateWindowExW`, a dedicated thread with its own message loop,
  an owner-drawn/custom-painted client area, and `WM_DPICHANGED` handling via
  `SetProcessDpiAwarenessContext` (or `SetProcessDPIAware` on older hosts).
- No Qt, no third-party UI library, no second process. The process remains a
  single resident plugin exe.
- The UI thread is started on the action, not on the protocol thread. All
  settings and locale values it needs are snapshotted as plain values before
  the thread starts; it never reads `settings()` from the UI thread.
- On `shutdown` the window is posted `WM_CLOSE`, the UI thread is joined, and
  all handles are released. The local SDK has no `shuttingDown()` hook, so the
  derived class destructor must do the join; if the SDK copy is upgraded to the
  remote `main` version, override `shuttingDown()` as well.
- The window never calls `SetForegroundWindow` on another application's window,
  never enumerates other windows, and never installs a global hook.

### If path B is chosen: asynchronous queries

The remote `main` SDK adds
`queryAsync(const Query&, std::function<void(Json)>)`; its default
implementation calls `queryResult` synchronously. An async override must:

1. Snapshot `settings()` and `locale()` on the protocol thread before starting
   work.
2. Reply **exactly once** for each `queryId`.
3. Map `cancel(queryId)` to a cancellation flag and drop stale replies.
4. Join all workers in `shuttingDown()` **and** in the derived destructor.
5. Optionally send `partial()` results while a request is pending.

The wire protocol is unchanged. The SDK copy currently bundled with v1 is the
local `WinShun/src/plugins-sdk/winshun.h` and has **no** `queryAsync` and **no**
`initialize.capabilities`; path B requires upgrading that copy (or handling the
raw JSON-RPC directly). This is a compatibility cost and is the main reason
path A is preferred.

---

## v2 permission changes (the important part)

| Permission | v1 | v2 | Reason |
|---|---|---|---|
| `ui` | no | **yes** | Opening the plugin's own window. The host's `ui` permission covers own windows, finding other windows, and foreground switching; only the first is used. |
| `clipboard:write` | no | **yes, if the window has a Copy button** | Effects such as `copy` are only returned from `action` responses. A window that copies at an arbitrary time needs direct clipboard access. The host enforces this through job UI restrictions. |
| `clipboard:read` | no | no | Not needed. |
| `files:*`, `network`, `processes`, `notifications`, `winshun:*` | no | no | No file, network, subprocess or host-tool dependency. |
| `elevated` | no | **no** | v2 never needs to operate on a window opened by an elevated process. `elevated` is only for plugins bundled with their own exe when WinShun itself runs elevated, from a user-confirmed protected copy. Requesting it here would be unjustified. |
| `ai.expose` | no | no | Queries stay read-only; do not expose an interactive window as an AI tool. |
| `contentSearch` | no | no | No file content search. |

`ui` is only **partly** enforced by Windows (the job limits desktops and USER
handles). `strict` sandbox is low integrity, which may prevent a normal
top-level window from being created or from receiving input from a
medium-integrity launcher. Decision gate: try `strict + ui` first and measure.
If the window cannot be created or cannot be interacted with, evaluate either
`normal + ui` (weaker isolation, must be explained in the README and in the
permission confirmation) or shipping the editor as a **separate optional
plugin** so v1 stays zero-permission. Never silently downgrade v1.

The host shows the permission declaration when the user enables the plugin;
developer mode and template creation do not replace that confirmation. A
permission declaration is not an OS-enforced boundary — only the `strict`
sandbox restrictions are.

---

## v2.1 — global hotkeys (optional, after v2 works)

Only when `initialize.capabilities.hostHotkeys == true`:

1. Send `winshun/hotkeys` to reserve the keys the editor wants.
2. Call `winshun/tool` with `register_hotkeys`, parameters
   `{keys: ["F1"]}`. The result is `{errors: {}}` or a per-key Windows
   registration error map; surface failures as a non-fatal notification.
3. The host sends a `hotkey` notification `{key: "F1"}`.
4. The plugin executes only the action currently bound to that key, and
   **ignores late notifications while a key is being recorded**.
5. During recording, send `winshun/hotkeys` with an empty array to release the
   keys temporarily; the host cleans everything up on window close or crash.
6. This tool uses the existing `ui` permission and does not raise the plugin's
   permission level. On an older host without the capability, the plugin must
   register the hotkey itself (`RegisterHotKey`) or disable the feature.

The bundled local SDK has no `initialize.capabilities` field, so v2.1 needs the
newer SDK copy. Do not pretend the local host supports it.

---

## v2 data and state

- Still no secrets, no network, no upload of user data.
- Field definitions and window geometry may be persisted, but only in
  `WS_DATA_DIR` (`<id>.data`) via atomic JSON writes; `strict` sandbox permits
  writes there. If `normal` sandbox is chosen for the window, this restriction
  becomes a convention rather than an enforcement — document that honestly.
- Any secret (there should be none) must use the host's `password` setting
  type, never source, ordinary config or logs.
- stdout remains JSON-RPC only; all diagnostics to stderr.

---

## v2 test plan

| Layer | Case | Pass criterion |
|---|---|---|
| Manifest | `ui` (+ optional `clipboard:write`) declared and justified; no `elevated`; version bumped | Schema and README agree with the code |
| Query purity | Type 20 characters into `bits`, `bytes`, `struct`, `bitedit` | No window, no clipboard change, no file write, no process start; message log shows queries only |
| Window creation | `action("editBits")` under `strict + ui` | Window appears, is interactive, process stays LOW integrity and inside the job |
| DPI / focus | 100% / 150% / 200% scaling, focus loss, minimize/restore | Cells remain hit-testable; no crash; no leaked HWND |
| Editing | Click a bit, drag a range, set/clear/toggle, write a field, change width and endianness | Value matches the v1 pure-logic oracle for every operation |
| Keyboard-only | Open, edit, apply and close without a mouse | Complete path via Tab / arrows / Space / Enter / Esc |
| Hotkeys (v2.1) | Register, fire, record a new key, fire a late notification | Only the currently bound action runs; late notifications ignored; keys released during recording and cleaned on close/crash |
| Lifecycle | `cancel`, `settingsChanged`, `shutdown`, crash, host exit | One reply per queryId, no stale replies, UI thread joined, no orphan window or process |
| Failure injection | Missing exe, missing VC runtime, corrupt settings, strict low integrity, host without `hostHotkeys` | Clear error row or non-fatal notification; no fabricated value; no hang |
| Performance | 16 KiB input, repeated open/close of the editor | No per-keystroke window churn; p95 stays under the 1500 ms query budget |
| Real host | WinShun developer mode + message log | Verified `initialize` / `query` / `action` / `cancel` / `settingsChanged` / `shutdown` round trips, not just a standalone exe run |

`wsbench --action` can verify the `action` response but cannot show a window;
the window, DPI, focus and hotkey cases require a human in the real WinShun UI.

---

## Versioning and packaging

- v2 must bump `version` (e.g. `0.2.0`) and update the README permission table,
  the failure-handling section and TESTPLAN.md in the same commit.
- Keep v1's text-based rows intact. Users who do not want `ui` should still be
  able to use `bits` / `bytes` / `struct`. If that cannot be guaranteed, ship
  the editor as a second plugin id rather than changing v1's permissions.
- Distribution stays an own-repository `.wsplugin` import: a native exe is not
  accepted by the community `WinShun-plugins` repository. Package only source,
  the SDK copy, docs and the built exe — no `build/`, logs, keys or venv.

---

## Open questions (must be answered before v2 coding starts)

1. Does a top-level Win32 window created by a **low-integrity** process accept
   mouse/keyboard input from the medium-integrity launcher? Measured: **no** —
   not tested yet.
2. Is `clipboard:write` actually usable under `strict` sandbox, or must the
   editor use the host's `copy` effect via a follow-up action instead?
3. Does the installed WinShun build advertise
   `initialize.capabilities.hostHotkeys`? The local SDK in this workspace does
   not expose capabilities at all.
4. Does `preview.text` use a monospace font in the real GUI? Still unverified
   for v1; the answer affects how much of the grid should move into v2.
5. Does MSVC build cleanly with `/utf-8` and the static CRT? Only MinGW-w64 was
   measured for v1.
6. Is a second plugin id preferable to a permission change on
   `ra9fael.binarylab`? Product decision, not a technical one.

No v2 code is written until questions 1 and 2 are measured.
