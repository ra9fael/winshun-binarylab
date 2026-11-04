// editor.h - interactive bit editor for the v2 Binary Lab plugin.
//
// This is the editor MVP that the spike (editor.h revision 7ff930e) proved was
// possible: a strict-sandbox (low integrity, job object) plugin with the `ui`
// permission can own and pump a window. The spike only painted a static grid;
// this revision adds the interaction layer on top of the same threading and
// lifetime model, which is what the host UI cannot exercise from outside.
//
// Interaction model
// -----------------
//   * Click a bit cell        -> toggle that bit.
//   * Drag across cells       -> paint them to the value of the first cell.
//   * Arrow keys              -> move the focus cell (Left/Right +-1, Up/Down +-8).
//   * Space                   -> toggle the focused bit.
//   * Ctrl+C / B / D / O      -> copy the value as HEX / BIN / DEC / OCT.
//   * Esc                     -> close.
// The header shows the four bases live; the focused cell gets a highlight.
//
// Threading model: the SDK serve() loop stays on the main thread. The window
// is created, pumped and destroyed on one dedicated thread (Win32 windows are
// thread-affine). All state the window reads or writes lives in EditorState and
// is touched only from that GUI thread, so no lock is needed there; the protocol
// thread talks to it only through the small API below, which never blocks longer
// than the stated timeout. Cleanup runs in the plugin's destructor, after
// serve() has returned.
//
// This file must not write to stdout; report problems through `error` and
// winshun::debug (stderr).

#include "logic.h"
#include "winshun.h"

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace binlab {

struct EditorSpec {
    std::uint64_t value = 0;
    int width = 32;
    std::string caption; // the query text, shown as the window subtitle
    bool zh = true;      // window language: Simplified Chinese or English
};

// True when the window is created and shown. On false, `error` says why
// (CreateWindow failing is exactly what the spike exists to detect).
bool openEditorWindow(const EditorSpec& spec, std::string& error);
bool editorVisible();
// Posts WM_CLOSE and waits up to `timeoutMs` for the GUI thread to finish.
// If the timeout expires the thread is detached (it should never happen;
// the destructor uses this as a last resort so the process can still exit).
void closeEditorAndWait(unsigned timeoutMs);

namespace detail {

struct EditorState {
    std::mutex m;
    std::condition_variable cv;
    std::thread worker;
    HWND hwnd = nullptr;
    bool running = false;  // the GUI thread is alive
    bool visible = false;  // the window was created successfully
    std::string error;
    // Live, editable state. Written only from the GUI thread after open; the
    // protocol thread reads only running/visible/hwnd under the mutex.
    std::uint64_t value = 0;
    int width = 32;
    std::string caption;
    bool zh = true;
    int focusBit = -1;
    bool dragging = false;
    int dragPaint = 0; // 0/1 value painted during a drag
};

EditorState& state()
{
    static EditorState s;
    return s;
}

std::wstring wideFromUtf8(const std::string& text)
{
    if (text.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0)
        // The source length is the BYTE count (text.size()), not the wide-char
        // count n: passing n silently truncated every string containing CJK
        // (n < bytes) at n bytes, leaving NULs that ended DrawTextW early.
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), w.data(), n);
    return w;
}

void ensureDpiAwareness()
{
    static const bool done = [] {
        if (const auto proc = reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(
                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                       "SetProcessDpiAwarenessContext"))))
            return proc(reinterpret_cast<HANDLE>(-4)) != FALSE; // PER_MONITOR_AWARE_V2
        return SetProcessDPIAware() != FALSE;
    }();
    (void)done;
}

const wchar_t kClassName[] = L"WinShunBinaryLabEditor";

// Colors: dark surface so both bit states read clearly.
constexpr COLORREF kBackground = RGB(24, 26, 30);
constexpr COLORREF kPanelText = RGB(232, 234, 237);
constexpr COLORREF kValueText = RGB(127, 209, 168);
constexpr COLORREF kLabelText = RGB(86, 96, 104);
constexpr COLORREF kBitOn = RGB(42, 162, 90);
constexpr COLORREF kBitOnText = RGB(255, 255, 255);
constexpr COLORREF kBitOff = RGB(44, 49, 56);
constexpr COLORREF kBitOffText = RGB(138, 145, 153);
constexpr COLORREF kFocus = RGB(240, 200, 80); // highlighted cell border

constexpr int kCell = 40;          // bit cell width
constexpr int kCellH = 34;         // bit cell height
constexpr int kMargin = 24;
constexpr int kLabelH = 16;        // label strip above each cell row
constexpr int kHeaderH = 142;      // caption + two value lines + two hint lines

struct Layout {
    int colsPerRow = 8;
    int rows = 1;
    int clientW = 0;
    int clientH = 0;
};

Layout layoutFor(int width, int minWidth = 420)
{
    Layout l;
    l.colsPerRow = 8;
    l.rows = (width + l.colsPerRow - 1) / l.colsPerRow;
    if (l.rows < 1)
        l.rows = 1;
    l.clientW = kMargin * 2 + l.colsPerRow * kCell;
    if (l.clientW < minWidth) // header text is measured, never guessed (threadMain)
        l.clientW = minWidth;
    l.clientH = kHeaderH + l.rows * (kCellH + kLabelH + 8) + kMargin;
    return l;
}

// The grid is centered in whatever client width the window ended up with.
int gridLeft(int clientW, int cols)
{
    return (clientW - cols * kCell) / 2;
}

// Consolas has no CJK glyphs and GDI does not fall back per glyph, so any
// string that can contain Chinese must use a CJK-capable face. The value lines
// and the bit grid stay on Consolas (digits only) to keep monospace alignment.
const wchar_t* uiFace(bool zh)
{
    return zh ? L"Microsoft YaHei UI" : L"Segoe UI";
}

HFONT makeFont(int height, bool bold, const wchar_t* face, DWORD pitchFamily)
{
    return CreateFontW(-height, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       pitchFamily, face);
}

HFONT monoFont(int height, bool bold)
{
    return makeFont(height, bold, L"Consolas", FIXED_PITCH | FF_MODERN);
}

HFONT uiFont(int height, bool bold, bool zh)
{
    return makeFont(height, bold, uiFace(zh), DEFAULT_PITCH | FF_DONTCARE);
}

void drawCenteredText(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color)
{
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, const_cast<LPRECT>(&rc), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

int textWidth(HDC dc, HFONT font, const std::wstring& text)
{
    if (text.empty())
        return 0;
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz {};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

// Bit index for a (row, col) cell, MSB first within each byte group.
int bitAt(int rows, int cols, int row, int col)
{
    return (rows - 1 - row) * cols + (cols - 1 - col);
}

int getBit(std::uint64_t value, int bit)
{
    return static_cast<int>((value >> bit) & 1u);
}

void setBit(std::uint64_t& value, int width, int bit, int v)
{
    if (bit < 0 || bit >= width)
        return;
    if (v)
        value |= (1ull << bit);
    else
        value &= ~(1ull << bit);
}

// Map a client point to a valid bit index, or -1 if outside the grid or on a
// cell that exceeds the configured width (the top row is only partially filled
// for widths that are not multiples of 8).
int clientWOf(HWND hwnd)
{
    RECT rc {};
    GetClientRect(hwnd, &rc);
    return rc.right;
}

int hitBit(int x, int y, int width, int clientW)
{
    const Layout l = layoutFor(width);
    const int gx = gridLeft(clientW, l.colsPerRow);
    for (int row = 0; row < l.rows; ++row) {
        const int cellTop = kHeaderH + row * (kCellH + kLabelH + 8);
        const int cellY = cellTop + kLabelH;
        if (y < cellY || y > cellY + kCellH)
            continue;
        for (int col = 0; col < l.colsPerRow; ++col) {
            const int cellLeft = gx + col * kCell;
            if (x < cellLeft || x > cellLeft + kCell)
                continue;
            const int bit = bitAt(l.rows, l.colsPerRow, row, col);
            if (bit >= width)
                return -1;
            return bit;
        }
    }
    return -1;
}

// Native clipboard write. The plugin holds `clipboard:write`; the editor window
// is our own window in our own process, so writing directly through Win32 is the
// pragmatic path (the GUI thread cannot issue a protocol request back to serve).
bool copyToClipboard(HWND hwnd, const std::string& utf8)
{
    const std::wstring w = wideFromUtf8(utf8);
    if (w.empty())
        return false;
    if (!OpenClipboard(hwnd))
        return false;
    EmptyClipboard();
    const std::size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) {
        CloseClipboard();
        return false;
    }
    wchar_t* p = static_cast<wchar_t*>(GlobalLock(h));
    if (!p) {
        GlobalFree(h);
        CloseClipboard();
        return false;
    }
    std::memcpy(p, w.data(), bytes);
    GlobalUnlock(h);
    const bool ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
    CloseClipboard();
    if (!ok)
        GlobalFree(h);
    return ok;
}

void copyBase(HWND hwnd, char which)
{
    EditorState& s = state();
    const binlab::Repr r = binlab::represent(s.value, s.width, false);
    std::string text;
    switch (which) {
    case 'H': text = r.hex; break;
    case 'B': text = r.bin; break;
    case 'O': text = r.oct; break;
    case 'D': text = r.dec; break;
    default: text = r.hex; break;
    }
    copyToClipboard(hwnd, text);
}

void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc {};
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(kBackground);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);

    EditorState& s = state();

    // Header: caption, then the value in every base, a status line and a hint.
    // Fonts are split by content: Consolas only for digit-only lines, a
    // CJK-capable UI face for everything that can contain Chinese.
    HFONT captionFont = uiFont(15, true, s.zh);
    HFONT valueFont = monoFont(14, false);
    HFONT labelFont = monoFont(11, false);
    HFONT statusFont = uiFont(11, false, s.zh);
    HGDIOBJ oldFont = SelectObject(mem, captionFont);
    RECT line { 0, 10, w, 32 };
    std::wstring cap = wideFromUtf8(s.caption.empty() ? (s.zh ? "位编辑器" : "bit editor") : s.caption);
    {
        const int maxCap = w - 2 * kMargin;
        auto fits = [&](const std::wstring& t) {
            SIZE sz {};
            GetTextExtentPoint32W(mem, t.c_str(), static_cast<int>(t.size()), &sz);
            return sz.cx <= maxCap;
        };
        if (!fits(cap)) {
            while (cap.size() > 1 && !fits(cap + L"…"))
                cap.resize(cap.size() - 1);
            cap += L"…";
        }
    }
    drawCenteredText(mem, line, cap, kPanelText);

    const binlab::Repr r = binlab::represent(s.value, s.width, false);
    SelectObject(mem, valueFont);
    const std::string bases1 = "HEX " + r.hex + "   OCT " + r.oct;
    const std::string bases2 = "BIN " + r.bin + "   DEC " + r.dec;
    line = { 0, 36, w, 58 };
    drawCenteredText(mem, line, wideFromUtf8(bases1), kValueText);
    line = { 0, 60, w, 82 };
    drawCenteredText(mem, line, wideFromUtf8(bases2), kValueText);

    SelectObject(mem, statusFont);
    const std::string status = s.zh
        ? "位宽 " + std::to_string(s.width) + " bit"
              + (s.focusBit >= 0 ? " · 焦点位 " + std::to_string(s.focusBit) : "")
        : std::to_string(s.width) + "-bit"
              + (s.focusBit >= 0 ? " · focus bit " + std::to_string(s.focusBit) : "");
    line = { 0, 84, w, 100 };
    drawCenteredText(mem, line, wideFromUtf8(status), kLabelText);
    line = { 0, 104, w, 120 };
    drawCenteredText(mem, line,
                     wideFromUtf8(s.zh ? "点击翻转 · 拖动涂抹 · 方向键移动焦点 · 空格翻转"
                                       : "Click to toggle · drag to paint · arrows move focus · Space toggles"),
                     kLabelText);
    line = { 0, 122, w, 138 };
    drawCenteredText(mem, line,
                     wideFromUtf8(s.zh ? "Ctrl+C/B/D/O 复制 HEX/BIN/DEC/OCT · Esc 关闭"
                                       : "Ctrl+C/B/D/O copy HEX/BIN/DEC/OCT · Esc closes"),
                     kLabelText);

    // Bit grid: MSB on the left of each byte group, bit 0 at the right end.
    HFONT cellFont = monoFont(15, false);
    const Layout l = layoutFor(s.width);
    const int gx = gridLeft(w, l.colsPerRow);
    int y = kHeaderH;
    for (int row = 0; row < l.rows; ++row) {
        const int cellTop = y;
        // Index labels above each cell: MSB..LSB of this byte group.
        SelectObject(mem, labelFont);
        for (int col = 0; col < l.colsPerRow; ++col) {
            const int bit = bitAt(l.rows, l.colsPerRow, row, col);
            if (bit >= s.width)
                continue;
            RECT cell { gx + col * kCell, cellTop, gx + (col + 1) * kCell, cellTop + kLabelH };
            drawCenteredText(mem, cell, std::to_wstring(bit), kLabelText);
        }
        SelectObject(mem, cellFont);
        for (int col = 0; col < l.colsPerRow; ++col) {
            const int bit = bitAt(l.rows, l.colsPerRow, row, col);
            if (bit >= s.width)
                continue;
            const bool on = getBit(s.value, bit) != 0;
            RECT cell { gx + col * kCell, cellTop + kLabelH, gx + (col + 1) * kCell - 4,
                        cellTop + kLabelH + kCellH };
            HBRUSH fill = CreateSolidBrush(on ? kBitOn : kBitOff);
            FillRect(mem, &cell, fill);
            DeleteObject(fill);
            drawCenteredText(mem, cell, on ? L"1" : L"0", on ? kBitOnText : kBitOffText);
            if (bit == s.focusBit) {
                // Highlight the focused cell with a 2px border.
                HGDIOBJ oldPen = SelectObject(mem, CreatePen(PS_SOLID, 2, kFocus));
                RECT border { cell.left + 1, cell.top + 1, cell.right - 1, cell.bottom - 1 };
                MoveToEx(mem, border.left, border.top, nullptr);
                LineTo(mem, border.right, border.top);
                LineTo(mem, border.right, border.bottom);
                LineTo(mem, border.left, border.bottom);
                LineTo(mem, border.left, border.top);
                DeleteObject(SelectObject(mem, oldPen));
            }
        }
        y += kCellH + kLabelH + 8;
    }

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(cellFont);
    DeleteObject(statusFont);
    DeleteObject(labelFont);
    DeleteObject(valueFont);
    DeleteObject(captionFont);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        EditorState& s = state();
        const int bit = hitBit(static_cast<int>(LOWORD(lp)), static_cast<int>(HIWORD(lp)), s.width, clientWOf(hwnd));
        if (bit >= 0) {
            s.focusBit = bit;
            s.dragPaint = getBit(s.value, bit) ? 0 : 1;
            s.dragging = true;
            setBit(s.value, s.width, bit, s.dragPaint);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        EditorState& s = state();
        if (s.dragging) {
            const int bit = hitBit(static_cast<int>(LOWORD(lp)), static_cast<int>(HIWORD(lp)), s.width, clientWOf(hwnd));
            if (bit >= 0)
                setBit(s.value, s.width, bit, s.dragPaint);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        state().dragging = false;
        return 0;
    case WM_KEYDOWN: {
        EditorState& s = state();
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wp == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (ctrl) {
            if (wp == 'C' || wp == 'c') { copyBase(hwnd, 'H'); return 0; }
            if (wp == 'B' || wp == 'b') { copyBase(hwnd, 'B'); return 0; }
            if (wp == 'D' || wp == 'd') { copyBase(hwnd, 'D'); return 0; }
            if (wp == 'O' || wp == 'o') { copyBase(hwnd, 'O'); return 0; }
            return 0;
        }
        if (wp == VK_SPACE) {
            if (s.focusBit >= 0) {
                setBit(s.value, s.width, s.focusBit, getBit(s.value, s.focusBit) ? 0 : 1);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN) {
            int f = s.focusBit < 0 ? s.width - 1 : s.focusBit;
            if (wp == VK_LEFT) f -= 1;
            else if (wp == VK_RIGHT) f += 1;
            else if (wp == VK_UP) f -= 8;
            else f += 8;
            if (f < 0) f = 0;
            if (f >= s.width) f = s.width - 1;
            s.focusBit = f;
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void threadMain(EditorSpec spec)
{
    EditorState& s = state();
    // Seed the live state before the window paints.
    s.value = spec.value;
    s.width = spec.width;
    s.caption = spec.caption;
    s.zh = spec.zh;
    s.focusBit = -1;
    s.dragging = false;

    ensureDpiAwareness();

    WNDCLASSW wc {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc); // ERROR_CLASS_ALREADY_EXISTS is fine; the class matches

    // Size the window from the worst-case rendered header text, never guessed.
    // The value lines can only grow to the full width, so measure an all-ones
    // pattern; the status line is measured with the focus suffix included.
    const std::string hint1 = spec.zh ? "点击翻转 · 拖动涂抹 · 方向键移动焦点 · 空格翻转"
                                      : "Click to toggle · drag to paint · arrows move focus · Space toggles";
    const std::string hint2 = spec.zh ? "Ctrl+C/B/D/O 复制 HEX/BIN/DEC/OCT · Esc 关闭"
                                      : "Ctrl+C/B/D/O copy HEX/BIN/DEC/OCT · Esc closes";
    const std::string status0 = spec.zh ? "位宽 " + std::to_string(spec.width) + " bit · 焦点位 63"
                                        : std::to_string(spec.width) + "-bit · focus bit 63";
    const std::string bases1 = "HEX 0x" + std::string((spec.width + 3) / 4, 'F')
                             + "   OCT 0o" + std::string((spec.width + 2) / 3, '7');
    const std::string bases2 = "BIN 0b" + std::string(spec.width, '1')
                             + "   DEC " + std::string(spec.width > 32 ? 20 : 10, '9');
    int minW = 420;
    {
        HDC dc = GetDC(nullptr);
        HFONT tf = uiFont(11, false, spec.zh);
        HFONT vf = monoFont(14, false);
        for (const std::string* t : { &hint1, &hint2, &status0 }) {
            const int need = textWidth(dc, tf, wideFromUtf8(*t)) + 2 * kMargin;
            if (need > minW)
                minW = need;
        }
        for (const std::string* t : { &bases1, &bases2 }) {
            const int need = textWidth(dc, vf, wideFromUtf8(*t)) + 2 * kMargin;
            if (need > minW)
                minW = need;
        }
        DeleteObject(tf);
        DeleteObject(vf);
        ReleaseDC(nullptr, dc);
    }

    const Layout l = layoutFor(spec.width, minW);
    RECT rc { 0, 0, l.clientW, l.clientH };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    const std::wstring title = spec.zh ? L"二进制工作台" : L"Binary Lab"; // the caption carries the expression

    HWND hwnd = CreateWindowExW(0, kClassName, title.c_str(), WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    {
        std::lock_guard<std::mutex> lock(s.m);
        if (!hwnd) {
            s.error = "CreateWindowW failed (GetLastError " + std::to_string(GetLastError()) + ")";
            s.running = false;
            s.cv.notify_all();
            return;
        }
        s.hwnd = hwnd;
        s.visible = true;
        s.cv.notify_all();
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        try {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } catch (const std::exception& e) {
            winshun::debug(std::string("renfei.binarylab: editor loop: ") + e.what());
        } catch (...) {
            winshun::debug("renfei.binarylab: editor loop: unknown exception");
        }
    }

    std::lock_guard<std::mutex> lock(s.m);
    s.hwnd = nullptr;
    s.visible = false;
    s.running = false;
    s.cv.notify_all();
}

} // namespace detail

bool openEditorWindow(const EditorSpec& spec, std::string& error)
{
    detail::EditorState& s = detail::state();
    {
        std::lock_guard<std::mutex> lock(s.m);
        if (s.running && s.hwnd)
            PostMessageW(s.hwnd, WM_CLOSE, 0, 0); // one window at a time
    }
    closeEditorAndWait(1500);

    error.clear();
    {
        std::lock_guard<std::mutex> lock(s.m);
        if (s.running) { // the previous thread missed its timeout; refuse rather than race it
            error = "previous editor window did not close in time";
            return false;
        }
        s.error.clear();
        s.visible = false;
        s.running = true;
        s.worker = std::thread(detail::threadMain, spec);
    }

    std::unique_lock<std::mutex> lock(s.m);
    const bool ok = s.cv.wait_for(lock, std::chrono::seconds(3), [&] { return s.visible || !s.error.empty(); });
    if (!ok) {
        error = "timed out waiting for the window thread";
        return false;
    }
    error = s.error;
    return s.visible;
}

bool editorVisible()
{
    std::lock_guard<std::mutex> lock(detail::state().m);
    return detail::state().visible;
}

void closeEditorAndWait(unsigned timeoutMs)
{
    detail::EditorState& s = detail::state();
    {
        std::lock_guard<std::mutex> lock(s.m);
        if (s.hwnd)
            PostMessageW(s.hwnd, WM_CLOSE, 0, 0);
    }
    std::unique_lock<std::mutex> lock(s.m);
    const bool closed = s.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !s.running; });
    if (closed && s.worker.joinable()) {
        s.worker.join();
    } else if (!closed && s.worker.joinable()) {
        // The window did not close in time (e.g. it was mid-clipboard or the
        // host is tearing the process down). Detach rather than join: a joinable
        // std::thread left in the static EditorState would call std::terminate()
        // at program exit. The detached thread finishes its own cleanup.
        s.worker.detach();
        winshun::debug("renfei.binarylab: editor close timed out; detached");
    }
}

} // namespace binlab
