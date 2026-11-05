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
#   6. screenshots and clean exit 0
#
# Geometry comes from the plugin's stderr "layout:" debug lines; a daemon
# thread accumulates every stderr line so nothing is lost between steps.
import ctypes
import json
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "verify"
OUT.mkdir(parents=True, exist_ok=True)
EXE = str(ROOT / "binarylab.exe")
SHOT = str(ROOT / "tools" / "verify" / "spike_shot.py")
PNG_BITS = str(OUT / "workbench_bits.png")
PNG_PANEL = str(OUT / "workbench_settings.png")
PNG_HELP = str(OUT / "workbench_help.png")
PNG_ERR = str(OUT / "workbench_error.png")
PNG_HOVER = str(OUT / "workbench_hover.png")
PNG_BLANK = str(OUT / "workbench_blank.png")

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
      "params": {"protocolVersion": "1", "winshun": "0.7.0", "pluginDir": ".", "dataDir": ".",
                 "locale": "zh", "theme": "dark", "settings": {}}})
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

# ---- 8: final screenshot and clean shutdown ----
assert shoot(PNG_BITS)
send({"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": {}})
recv(99)
p.stdin.close()
p.wait(timeout=10)
print("exit code:", p.returncode)
assert p.returncode == 0, "process must exit cleanly"
print("ALL CHECKS PASSED")
