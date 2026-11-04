// editor.h - spike for the v2 interactive bit editor.
//
// Question this file answers: can a strict-sandbox (low integrity, job
// object) plugin with the `ui` permission create, show and keep a window of
// its own? Everything here is scaffolding: a dedicated GUI thread paints a
// static bit grid of one value; there is no editing, no clipboard and no
// protocol traffic from the window. The v2 editor replaces the paint code
// and adds an interaction state machine; the threading and lifetime model
// below is the part intended to survive.
//
// Threading model: the SDK serve() loop stays on the main thread. The window
// is created, pumped and destroyed on one dedicated thread (Win32 windows are
// thread-affine). The protocol thread talks to it only through the small
// API below, which never blocks longer than the stated timeout. Cleanup runs
// in the plugin's destructor, after serve() has returned.
//
// This file must not write to stdout; report problems through `error` and
// winshun::debug (stderr).

#include "logic.h"

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
    EditorSpec spec;
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
        MultiByteToWideChar(CP_UTF8, 0, text.data(), n, w.data(), n);
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

constexpr int kCell = 40;          // bit cell width
constexpr int kCellH = 34;         // bit cell height
constexpr int kMargin = 24;
constexpr int kHeaderH = 142;      // caption + two value lines above the grid

struct Layout {
    int colsPerRow = 8;
    int rows = 1;
    int clientW = 0;
    int clientH = 0;
};

Layout layoutFor(int width)
{
    Layout l;
    l.colsPerRow = 8;
    l.rows = (width + l.colsPerRow - 1) / l.colsPerRow;
    l.clientW = kMargin * 2 + l.colsPerRow * kCell;
    if (l.clientW < 420) // the two value lines must fit without clipping
        l.clientW = 420;
    l.clientH = kHeaderH + l.rows * (kCellH + 20) + kMargin;
    return l;
}

HFONT makeFont(int height, bool bold)
{
    return CreateFontW(-height, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       FIXED_PITCH | FF_MODERN, L"Consolas");
}

void drawCenteredText(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color)
{
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, const_cast<LPRECT>(&rc), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void paint(HWND hwnd, const EditorSpec& spec)
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

    // Header: caption, then the value in every base.
    HFONT captionFont = makeFont(15, true);
    HFONT valueFont = makeFont(14, false);
    HGDIOBJ oldFont = SelectObject(mem, captionFont);
    RECT line { 0, 12, w, 34 };
    drawCenteredText(mem, line, wideFromUtf8(spec.caption.empty() ? "bit editor" : spec.caption), kPanelText);

    const binlab::Repr r = binlab::represent(spec.value, spec.width, false);
    SelectObject(mem, valueFont);
    // Two lines: the bases line must fit the 8-column grid width. Repr
    // carries its own 0x/0b/0o prefixes; do not add another.
    const std::string bases1 = "HEX " + r.hex + "   OCT " + r.oct;
    const std::string bases2 = "BIN " + r.bin + "   DEC " + r.unsignedDec;
    line = { 0, 38, w, 60 };
    drawCenteredText(mem, line, wideFromUtf8(bases1), kValueText);
    line = { 0, 62, w, 84 };
    drawCenteredText(mem, line, wideFromUtf8(bases2), kValueText);
    line = { 0, 86, w, 108 };
    drawCenteredText(mem, line,
                     wideFromUtf8("width " + std::to_string(spec.width) + " bit \xC2\xB7 spike build \xC2\xB7 Esc closes"),
                     kLabelText);

    // Bit grid: MSB on the left of each byte group, bit 0 at the right end.
    HFONT cellFont = makeFont(15, false);
    HFONT labelFont = makeFont(11, false);
    const Layout l = layoutFor(spec.width);
    int y = kHeaderH;
    for (int row = 0; row < l.rows; ++row) {
        // Index labels above each cell: MSB..LSB of this byte group.
        SelectObject(mem, labelFont);
        for (int col = 0; col < l.colsPerRow; ++col) {
            const int bit = (l.rows - 1 - row) * l.colsPerRow + (l.colsPerRow - 1 - col);
            if (bit >= spec.width)
                continue;
            RECT cell { kMargin + col * kCell, y, kMargin + (col + 1) * kCell, y + 14 };
            drawCenteredText(mem, cell, std::to_wstring(bit), kLabelText);
        }
        SelectObject(mem, cellFont);
        for (int col = 0; col < l.colsPerRow; ++col) {
            const int bit = (l.rows - 1 - row) * l.colsPerRow + (l.colsPerRow - 1 - col);
            if (bit >= spec.width)
                continue;
            const bool on = (spec.value >> bit) & 1;
            RECT cell { kMargin + col * kCell, y + 16, kMargin + (col + 1) * kCell - 4, y + 16 + kCellH };
            HBRUSH fill = CreateSolidBrush(on ? kBitOn : kBitOff);
            FillRect(mem, &cell, fill);
            DeleteObject(fill);
            drawCenteredText(mem, cell, on ? L"1" : L"0", on ? kBitOnText : kBitOffText);
        }
        y += kCellH + 20;
    }

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(cellFont);
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
        paint(hwnd, state().spec);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            DestroyWindow(hwnd);
        return 0;
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
    ensureDpiAwareness();

    WNDCLASSW wc {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc); // ERROR_CLASS_ALREADY_EXISTS is fine; the class matches

    const Layout l = layoutFor(spec.width);
    RECT rc { 0, 0, l.clientW, l.clientH };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    const std::wstring title = wideFromUtf8("Binary Lab"); // the caption carries the expression

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
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
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
        s.spec = spec;
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
    if (closed && s.worker.joinable())
        s.worker.join();
}

} // namespace binlab
