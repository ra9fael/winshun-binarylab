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
   `renfei.binarylab`? Product decision, not a technical one.

No v2 code is written until questions 1 and 2 are measured.
