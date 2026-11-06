# workbench_drive.py - end-to-end verification of the v0.3a3 workbench shell.
#
# Drives the plugin over raw JSON-RPC (this script owns the process lifetime),
# opens the editor from the FIRST result row, then exercises:
#   1. live expression re-evaluation + HEX chip copy
#   2. bit toggle + the four parity chips
#   3. toolbar width/sign changes, sign extension, type-prefix rewrite
#   4. settings tab (real content area), help tab, direct tab-to-tab switching
#      with no "close the overlay first" click
#   5. full untruncated parse error in status row 2
#   6. full-precedence expressions and the session fixed/C eval-mode switch
#   7. float composition bar: toggle, S/E/M copy, f16, ghost and no-container
#   8. screenshots and clean exit 0
#
# Geometry comes from the plugin's stderr "layout:" debug lines; a daemon
# thread accumulates every stderr line so nothing is lost between steps.
import ctypes
import json
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "verify"
OUT.mkdir(parents=True, exist_ok=True)
EXE = str(ROOT / "binarylab.exe")
SHOT = str(ROOT / "tools" / "verify" / "spike_shot.py")
DATA_DIR = tempfile.mkdtemp(prefix="binarylab-hist-")
HIST_FILE = Path(DATA_DIR) / "history.jsonl"
PNG_BITS = str(OUT / "workbench_bits.png")
PNG_PANEL = str(OUT / "workbench_settings.png")
PNG_HELP = str(OUT / "workbench_help.png")
PNG_ERR = str(OUT / "workbench_error.png")
PNG_HOVER = str(OUT / "workbench_hover.png")
PNG_BLANK = str(OUT / "workbench_blank.png")
PNG_HISTORY = str(OUT / "workbench_history.png")
PNG_FLOAT = str(OUT / "workbench_float.png")
PNG_FLOATBAR = str(OUT / "workbench_floatbar.png")
PNG_GHOST = str(OUT / "workbench_floatbar_ghost.png")
PNG_KIND = str(OUT / "workbench_float_toolbar.png")

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
user32.SetProcessDPIAware()
user32.FindWindowW.restype = ctypes.c_void_p
user32.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
user32.FindWindowExW.restype = ctypes.c_void_p
user32.FindWindowExW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p]
user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_size_t]
user32.GetClipboardData.restype = ctypes.c_void_p
user32.GetClipboardData.argtypes = [ctypes.c_uint]
kernel32.GlobalAlloc.restype = ctypes.c_void_p
kernel32.GlobalAlloc.argtypes = [ctypes.c_uint, ctypes.c_size_t]
kernel32.GlobalLock.restype = ctypes.c_void_p
kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
kernel32.GlobalSize.restype = ctypes.c_size_t
kernel32.GlobalSize.argtypes = [ctypes.c_void_p]


def read_clipboard():
    if not user32.OpenClipboard(None):
        return None
    try:
        h = user32.GetClipboardData(13)  # CF_UNICODETEXT
        if not h:
            return None
        size = kernel32.GlobalSize(h)
        p = kernel32.GlobalLock(h)
        if not p:
            return None
        try:
            return ctypes.wstring_at(p, size // 2).rstrip("\x00")
        finally:
            kernel32.GlobalUnlock(h)
    finally:
        user32.CloseClipboard()


def post_click(hwnd, x, y):
    lp = (int(y) << 16) | (int(x) & 0xFFFF)
    user32.PostMessageW(hwnd, 0x0201, 1, lp)  # WM_LBUTTONDOWN
    user32.PostMessageW(hwnd, 0x0202, 0, lp)  # WM_LBUTTONUP


def post_move(hwnd, x, y):
    lp = (int(y) << 16) | (int(x) & 0xFFFF)
    user32.PostMessageW(hwnd, 0x0200, 0, lp)  # WM_MOUSEMOVE


WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_SYSKEYDOWN, WM_SYSKEYUP = 0x0104, 0x0105
WM_CHAR = 0x0102
VK_RETURN, VK_UP, VK_DOWN = 0x0D, 0x26, 0x28
VK_BACK, VK_DELETE, VK_ESCAPE, VK_TAB = 0x08, 0x2E, 0x1B, 0x09


def post_key(hwnd, vk):
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, 0)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, 0)


def post_alt_key(hwnd, vk):
    user32.PostMessageW(hwnd, WM_SYSKEYDOWN, vk, 0)
    user32.PostMessageW(hwnd, WM_SYSKEYUP, vk, 0)


def post_ctrl_char(hwnd, code):
    user32.PostMessageW(hwnd, WM_CHAR, code, 0)


def edit_text(hwnd):
    buf = ctypes.create_unicode_buffer(512)
    user32.SendMessageW(hwnd, 0x000D, 512, buf)  # WM_GETTEXT
    return buf.value


def shoot(path):
    r = subprocess.run([sys.executable, SHOT, path], capture_output=True, text=True)
    print("shot:", r.stdout.strip(), r.stderr.strip())
    return r.returncode == 0


p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
stderr_lines = []


def pump_stderr():
    for line in p.stderr:
        stderr_lines.append(line)
        if "copyBase" not in line:
            sys.stdout.write("stderr: " + line)
            sys.stdout.flush()


threading.Thread(target=pump_stderr, daemon=True).start()


def n_layouts():
    return len([l for l in stderr_lines if "layout:" in l])


def layout(min_seen):
    """Wait until at least min_seen layout lines appeared, parse the newest."""
    deadline = time.time() + 5
    while time.time() < deadline:
        lines = [l for l in stderr_lines if "layout:" in l]
        if len(lines) > min_seen:
            m = re.search(r"layout: (.+)", lines[-1])
            d = {}
            for kv in m.group(1).split():
                k, v = kv.split("=", 1)
                d[k] = v
            return d
        time.sleep(0.05)
    raise RuntimeError("no layout line after step")


def rect(d, key):
    l, t, w, h = [int(v) for v in d[key].split(",")]
    return l, t, w, h


def click_rect(d, key):
    l, t, w, h = rect(d, key)
    post_click(hwnd, l + w // 2, t + h // 2)


def copy_chip(d, idx):
    click_rect(d, f"chip{idx}")
    time.sleep(0.35)
    return read_clipboard()


def send(o):
    p.stdin.write(json.dumps(o, ensure_ascii=False) + "\n")
    p.stdin.flush()


def recv(rid):
    while True:
        line = p.stdout.readline()
        if not line:
            raise RuntimeError("plugin closed stdout")
        o = json.loads(line)
        if o.get("id") == rid:
            return o


WM_SETTEXT, WM_GETTEXT = 0x000C, 0x000D

send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
      "params": {"protocolVersion": "1", "winshun": "0.7.0", "pluginDir": ".", "dataDir": DATA_DIR,
                 "locale": "zh", "theme": "dark",
                 "settings": {"formatDir": str(ROOT / "tests" / "fixtures")}}})
recv(1)
# ---- 0: `bits ` with no words must offer a REAL, actionable first row ----
# The host sends command=bits, text="", raw="bits " for a keyword + space.
send({"jsonrpc": "2.0", "id": 20, "method": "query",
      "params": {"queryId": 20, "command": "bits", "trigger": "keyword", "keyword": "bits",
                 "text": "", "raw": "bits ", "limit": 200}})
blank = recv(20)["result"]["items"]
print("empty-query first row:", blank[0].get("title"))
e0 = blank[0]
assert e0["id"] == "bits.open.workbench.empty", "empty keyword state needs a workbench entry"
assert e0.get("valid", True), "an invalid row cannot be actioned by WinShun"
assert e0["actions"][0]["id"] == "openEditor" and e0["actions"][0]["key"] == "Enter", \
    "Enter on the first row must open the workbench"
assert e0["data"]["expression"] == "" and e0["data"]["width"] == 32, \
    "entry opens with a blank expression and the default width"
assert len(blank) > 1 and e0["score"] > blank[1]["score"], "entry must outrank the examples"

# Same state when the host leaves `command` empty and only supplies raw text.
send({"jsonrpc": "2.0", "id": 21, "method": "query",
      "params": {"queryId": 21, "command": "", "trigger": "keyword", "keyword": "",
                 "text": "", "raw": "bits ", "limit": 200}})
blank_raw = recv(21)["result"]["items"]
assert blank_raw[0]["id"] == "bits.open.workbench.empty", "raw fallback must resolve the keyword"

# ---- 0b: open from that row; the expression box must be blank ----
send({"jsonrpc": "2.0", "id": 22, "method": "action",
      "params": {"command": "bits", "action": "openEditor", "item": e0, "text": ""}})
recv(22)
time.sleep(1.0)
hwnd0 = user32.FindWindowW("WinShunBinaryLabEditor", None)
assert hwnd0, "blank-state entry must open the editor"
edit0 = user32.FindWindowExW(hwnd0, None, "EDIT", None)
buf0 = ctypes.create_unicode_buffer(256)
user32.SendMessageW(edit0, WM_GETTEXT, 256, buf0)
print("blank expression box:", repr(buf0.value))
assert buf0.value == "", f"entry row must leave the expression blank, got {buf0.value!r}"
lay_blank = layout(0)
assert int(lay_blank["clientH"]) == 446, "blank entry uses the default 32-bit width"
assert shoot(PNG_BLANK)

# ---- 0c: the ordinary expression entry still opens with its expression ----
send({"jsonrpc": "2.0", "id": 23, "method": "query",
      "params": {"queryId": 23, "command": "bits", "trigger": "keyword", "keyword": "bits",
                 "text": "u32 0xA5 & 0x0F", "raw": "bits u32 0xA5 & 0x0F", "limit": 200}})
items = recv(23)["result"]["items"]
print("first row:", items[0].get("title"))
assert items[0]["id"] == "bits.open.workbench", "first result row must be the workbench entry"
n_before_reopen = len([l for l in stderr_lines if "layout:" in l])
send({"jsonrpc": "2.0", "id": 24, "method": "action",
      "params": {"command": "bits", "action": "openEditor", "item": items[0],
                 "text": "u32 0xA5 & 0x0F"}})
recv(24)
time.sleep(1.0)

hwnd = user32.FindWindowW("WinShunBinaryLabEditor", None)
if not hwnd:
    print("FAIL: window not found")
    sys.exit(1)
print("hwnd:", hwnd)
lay = layout(n_before_reopen)
print("layout:", lay)
assert int(lay["mode"]) == 0
assert int(lay["clientH"]) == 446, f"u32 two-row status bar -> 446, got {lay['clientH']}"
assert "parity0" in lay and "tab3" in lay and "tab4" in lay

edit = user32.FindWindowExW(hwnd, None, "EDIT", None)
print("edit:", edit)
buf = ctypes.create_unicode_buffer(256)
user32.SendMessageW(edit, WM_GETTEXT, 256, buf)
assert buf.value == "u32 0xA5 & 0x0F", f"expression lost on reopen: {buf.value!r}"

# ---- 1: live re-evaluation + HEX chip ----
user32.SendMessageW(edit, WM_SETTEXT, 0, "0xFF ^ 0x0F")
time.sleep(0.5)
cl, ct, cw, ch = rect(lay, "chip0")
assert copy_chip(lay, 0) == "0x000000F0", "HEX chip must copy the evaluated value"

# ---- 2: bit toggle (0xF0 -> 0xF1) ----
gl, gt = int(lay["gridLeft"]), int(lay["gridTop"])
cell = int(lay["cellW"])
assert gl + 8 * cell + 8 - 4 >= int(lay["clientW"]) - 16 - 8, "grid must fill the width"
post_click(hwnd, gl + 7 * cell + 8 + cell // 2, gt + 3 * 52 + 14 + 16)
time.sleep(0.35)
assert copy_chip(lay, 0) == "0x000000F1", "bit 0 toggle"

# ---- 3: parity chips (0xF1 has 5 ones -> odd, even-bit 1, odd-bit 0) ----
for idx, expect in ((0, "5"), (1, "奇"), (2, "1"), (3, "0")):
    l, t, w, h = rect(lay, f"parity{idx}")
    post_click(hwnd, l + w // 2, t + h // 2)
    time.sleep(0.3)
    got = read_clipboard()
    print(f"parity{idx}:", repr(got))
    assert got == expect, f"parity{idx}: expected {expect!r}, got {got!r}"
# hover a chip: its definition replaces the row-1 context text
l, t, w, h = rect(lay, "parity2")
post_move(hwnd, l + w // 2, t + h // 2)
time.sleep(0.3)
assert shoot(PNG_HOVER)
post_move(hwnd, 10, 10)
time.sleep(0.2)

# ---- 4: toolbar 8 -> shrink (min 280), then i8, then 64 sign extension ----
n = n_layouts()
click_rect(lay, "w8")
time.sleep(0.5)
lay8 = layout(n)
print("u8 layout:", lay8["clientW"], lay8["clientH"])
assert int(lay8["clientH"]) == 280, f"u8 must hit kMinClientH=280, got {lay8['clientH']}"
assert copy_chip(lay8, 0) == "0xF1"

click_rect(lay8, "sgn1")
time.sleep(0.35)
assert copy_chip(lay8, 3) == "-15", "i8 DEC"
assert copy_chip(lay8, 2) == "0b11110001", "BIN chip must copy BIN"

n = n_layouts()
click_rect(lay8, "w64")
time.sleep(0.5)
lay64 = layout(n)
assert copy_chip(lay64, 0) == "0xFFFFFFFFFFFFFFF1", "sign extension into i64"

# ---- 5: type prefix rewrite from the toolbar ----
n = n_layouts()
user32.SendMessageW(edit, WM_SETTEXT, 0, "u32 0x1234")
time.sleep(0.5)
lay32 = layout(n)
l8, t8, w8_, h8 = rect(lay32, "w8")
post_click(hwnd, l8 + w8_ + w8_ // 2, t8 + h8 // 2)  # "16" is the 2nd segment
time.sleep(0.5)
buf = ctypes.create_unicode_buffer(256)
user32.SendMessageW(edit, WM_GETTEXT, 256, buf)
print("expr after toolbar 16:", repr(buf.value))
assert buf.value == "u16 0x1234", f"prefix not rewritten: {buf.value!r}"
lay16 = layout(n)
assert copy_chip(lay16, 0) == "0x1234"

# ---- 6: settings/help are real tabs; no overlay to close first ----
n = n_layouts()
click_rect(lay, "tab3")
time.sleep(0.5)
lay_set = layout(n)
print("settings tab:", lay_set["mode"], lay_set["clientH"], lay_set["chip0"])
assert int(lay_set["mode"]) == 3 and lay_set["chip0"] == "none"
assert int(lay_set["clientH"]) == 280
assert shoot(PNG_PANEL)

n_before = len([l for l in stderr_lines if "setType u8" in l])
click_rect(lay_set, "setw0")   # settings tab width button 8
time.sleep(0.4)
assert len([l for l in stderr_lines if "setType u8" in l]) > n_before, "settings width button must call setType"

n = n_layouts()
click_rect(lay, "tab4")
time.sleep(0.5)
lay_help = layout(n)
assert int(lay_help["mode"]) == 4, "help tab"
assert shoot(PNG_HELP)

n = n_layouts()
click_rect(lay, "tab1")        # DIRECT from help: old overlay swallowed this click
time.sleep(0.5)
lay_bytes = layout(n)
assert int(lay_bytes["mode"]) == 1, "a tab click must never be swallowed by an overlay"

n = n_layouts()
click_rect(lay, "tab0")
time.sleep(0.5)
lay_back = layout(n)
assert int(lay_back["mode"]) == 0 and lay_back["chip0"] != "none"

# ---- 7: full untruncated parse error ----
user32.SendMessageW(edit, WM_SETTEXT, 0, "0xZZ")
time.sleep(0.6)
errs = [l.strip() for l in stderr_lines if "evalError " in l]
print("error lines:", errs[-1:] if errs else [])
assert errs, "parse failure must be reported"
err = errs[-1].split("evalError ", 1)[1]
assert len(err) > 40 and ": " in err and "…" not in err, f"error text was trimmed: {err!r}"
assert shoot(PNG_ERR)
user32.SendMessageW(edit, WM_SETTEXT, 0, "u32 0x1234")
time.sleep(0.5)

# ---- 7b: Enter submits and records CONFIRMED history only ------------------
def hist_lines():
    return [l for l in stderr_lines if "history append expr=" in l]


assert not HIST_FILE.exists(), "live EN_CHANGE evaluation must not write history"
for expr in ("u32 0x1234", "4M", "4Mi"):
    user32.SendMessageW(edit, WM_SETTEXT, 0, expr)
    time.sleep(0.35)
    before = len(hist_lines())
    post_key(edit, VK_RETURN)
    time.sleep(0.35)
    assert len(hist_lines()) > before, f"Enter must submit {expr!r}"
print("history file:", HIST_FILE, HIST_FILE.exists())
assert HIST_FILE.exists(), "history must persist in the host dataDir"
lines = HIST_FILE.read_text(encoding="utf-8").splitlines()
assert len(lines) == 3, f"expected 3 confirmed expressions, got {len(lines)}"
assert '"expr":"4Mi"' in lines[-1], "JSONL stores the expression verbatim"
last_layout = [l for l in stderr_lines if "layout:" in l][-1]
assert "histPersist=1" in last_layout, "dataDir is writable, not session-only"

before = len(hist_lines())
post_key(edit, VK_RETURN)  # same expression again -> adjacent duplicate
time.sleep(0.35)
assert len(hist_lines()) == before, "resubmitting the same expression is a no-op"
assert len(HIST_FILE.read_text(encoding="utf-8").splitlines()) == 3

# ---- 7b2: bash-style editing shortcuts in the expression line ---------------
user32.SendMessageW(edit, WM_SETTEXT, 0, "u32 0xA5 & 0x0F")
time.sleep(0.4)
# caret to the end (Ctrl+E), then Ctrl+Backspace kills separators + one word
post_ctrl_char(edit, 0x05)
post_ctrl_char(edit, 0x7F)
time.sleep(0.4)
assert edit_text(edit) == "u32 0xA5 & ", f"Ctrl+Backspace word kill, got {edit_text(edit)!r}"
post_ctrl_char(edit, 0x7F)
time.sleep(0.3)
# bash semantics: the kill spans every separator plus the previous word
assert edit_text(edit) == "u32 ", f"second word kill, got {edit_text(edit)!r}"
# Ctrl+Y yanks the last killed span back
post_ctrl_char(edit, 0x19)
time.sleep(0.4)
assert edit_text(edit) == "u32 0xA5 & ", f"Ctrl+Y yank, got {edit_text(edit)!r}"
# Ctrl+U kills everything (yankable), Ctrl+L clears too
post_ctrl_char(edit, 0x15)
time.sleep(0.3)
assert edit_text(edit) == "", f"Ctrl+U kill-all, got {edit_text(edit)!r}"
user32.SendMessageW(edit, WM_SETTEXT, 0, "0xFF + 0x01")
time.sleep(0.3)
post_ctrl_char(edit, 0x0C)
time.sleep(0.3)
assert edit_text(edit) == "", f"Ctrl+L clear, got {edit_text(edit)!r}"
# Ctrl+A / Ctrl+E move the caret; verify via EM_GETSEL after Ctrl+A then a char
user32.SendMessageW(edit, WM_SETTEXT, 0, "0xFF + 0x01")
time.sleep(0.3)
post_ctrl_char(edit, 0x01)
sel = ctypes.c_uint()
sel_end = ctypes.c_uint()
user32.SendMessageW(edit, 0x00B0, ctypes.byref(sel), ctypes.byref(sel_end))
assert sel.value == 0 and sel_end.value == 0, "Ctrl+A must move the caret to line start"
post_ctrl_char(edit, 0x05)
user32.SendMessageW(edit, 0x00B0, ctypes.byref(sel), ctypes.byref(sel_end))
assert sel.value == len("0xFF + 0x01"), "Ctrl+E must move the caret to line end"
# Esc clears a non-empty line; Esc on the empty line closes the window
post_key(edit, VK_ESCAPE)
time.sleep(0.4)
assert edit_text(edit) == "", f"Esc clears the line, got {edit_text(edit)!r}"

# ---- 7c: Alt+Up/Down browse history; Down past the newest restores the draft
user32.SendMessageW(edit, WM_SETTEXT, 0, "1")
time.sleep(0.3)
post_alt_key(edit, VK_UP)
time.sleep(0.3)
assert edit_text(edit) == "4Mi", f"Alt+Up -> newest, got {edit_text(edit)!r}"
post_alt_key(edit, VK_UP)
time.sleep(0.3)
assert edit_text(edit) == "4M", "Alt+Up -> older"
post_alt_key(edit, VK_DOWN)
time.sleep(0.3)
assert edit_text(edit) == "4Mi", "Alt+Down -> newer"
post_alt_key(edit, VK_DOWN)
time.sleep(0.3)
assert edit_text(edit) == "1", "Alt+Down past the newest restores the typed draft"

# ---- 7d: the dropdown button opens and a click recalls an expression ----
user32.SendMessageW(edit, WM_SETTEXT, 0, "0x1")
time.sleep(0.3)
click_rect(lay, "histBtn")
time.sleep(0.5)
assert shoot(PNG_HISTORY)
hl, ht, hw_, hh = rect(lay, "hist0")
n_hist_clicks = len([l for l in stderr_lines if "histClick" in l])
post_click(hwnd, hl + 60, ht + hh // 2)
time.sleep(0.5)
assert len([l for l in stderr_lines if "histClick" in l]) > n_hist_clicks, \
    "the dropdown click must reach the window"
assert edit_text(edit) == "4Mi", f"dropdown recall, got {edit_text(edit)!r}"

# ---- 7e: unit suffixes (SI 1000 vs IEC 1024) -------------------------------
for expr, want in (("4M", "0x003D0900"), ("4Mi", "0x00400000"), ("4MB", "0x003D0900")):
    user32.SendMessageW(edit, WM_SETTEXT, 0, expr)
    time.sleep(0.4)
    got = copy_chip(lay, 0)
    print("unit", expr, "->", got)
    assert got == want, f"{expr}: expected {want}, got {got!r}"

# ---- 7f: IEEE-754 bit patterns and the FLOAT chip ---------------------------
n = n_layouts()
user32.SendMessageW(edit, WM_SETTEXT, 0, "f64 1.5")
time.sleep(0.6)
lay_f = layout(n)  # u32 -> f64 widens the window
assert int(lay_f["float"]) == 1, "float mode active"
assert copy_chip(lay_f, 0) == "0x3FF8000000000000", "f64 1.5 bit pattern"
assert copy_chip(lay_f, 3) == "1.5", "FLOAT chip copies the decoded text"
assert shoot(PNG_FLOAT)
user32.SendMessageW(edit, WM_SETTEXT, 0, "1u")
time.sleep(0.5)
assert copy_chip(lay_f, 0) == "0x3EB0C6F7A0B5ED8D", "1u = micro = 1e-6"

# ---- 7g: settings tab clears history ---------------------------------------
n = n_layouts()
click_rect(lay, "tab3")
time.sleep(0.4)
lay_clear = layout(n)
click_rect(lay_clear, "clearHist")
time.sleep(0.4)
assert any("history cleared" in l for l in stderr_lines), "clear-history button"
assert HIST_FILE.read_text(encoding="utf-8") == "", "history file truncated"
n = n_layouts()
click_rect(lay, "tab0")
time.sleep(0.4)
layout(n)

# ---- 7h: full-precedence expressions ----------------------------------------
for expr, want in (("u8 1 + 2 * 3", "0x07"),
                   ("u8 (1 + 2) * 3", "0x09"),
                   ("u8 1 << 2 + 1", "0x08"),
                   ("u8 7 / 2", "0x03"),
                   ("u8 7 % 2", "0x01"),
                   ("u8 1 << 7", "0x80")):
    user32.SendMessageW(edit, WM_SETTEXT, 0, expr)
    time.sleep(0.4)
    got = copy_chip(lay, 0)
    print("expr", expr, "->", got)
    assert got == want, f"{expr}: expected {want}, got {got!r}"

# ---- 7i: session eval mode switch (fixed <-> C) ------------------------------
# The mode buttons only have geometry on the settings tab; setmode1 is "C" and
# "fixed" sits one 56 px segment to its left.
def goto_tab(idx):
    n_ = n_layouts()
    click_rect(lay, f"tab{idx}")
    time.sleep(0.5)
    return layout(n_)


lay_s = goto_tab(3)
ml, mt, mw_, mh = rect(lay_s, "setmode1")
assert mw_ > 0, "settings tab must report the mode buttons"
n_mode_clicks = len([l for l in stderr_lines if "setEvalMode" in l])
post_click(hwnd, ml + mw_ // 2, mt + mh // 2)  # the "C" segment
time.sleep(0.5)
assert len([l for l in stderr_lines if "setEvalMode c" in l]) > 0, "C-mode click"
lay_c = goto_tab(0)
assert lay_c["cmode"] == "1", "bits page must report cmode=1"
user32.SendMessageW(edit, WM_SETTEXT, 0, "200 + 100")  # no prefix -> C promotions
time.sleep(0.4)
assert copy_chip(lay_c, 0) == "0x0000012C", "C mode: i32 promotion"
user32.SendMessageW(edit, WM_SETTEXT, 0, "u8 200 + 100")  # prefix wins -> fixed wrap
time.sleep(0.4)
assert copy_chip(layout(0), 0) == "0x2C", "explicit type prefix still evaluates fixed"
goto_tab(3)
post_click(hwnd, ml - 56 + mw_ // 2, mt + mh // 2)  # back to "fixed"
time.sleep(0.5)
assert any("setEvalMode fixed" in l for l in stderr_lines), "fixed-mode click"
assert goto_tab(0)["cmode"] == "0", "bits page must report cmode=0"

user32.SendMessageW(edit, WM_SETTEXT, 0, "u32 0xA5 & 0x0F")
time.sleep(0.5)

# ---- 7j: float composition bar ---------------------------------------------
# Off by default (settings={}): no bar row, no segment rects.
lay_nb = layout(0)
assert lay_nb["floatbar"] == "0" and lay_nb["barTop"] == "0", "bar starts hidden"
h_off = int(lay_nb["clientH"])


def bar_copy(d, idx):
    click_rect(d, f"bar{idx}")
    time.sleep(0.35)
    return read_clipboard()


user32.SendMessageW(edit, WM_SETTEXT, 0, "f32 1.5")
time.sleep(0.5)
n = n_layouts()
click_rect(lay_nb, "barToggle")
time.sleep(0.5)
lay_bar = layout(n)
print("bar on:", lay_bar["clientH"], lay_bar["bar0"], lay_bar["bar1"], lay_bar["bar2"])
assert lay_bar["floatbar"] == "1", "toggle turns the bar on"
assert int(lay_bar["clientH"]) == h_off + 34, "only the toggle adds the 34 px bar row"
assert int(lay_bar["barTop"]) > int(lay_bar["gridTop"]), "bar sits under the grid"
assert lay_bar["gridTop"] == lay_nb["gridTop"], "the grid must not move"
segs = [rect(lay_bar, f"bar{i}") for i in range(3)]
assert sum(s[2] for s in segs) == int(lay_bar["clientW"]) - 24, "S|E|M span the row"
assert segs[0][0] < segs[1][0] < segs[2][0], "display order is S | E | M"
# f32 1.5 = 0x3FC00000: S=0, E=0x7F, M=0x400000
assert bar_copy(lay_bar, 0) == "0x0", "S block copies the sign"
assert bar_copy(lay_bar, 1) == "0x7F", "E block copies the biased exponent"
assert bar_copy(lay_bar, 2) == "0x400000", "M block copies the mantissa"
# hover E: the caption names the field, the grid highlights its bits
l, t, w, h = rect(lay_bar, "bar1")
post_move(hwnd, l + w // 2, t + h // 2)
time.sleep(0.3)
assert shoot(PNG_FLOATBAR)
post_move(hwnd, 10, 10)
time.sleep(0.2)

# f16 changes the container: the bar follows the table, height stays the same
n = n_layouts()
user32.SendMessageW(edit, WM_SETTEXT, 0, "f16 1.5")
time.sleep(0.6)
lay_h = layout(n)
assert int(lay_h["clientH"]) - int(lay_h["barTop"]) == int(lay_bar["clientH"]) - int(lay_bar["barTop"])
assert bar_copy(lay_h, 1) == "0xF" and bar_copy(lay_h, 2) == "0x200", "f16 1.5 = 0x3E00"

# integer with a matching width: a dimmed ghost reading of the same bits
n = n_layouts()
user32.SendMessageW(edit, WM_SETTEXT, 0, "u32 0x3FC00000")
time.sleep(0.6)
lay_g = layout(n)
assert lay_g["float"] == "0" and rect(lay_g, "bar1")[2] > 0, "ghost bar for a 32-bit integer"
assert bar_copy(lay_g, 1) == "0x7F", "ghost E block reads the same pattern"
assert shoot(PNG_GHOST)

# 8 bits has no container: grey band, nothing clickable, same row height
n = n_layouts()
user32.SendMessageW(edit, WM_SETTEXT, 0, "u8 0x5")
time.sleep(0.6)
lay_u8 = layout(n)
assert all(rect(lay_u8, f"bar{i}")[2] == 0 for i in range(3)), "no segments without a container"
assert int(lay_u8["barTop"]) > 0, "the bar row stays while the toggle is on"

# toggle off restores the original height
user32.SendMessageW(edit, WM_SETTEXT, 0, "u32 0xA5 & 0x0F")
time.sleep(0.5)
n = n_layouts()
click_rect(lay_bar, "barToggle")
time.sleep(0.5)
lay_off = layout(n)
assert lay_off["floatbar"] == "0" and int(lay_off["clientH"]) == h_off, "toggle off shrinks back"
time.sleep(2.3)  # let the clear-history message fade before the final shot

# ---- 7k: float on the toolbar -------------------------------------------------
# The window only relays out when the width changes, so each step reads the
# newest layout line instead of waiting for a new one; the float flag comes
# from the paint-time log line that every relayout or evaluation emits.
def newest():
    return layout(0)



def expr_after(click_key, d):
    click_rect(d, click_key)
    time.sleep(0.6)
    return edit_text(edit), newest()


def click_width(d, idx):
    l_, t_, w_, h_ = rect(d, "w8")
    post_click(hwnd, l_ + idx * w_ + w_ // 2, t_ + h_ // 2)
    time.sleep(0.6)
    return edit_text(edit), newest()


# f32 88 is now a value: 88.0 = 0x42B00000
user32.SendMessageW(edit, WM_SETTEXT, 0, "f32 88")
time.sleep(0.6)
lay_k = newest()
assert copy_chip(lay_k, 0) == "0x42B00000", "f32 88 encodes 88.0"
assert copy_chip(lay_k, 3) == "88", "FLOAT chip shows 88"
# container switch re-encodes the value
txt, lay_k = click_width(lay_k, 3)      # f64
print("f32 -> f64:", txt)
assert txt == "f64 88", f"container switch rewrites the prefix, got {txt!r}"
assert copy_chip(lay_k, 0) == "0x4056000000000000", "f64 88.0"
txt, lay_k = click_width(lay_k, 1)      # f16
assert txt == "f16 88" and copy_chip(lay_k, 0) == "0x5580", "f16 88.0"
# the 8 button has no container: nothing changes
txt, lay_k = click_width(lay_k, 0)
assert txt == "f16 88", "8 is disabled in float mode"
# float -> unsigned keeps the bits
txt, lay_k = expr_after("kind0", lay_k)
print("f16 -> u16:", txt)
assert txt == "u16 0x5580", f"float -> u keeps the bits, got {txt!r}"
assert copy_chip(lay_k, 3) == "21888", "DEC chip is back: 0x5580 as u16"
# signed reading of a pattern with the top bit set
user32.SendMessageW(edit, WM_SETTEXT, 0, "f32 -1.5")
time.sleep(0.6)
lay_k = newest()
txt, lay_k = expr_after("sgn1", lay_k)
assert txt == "i32 @0xBFC00000", f"float -> i keeps the bits, got {txt!r}"
assert copy_chip(lay_k, 3) == "-1077936128", "signed reading of 0xBFC00000"
# integer -> float keeps the bits
txt, lay_k = expr_after("kind2", lay_k)
assert txt == "f32 0xBFC00000", f"i -> f keeps the bits, got {txt!r}"
assert copy_chip(lay_k, 3) == "-1.5", "the same bits read back as -1.5"
assert shoot(PNG_KIND)
user32.SendMessageW(edit, WM_SETTEXT, 0, "u32 0xA5 & 0x0F")
time.sleep(0.5)

# ---- 7l: a bit edit re-decodes the FLOAT readout ------------------------------
# Painting bits used to leave floatText at the value the expression produced, so
# an all-ones pattern still read 1.2.
user32.SendMessageW(edit, WM_SETTEXT, 0, "f32 1.5")
time.sleep(0.6)
lay_r = newest()
assert copy_chip(lay_r, 3) == "1.5", "f32 1.5 reads back as 1.5"
gl, gt = int(lay_r["gridLeft"]), int(lay_r["gridTop"])
cell = int(lay_r["cellW"])
post_click(hwnd, gl + 7 * cell + 8 + cell // 2, gt + 3 * 52 + 14 + 16)  # bit 0
time.sleep(0.5)
assert copy_chip(lay_r, 0) == "0x3FC00001", "bit 0 of the mantissa toggles"
assert copy_chip(lay_r, 3) == "1.5000001", "FLOAT follows the edited bits"
user32.SendMessageW(edit, WM_SETTEXT, 0, "f32 0xFFFFFFFF")
time.sleep(0.5)
assert copy_chip(lay_r, 3) == "NaN", "an all-ones exponent reads NaN, not a stale number"
user32.SendMessageW(edit, WM_SETTEXT, 0, "u32 0xA5 & 0x0F")
time.sleep(2.3)

# ---- 8: final screenshot and clean shutdown ----
assert shoot(PNG_BITS)
# ---- 8: the bytes page (scalar + record formats) -----------------------------
# Reopens the window from bytes rows; the page owns its own expression text.
PNG_BYTES_SCALAR = str(OUT / "bytes_scalar.png")
PNG_BYTES_RECORDS = str(OUT / "bytes_records.png")
BYTE_HEAD = 18  # header line above the 8 data rows


def click_byte(d, idx):
    off_w, bw, top = int(d["offW"]), int(d["byteW"]), int(d["byteTop"])
    col, row = idx % 16, idx // 16
    x = off_w + 8 + col * bw + (8 if col >= 8 else 0) + bw // 2
    y = top + BYTE_HEAD + row * 20 + 10
    post_click(hwnd, x, y)


def bytes_lay(min_seen):
    return layout(min_seen)


def open_bytes(expr, rid):
    global hwnd, eb
    n_before = len([l for l in stderr_lines if "layout:" in l])
    send({"jsonrpc": "2.0", "id": rid, "method": "query",
          "params": {"queryId": rid, "command": "bytes", "trigger": "keyword", "keyword": "bytes",
                     "text": expr, "raw": "bytes " + expr, "limit": 200}})
    items = recv(rid)["result"]["items"]
    assert items[0]["id"] == "bytes.open.workbench", f"first bytes row must be the workbench entry: {items[0]['id']}"
    send({"jsonrpc": "2.0", "id": rid + 1, "method": "action",
          "params": {"command": "bytes", "action": "openEditor", "item": items[0], "text": "bytes " + expr}})
    recv(rid + 1)
    time.sleep(1.2)
    hwnd = user32.FindWindowW("WinShunBinaryLabEditor", None)
    assert hwnd, "the bytes entry must open the editor window"
    eb = user32.FindWindowExW(hwnd, None, "EDIT", None)
    return bytes_lay(n_before)


lay_b = open_bytes("hex u16le 34 12 78 56", 300)
assert int(lay_b["mode"]) == 1 and lay_b["bytesMode"] == "scalar", "bytes page opens in scalar mode"
assert int(lay_b["clientH"]) == int(lay_b["statusTop"]) + 46, "bytes page keeps the two-row status bar"
assert edit_text(eb) == "hex u16le 34 12 78 56", f"expression carried over: {edit_text(eb)!r}"

# click byte #2 (0x78): the u16le word there is 78 56 -> 0x5678
click_byte(lay_b, 2)
time.sleep(0.4)
lay_b = bytes_lay(0)
assert lay_b["selBegin"] == "2" and lay_b["selEnd"] == "2", "single click selects one byte"
l, t, w, h = rect(lay_b, "chip0")
post_click(hwnd, l + w // 2, t + h // 2)
time.sleep(0.35)
assert read_clipboard() == "0x5678", f"word readout must be LE: got {read_clipboard()!r}"

# BE rewrites the suffix and re-reads the same bytes
l, t, w, h = rect(lay_b, "endianBtn1")
post_click(hwnd, l + w // 2, t + h // 2)
time.sleep(0.4)
lay_b = bytes_lay(0)
assert edit_text(eb) == "hex u16be 34 12 78 56", f"BE must rewrite the suffix: {edit_text(eb)!r}"
assert lay_b["endian"] == "be", "state endian follows the click"
l, t, w, h = rect(lay_b, "chip0")
post_click(hwnd, l + w // 2, t + h // 2)
time.sleep(0.35)
assert read_clipboard() == "0x7856", f"BE word readout: got {read_clipboard()!r}"
# back to LE
l, t, w, h = rect(lay_b, "endianBtn0")
post_click(hwnd, l + w // 2, t + h // 2)
time.sleep(0.4)
lay_b = bytes_lay(0)
assert lay_b["endian"] == "le" and edit_text(eb) == "hex u16le 34 12 78 56", "back to LE"

# drag a two-byte selection: selBegin/selEnd follow in the layout log
lp = (int(lay_b["byteTop"]) + BYTE_HEAD + 10) << 16 | (int(lay_b["offW"]) + 8 + 1 * int(lay_b["byteW"]) + 8)
user32.PostMessageW(hwnd, 0x0201, 1, lp)
mp = (int(lay_b["byteTop"]) + BYTE_HEAD + 10) << 16 | (int(lay_b["offW"]) + 8 + 2 * int(lay_b["byteW"]) + 8)
user32.PostMessageW(hwnd, 0x0200, 0, mp)
user32.PostMessageW(hwnd, 0x0202, 0, mp)
time.sleep(0.4)
lay_b = bytes_lay(0)
assert lay_b["selBegin"] == "1" and lay_b["selEnd"] == "2", f"drag selection: {lay_b['selBegin']}..{lay_b['selEnd']}"
assert shoot(PNG_BYTES_SCALAR)

# -> Bits carries the word at the active byte
tl, tt, tw_, th = rect(lay_b, "toBits")
post_click(hwnd, tl + tw_ // 2, tt + th // 2)
time.sleep(0.6)
lay_bits2 = bytes_lay(0)
assert int(lay_bits2["mode"]) == 0, "toBits opens the bits page"
assert edit_text(eb) == "u16 0x5678", f"bits expression from the word: {edit_text(eb)!r}"

# ---- record formats on the bytes page ----------------------------------------
rec = "01 00 00 00 02 00 00 00 fb ff 02 00 03 00 04 00 05 00 06 00 09 00 00 00 00 00 00 00 00 00 00 00"
lay_r = open_bytes(f"fmt blm_rawdata_256 lsb {rec} {rec} aa", 310)
assert int(lay_r["mode"]) == 1 and lay_r["bytesMode"] == "records", "fmt switches to the records view"
assert lay_r["selRecord"] == "0" and lay_r["selField"] == "0", "selection starts at #0"
tl, tt, tw_, th = rect(lay_r, "table")
assert th > 0, "table has geometry"
# click the integral0 column of record #0: columns are measured from column texts
mono, off_w = int(lay_r["monoW"]), int(lay_r["offW"])
w0 = len("wr_time_s") * mono + 24
w1 = len("wr_time_ns") * mono + 24
x = off_w + 8 + w0 + w1 + 30
y = int(lay_r["byteTop"]) + BYTE_HEAD + 10
post_click(hwnd, x, y)
time.sleep(0.4)
lay_r = bytes_lay(0)
assert lay_r["selField"] == "2" and lay_r["selRecord"] == "0", f"cell click: {lay_r['selField']}"
cl, ct, cw, ch = rect(lay_r, "cellChip")
assert cw > 0, "the selected cell strip has geometry"

# MSB rewrites the order word and changes the values
assert edit_text(eb).startswith("fmt blm_rawdata_256 lsb "), f"order word before MSB: {edit_text(eb)!r}"
ol, ot, ow, oh = rect(lay_r, "ord1")
post_click(hwnd, ol + ow // 2, ot + oh // 2)
time.sleep(0.5)
lay_r = bytes_lay(0)
assert edit_text(eb).startswith("fmt blm_rawdata_256 msb "), f"order word after MSB: {edit_text(eb)!r}"
assert any("setOrder msb" in l for l in stderr_lines), "MSB click logged"
assert shoot(PNG_BYTES_RECORDS)

# the format dropdown lists every entry, broken files in red
fl, ft, fw, fh = rect(lay_r, "fmtBtn")
post_click(hwnd, fl + fw // 2, ft + fh // 2)
time.sleep(0.4)
assert shoot(str(OUT / "bytes_formats.png"))
post_key(hwnd, VK_ESCAPE)
time.sleep(0.3)

send({"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": {}})
recv(99)
p.stdin.close()
p.wait(timeout=10)
print("exit code:", p.returncode)
assert p.returncode == 0, "process must exit cleanly"
print("ALL CHECKS PASSED")
