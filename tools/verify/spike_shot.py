# spike_shot.py - capture the editor window to a PNG.
#
# The window is GDI-painted from its own thread, so capture the visible screen
# pixels with BitBlt (PrintWindow's WM_PRINT path does not reach our wndProc).
# No third-party modules: the PNG is assembled with zlib from the standard
# library.
import ctypes
import struct
import sys
import time
import zlib
from ctypes import wintypes

u = ctypes.windll.user32
g = ctypes.windll.gdi32
u.SetProcessDPIAware()

u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
u.GetWindowRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.RECT)]
u.GetDC.restype = ctypes.c_void_p
u.GetDC.argtypes = [ctypes.c_void_p]
u.ReleaseDC.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
u.SetForegroundWindow.argtypes = [ctypes.c_void_p]
u.BringWindowToTop.argtypes = [ctypes.c_void_p]
u.SetWindowPos.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int,
                           ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint]
g.CreateCompatibleDC.restype = ctypes.c_void_p
g.CreateCompatibleDC.argtypes = [ctypes.c_void_p]
g.CreateCompatibleBitmap.restype = ctypes.c_void_p
g.CreateCompatibleBitmap.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
g.SelectObject.restype = ctypes.c_void_p
g.SelectObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
g.BitBlt.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                     ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_uint32]
g.GetDIBits.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint,
                        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
g.DeleteObject.argtypes = [ctypes.c_void_p]
g.DeleteDC.argtypes = [ctypes.c_void_p]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def main(path):
    hwnd = u.FindWindowW("WinShunBinaryLabEditor", None)
    if not hwnd:
        print("no window", file=sys.stderr)
        return 1
    u.BringWindowToTop(hwnd)
    u.SetForegroundWindow(hwnd)
    # SetForegroundWindow is unreliable cross-process. Make the editor
    # topmost for the duration of the capture so no dialog can cover it.
    SWP_NOMOVE, SWP_NOSIZE = 0x0002, 0x0001
    u.SetWindowPos(hwnd, ctypes.c_void_p(0xFFFFFFFFFFFFFFFF), 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE)  # HWND_TOPMOST
    time.sleep(0.15)
    wr = wintypes.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(wr))
    w, h = wr.right - wr.left, wr.bottom - wr.top
    if w <= 0 or h <= 0:
        return 1

    screen = u.GetDC(None)
    memdc = g.CreateCompatibleDC(screen)
    bmp = g.CreateCompatibleBitmap(screen, w, h)
    old = g.SelectObject(memdc, bmp)
    g.BitBlt(memdc, 0, 0, w, h, screen, wr.left, wr.top, 0x00CC0020)  # SRCCOPY

    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h  # top-down rows
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0
    buf = (ctypes.c_ubyte * (w * h * 4))()
    g.GetDIBits(memdc, bmp, 0, h, buf, ctypes.byref(bi), 0)

    px = bytearray(bytes(buf))
    for i in range(0, len(px), 4):  # BGRA -> RGBA
        px[i], px[i + 2] = px[i + 2], px[i]
    row = w * 4
    raw = b"".join(b"\x00" + bytes(px[y * row:(y + 1) * row]) for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)

    g.SelectObject(memdc, old)
    g.DeleteObject(bmp)
    g.DeleteDC(memdc)
    u.ReleaseDC(None, screen)
    u.SetWindowPos(hwnd, ctypes.c_void_p(0xFFFFFFFFFFFFFFFE), 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE)  # HWND_NOTOPMOST
    print(f"captured {w}x{h} -> {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "spike_window.png"))
