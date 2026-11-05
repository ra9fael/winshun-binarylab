// editor.h - the Binary Lab workbench window for the WinShun plugin.
//
// v0.3a: the single-purpose bit editor became a unified workbench shell.
// One window hosts three modes (bits / bytes / struct) behind top-bar tabs;
// only bits is interactive in v0.3a, the other two show a placeholder. The
// shell owns:
//   * top bar      - mode tabs, settings dropdown, help panel (all former
//                    floating hint text lives here now);
//   * input line   - a native EDIT control holding a SpeedCrunch-style
//                    expression re-evaluated on every change through the same
//                    evalBits() the query path uses, so the grid always edits
//                    the expression's result; a type prefix (u8/...) inside
//                    the expression overrides width/signedness;
//   * result row   - HEX/OCT/BIN/DEC chips + copy-all, all mouse-clickable;
//                    the Ctrl+C/B/D/O shortcuts stay;
//   * content area - the interactive bit grid;
//   * status bar   - mode state | selection/focus | transient feedback
//                    (green copies fade ~2 s, red parse errors persist).
//
// Threading model (unchanged from the spike): serve() stays on the main
// thread; the window, its child edit control and all GUI state live on one
// dedicated thread. GUI-thread-only fields need no lock; the protocol thread
// touches running/visible/hwnd/error under the mutex only.
//
// This file must not write to stdout; report problems through `error` and
// winshun::debug (stderr).

#include "logic.h"
#include "winshun.h"

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace binlab {

struct EditorSpec {
    enum class Mode { Bits, Bytes, Struct };
    Mode mode = Mode::Bits;
    std::uint64_t value = 0;       // initial value; the expression overrides it
    int width = 32;                // initial width unless the expression sets one
    bool isSigned = false;         // signedness for the DEC readout
    std::string expression;        // initial expression text (query args)
    std::string endian = "le";     // bytes mode (v0.3b)
    int pack = 0;                  // struct mode (v0.3c)
    bool zh = true;                // window language
};

bool openEditorWindow(const EditorSpec& spec, std::string& error);
bool editorVisible();
void closeEditorAndWait(unsigned timeoutMs);

namespace detail {

constexpr int kTimerId = 1;
constexpr DWORD kMsgFadeMs = 2000;

struct EditorState {
    std::mutex m;
    std::condition_variable cv;
    std::thread worker;
    HWND hwnd = nullptr;
    bool running = false;
    bool visible = false;
    std::string error;

    // ---- GUI-thread-only state ----
    EditorSpec::Mode mode = EditorSpec::Mode::Bits;
    std::uint64_t value = 0;
    int width = 32;
    bool isSigned = false;
    std::string endian = "le";
    int pack = 0;
    bool zh = true;
    int focusBit = -1;
    bool dragging = false;
    int dragPaint = 0;

    HWND edit = nullptr;
    WNDPROC origEditProc = nullptr;
    HFONT editFont = nullptr;
    std::string evalError;

    bool settingsOpen = false;
    bool helpOpen = false;
    std::string msg;
    DWORD msgTick = 0;

    // Cached geometry, recomputed by computeLayout().
    int clientW = 0;
    int clientH = 0;
    RECT rcTabs[3] = {};
    RECT rcSet = {};
    RECT rcHelp = {};
    RECT rcExpr = {};
    RECT rcChip[5] = {};
    int resultRows = 1;
    int gridTop = 0;
    int gridLeft = 0;
    int gridRows = 1;
    int cellW = 40;            // adaptive: the 8 columns fill the client width
    int toolTop = 0;           // width / signedness toolbar row (bits mode)
    RECT rcWidthBtn[4] = {};   // 8 | 16 | 32 | 64
    RECT rcSignBtn[2] = {};    // unsigned | signed
    bool suppressEval = false; // set while the toolbar rewrites the type prefix
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
        // Source length is the BYTE count (text.size()), never the wide-char
        // count n - passing n truncated every CJK string at n bytes.
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), w.data(), n);
    return w;
}

std::string utf8FromWide(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
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

constexpr COLORREF kBackground = RGB(24, 26, 30);
constexpr COLORREF kPanel = RGB(31, 34, 39);     // top bar / status bar surface
constexpr COLORREF kOverlay = RGB(38, 42, 48);   // dropdown / help panel surface
constexpr COLORREF kPanelText = RGB(232, 234, 237);
constexpr COLORREF kValueText = RGB(127, 209, 168);
constexpr COLORREF kLabelText = RGB(120, 128, 136);
constexpr COLORREF kAccent = RGB(88, 166, 255);  // active tab underline, values
constexpr COLORREF kBitOn = RGB(42, 162, 90);
constexpr COLORREF kBitOnText = RGB(255, 255, 255);
constexpr COLORREF kBitOff = RGB(44, 49, 56);
constexpr COLORREF kBitOffText = RGB(138, 145, 153);
constexpr COLORREF kFocus = RGB(240, 200, 80);   // focused bit cell border
constexpr COLORREF kOkText = RGB(127, 209, 168);
constexpr COLORREF kErrText = RGB(240, 128, 128);
constexpr COLORREF kSeparator = RGB(52, 57, 64);

// Shell metrics. Drive scripts parse the layout debug line instead of
// duplicating these, so they may change freely.
constexpr int kTopH = 34;
constexpr int kExprH = 34;
constexpr int kChipH = 24;
constexpr int kChipGap = 6;
constexpr int kMinCellW = 40;   // floor for the adaptive cell width
constexpr int kGridMargin = 16; // left/right margin of the bit grid
constexpr int kNibbleGap = 8;   // extra gap between bit 4 and bit 3 of each byte
constexpr int kToolH = 26;      // width / signedness toolbar
constexpr int kCellH = 32;
constexpr int kLabelH = 14;
constexpr int kRowGap = 6;
constexpr int kStatusH = 24;

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

void drawLeftText(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color)
{
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, const_cast<LPRECT>(&rc), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
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

int rcWidth(const RECT& r)
{
    return r.right - r.left;
}

bool inRect(const RECT& r, int x, int y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

void frameRect(HDC dc, const RECT& r, COLORREF color)
{
    HGDIOBJ old = SelectObject(dc, CreatePen(PS_SOLID, 1, color));
    MoveToEx(dc, r.left, r.top, nullptr);
    LineTo(dc, r.right, r.top);
    LineTo(dc, r.right, r.bottom);
    LineTo(dc, r.left, r.bottom);
    LineTo(dc, r.left, r.top);
    DeleteObject(SelectObject(dc, old));
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

// MSB first within each byte group.
int bitAt(int rows, int cols, int row, int col)
{
    return (rows - 1 - row) * cols + (cols - 1 - col);
}

// Left edge of grid column `col`; a nibble gap splits each byte 4 + 4.
int cellLeft(int col)
{
    const EditorState& s = state();
    return s.gridLeft + col * s.cellW + (col >= 4 ? kNibbleGap : 0);
}

// Worst-case base strings for the current width keep the chip geometry stable
// while the value changes (no layout jitter on every keystroke).
struct ChipTexts {
    std::string label;
    std::string value;
};

std::vector<ChipTexts> chipTextsFor(int width)
{
    return {
        {"HEX ", "0x" + std::string((width + 3) / 4, 'F')},
        {"OCT ", "0o" + std::string((width + 2) / 3, '7')},
        {"BIN ", "0b" + std::string(width, '1')},
        {"DEC ", std::string(width > 32 ? 20 : 10, '9')},
    };
}

// Compute every cached geometry rect from the current state.
void computeLayout(HDC dc)
{
    EditorState& s = state();
    const bool bits = s.mode == EditorSpec::Mode::Bits;

    HFONT tabFont = uiFont(12, false, s.zh);
    const int tabPad = 12;
    int x = 12;
    const char* tabs[3] = { s.zh ? "位运算" : "Bits",
                            s.zh ? "字节流" : "Bytes",
                            s.zh ? "结构体" : "Struct" };
    for (int i = 0; i < 3; ++i) {
        const int tw = textWidth(dc, tabFont, wideFromUtf8(tabs[i]));
        s.rcTabs[i] = { x, 0, x + tw + 2 * tabPad, kTopH };
        x += tw + 2 * tabPad + 2;
    }

    HFONT chipLabelFont = uiFont(11, false, s.zh);
    HFONT chipValueFont = monoFont(13, false);
    std::vector<ChipTexts> chips;
    if (bits) {
        chips = chipTextsFor(s.width);
        chips.push_back({ s.zh ? "全部 " : "All ", "" }); // copy-all
    }
    int oneRow = 0;
    std::vector<int> chipW;
    for (const ChipTexts& c : chips) {
        const int lw = textWidth(dc, chipLabelFont, wideFromUtf8(c.label));
        const int vw = c.value.empty() ? 0 : textWidth(dc, chipValueFont, wideFromUtf8(c.value));
        const int cw = lw + vw + 20;
        chipW.push_back(cw);
        oneRow += cw;
    }
    if (!chips.empty())
        oneRow += static_cast<int>(chips.size() - 1) * kChipGap;
    DeleteObject(chipLabelFont);
    DeleteObject(chipValueFont);

    // Toolbar segment widths (measured, so zh/en both fit).
    HFONT toolFont = uiFont(12, false, s.zh);
    const std::wstring toolWidthLabel = wideFromUtf8(s.zh ? "位宽" : "Width");
    const std::wstring toolSignLabel = wideFromUtf8(s.zh ? "符号" : "Sign");
    const std::wstring signNames[2] = { wideFromUtf8(s.zh ? "无符号 u" : "unsigned u"),
                                        wideFromUtf8(s.zh ? "有符号 i" : "signed i") };
    const int widthLabelW = textWidth(dc, toolFont, toolWidthLabel) + 10;
    const int signLabelW = textWidth(dc, toolFont, toolSignLabel) + 10;
    const int widthBtnW = std::max(36, textWidth(dc, toolFont, L"64") + 20);
    int signBtnW[2];
    for (int i = 0; i < 2; ++i)
        signBtnW[i] = textWidth(dc, toolFont, signNames[i]) + 20;
    DeleteObject(toolFont);

    int clientW = std::max(480, 2 * kGridMargin + 8 * kMinCellW + kNibbleGap);
    if (!chips.empty()) {
        if (oneRow + 24 <= 616)
            clientW = std::max(clientW, oneRow + 24);
        else
            clientW = std::max(clientW, 616);
    }
    s.clientW = clientW;

    // Settings/help buttons hug the right edge of the final client width.
    {
        const int setW = textWidth(dc, tabFont, wideFromUtf8(s.zh ? "设置 ▾" : "Settings ▾")) + 16;
        const int helpW = textWidth(dc, tabFont, wideFromUtf8(s.zh ? "帮助 ?" : "Help ?")) + 16;
        int right = clientW - 12;
        s.rcHelp = { right - helpW, 0, right, kTopH };
        right -= helpW + 8;
        s.rcSet = { right - setW, 0, right, kTopH };
    }
    DeleteObject(tabFont);

    s.resultRows = 0;
    if (bits) {
        const int chipsTop = kTopH + kExprH + 4;
        int cx = 12, cy = chipsTop, row = 0;
        for (std::size_t i = 0; i < chips.size(); ++i) {
            if (cx + chipW[i] > clientW - 12 && cx > 12) {
                ++row;
                cx = 12;
                cy = chipsTop + row * (kChipH + kChipGap);
            }
            s.rcChip[i] = { cx, cy, cx + chipW[i], cy + kChipH };
            cx += chipW[i] + kChipGap;
        }
        s.resultRows = row + 1;
    } else {
        for (RECT& r : s.rcChip)
            r = {};
    }

    s.gridRows = std::max(1, (s.width + 7) / 8);
    int contentTop = kTopH + kExprH + 4 + s.resultRows * (kChipH + kChipGap) + 8;
    if (bits) {
        // Width / signedness toolbar sits directly above the grid.
        s.toolTop = contentTop - 4;
        int tx = kGridMargin + widthLabelW;
        for (int i = 0; i < 4; ++i) {
            s.rcWidthBtn[i] = { tx, s.toolTop, tx + widthBtnW, s.toolTop + kToolH };
            tx += widthBtnW;
        }
        tx += 20 + signLabelW;
        for (int i = 0; i < 2; ++i) {
            s.rcSignBtn[i] = { tx, s.toolTop, tx + signBtnW[i], s.toolTop + kToolH };
            tx += signBtnW[i];
        }
        contentTop = s.toolTop + kToolH + 8;
    } else {
        s.toolTop = 0;
        for (RECT& r : s.rcWidthBtn)
            r = {};
        for (RECT& r : s.rcSignBtn)
            r = {};
    }
    s.gridTop = contentTop + kLabelH;
    s.gridLeft = kGridMargin;
    // Fill the width: 8 cells (each drawn 4px narrower than its pitch) plus
    // the nibble gap span exactly clientW - 2 * margin.
    s.cellW = std::max(kMinCellW, (s.clientW - 2 * kGridMargin - kNibbleGap + 4) / 8);
    s.clientH = s.gridTop + s.gridRows * (kLabelH + kCellH + kRowGap) + 8 + kStatusH;
    s.rcExpr = { 64, kTopH + (kExprH - 24) / 2, s.clientW - 12, kTopH + (kExprH + 24) / 2 };
}

// Emit the geometry for the drive scripts (they parse stderr rather than
// duplicating the metrics above).
void logLayout()
{
    EditorState& s = state();
    char line[768];
    if (s.mode == EditorSpec::Mode::Bits) {
        std::snprintf(line, sizeof(line),
                      "ra9fael.binarylab: layout: clientW=%d clientH=%d gridLeft=%d gridTop=%d cellW=%d "
                      "w8=%d,%d,%d,%d w64=%d,%d,%d,%d sgn1=%d,%d,%d,%d "
                      "chip0=%d,%d,%d,%d chip1=%d,%d,%d,%d chip2=%d,%d,%d,%d chip3=%d,%d,%d,%d chip4=%d,%d,%d,%d "
                      "set=%d,%d,%d,%d help=%d,%d,%d,%d tab1=%d,%d,%d,%d",
                      s.clientW, s.clientH, s.gridLeft, s.gridTop, s.cellW,
                      static_cast<int>(s.rcWidthBtn[0].left), static_cast<int>(s.rcWidthBtn[0].top),
                      static_cast<int>(rcWidth(s.rcWidthBtn[0])), kToolH,
                      static_cast<int>(s.rcWidthBtn[3].left), static_cast<int>(s.rcWidthBtn[3].top),
                      static_cast<int>(rcWidth(s.rcWidthBtn[3])), kToolH,
                      static_cast<int>(s.rcSignBtn[1].left), static_cast<int>(s.rcSignBtn[1].top),
                      static_cast<int>(rcWidth(s.rcSignBtn[1])), kToolH,
                      static_cast<int>(s.rcChip[0].left), static_cast<int>(s.rcChip[0].top),
                      static_cast<int>(rcWidth(s.rcChip[0])), static_cast<int>(s.rcChip[0].bottom - s.rcChip[0].top),
                      static_cast<int>(s.rcChip[1].left), static_cast<int>(s.rcChip[1].top),
                      static_cast<int>(rcWidth(s.rcChip[1])), static_cast<int>(s.rcChip[1].bottom - s.rcChip[1].top),
                      static_cast<int>(s.rcChip[2].left), static_cast<int>(s.rcChip[2].top),
                      static_cast<int>(rcWidth(s.rcChip[2])), static_cast<int>(s.rcChip[2].bottom - s.rcChip[2].top),
                      static_cast<int>(s.rcChip[3].left), static_cast<int>(s.rcChip[3].top),
                      static_cast<int>(rcWidth(s.rcChip[3])), static_cast<int>(s.rcChip[3].bottom - s.rcChip[3].top),
                      static_cast<int>(s.rcChip[4].left), static_cast<int>(s.rcChip[4].top),
                      static_cast<int>(rcWidth(s.rcChip[4])), static_cast<int>(s.rcChip[4].bottom - s.rcChip[4].top),
                      static_cast<int>(s.rcSet.left), static_cast<int>(s.rcSet.top),
                      static_cast<int>(rcWidth(s.rcSet)), static_cast<int>(s.rcSet.bottom - s.rcSet.top),
                      static_cast<int>(s.rcHelp.left), static_cast<int>(s.rcHelp.top),
                      static_cast<int>(rcWidth(s.rcHelp)), static_cast<int>(s.rcHelp.bottom - s.rcHelp.top),
                      static_cast<int>(s.rcTabs[1].left), static_cast<int>(s.rcTabs[1].top),
                      static_cast<int>(rcWidth(s.rcTabs[1])), static_cast<int>(s.rcTabs[1].bottom - s.rcTabs[1].top));
    } else {
        std::snprintf(line, sizeof(line),
                      "ra9fael.binarylab: layout: clientW=%d clientH=%d gridLeft=%d gridTop=%d chip0=none "
                      "set=%d,%d,%d,%d help=%d,%d,%d,%d tab1=%d,%d,%d,%d",
                      s.clientW, s.clientH, s.gridLeft, s.gridTop,
                      static_cast<int>(s.rcSet.left), static_cast<int>(s.rcSet.top),
                      static_cast<int>(rcWidth(s.rcSet)), static_cast<int>(s.rcSet.bottom - s.rcSet.top),
                      static_cast<int>(s.rcHelp.left), static_cast<int>(s.rcHelp.top),
                      static_cast<int>(rcWidth(s.rcHelp)), static_cast<int>(s.rcHelp.bottom - s.rcHelp.top),
                      static_cast<int>(s.rcTabs[1].left), static_cast<int>(s.rcTabs[1].top),
                      static_cast<int>(rcWidth(s.rcTabs[1])), static_cast<int>(s.rcTabs[1].bottom - s.rcTabs[1].top));
    }
    winshun::debug(line);
}

// Recompute the layout and resize the window frame if the client area changed.
void relayout(HWND hwnd)
{
    EditorState& s = state();
    const int oldW = s.clientW, oldH = s.clientH;
    HDC dc = GetDC(hwnd);
    computeLayout(dc);
    ReleaseDC(hwnd, dc);
    if (s.clientW != oldW || s.clientH != oldH) {
        RECT rc { 0, 0, s.clientW, s.clientH };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (s.edit) {
        MoveWindow(s.edit, s.rcExpr.left, s.rcExpr.top,
                   rcWidth(s.rcExpr), s.rcExpr.bottom - s.rcExpr.top, TRUE);
        ShowWindow(s.edit, s.mode == EditorSpec::Mode::Bits ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    logLayout();
    InvalidateRect(hwnd, nullptr, TRUE);
}

int hitBit(int x, int y)
{
    EditorState& s = state();
    for (int row = 0; row < s.gridRows; ++row) {
        const int cellTop = s.gridTop + row * (kLabelH + kCellH + kRowGap);
        if (y < cellTop || y > cellTop + kLabelH + kCellH)
            continue;
        for (int col = 0; col < 8; ++col) {
            const int left = cellLeft(col);
            if (x < left || x >= left + s.cellW - 4)
                continue;
            const int bit = bitAt(s.gridRows, 8, row, col);
            return bit < s.width ? bit : -1;
        }
    }
    return -1;
}

// Native clipboard write. The plugin holds `clipboard:write`; the window is our
// own window in our own process, so writing directly through Win32 is the
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

std::string baseText(char which)
{
    EditorState& s = state();
    const binlab::Repr r = binlab::represent(s.value, s.width, s.isSigned);
    switch (which) {
    case 'H': return r.hex;
    case 'O': return r.oct;
    case 'B': return r.bin;
    case 'D': return r.dec;
    case 'A': return "HEX " + r.hex + "\nOCT " + r.oct + "\nBIN " + r.bin + "\nDEC " + r.dec;
    default: return r.hex;
    }
}

void copyBase(HWND hwnd, char which, const char* label)
{
    EditorState& s = state();
    const std::string text = baseText(which);
    const bool ok = copyToClipboard(hwnd, text);
    winshun::debug(std::string("ra9fael.binarylab: copyBase ") + label + " len=" + std::to_string(text.size())
                   + " ok=" + (ok ? "1" : "0") + " gle=" + std::to_string(GetLastError()));
    if (ok)
        s.msg = s.zh ? std::string("已复制 ") + label + " ✓" : std::string("Copied ") + label;
    else
        s.msg = s.zh ? "复制失败" : "copy failed";
    s.msgTick = GetTickCount();
    InvalidateRect(hwnd, nullptr, TRUE);
}

// Re-evaluate the expression line. Keep the last good value on a parse error
// and surface the failure in the status bar instead of blanking the window.
void evaluate()
{
    EditorState& s = state();
    if (!s.edit || s.mode != EditorSpec::Mode::Bits || s.suppressEval)
        return;
    const int len = GetWindowTextLengthW(s.edit);
    std::wstring w(static_cast<std::size_t>(len > 0 ? len : 0), L'\0');
    if (len > 0)
        GetWindowTextW(s.edit, w.data(), len + 1);
    const std::string expr = binlab::trim(utf8FromWide(w));
    if (expr.empty()) {
        s.evalError.clear();
        return;
    }
    const auto r = binlab::evalBits(expr, s.width, s.isSigned);
    if (r.success) {
        s.evalError.clear();
        s.value = r.value.result;
        // An explicit type prefix inside the expression updates the settings
        // too, so the status bar always names the type actually on display.
        if (r.value.width != s.width || r.value.signedType != s.isSigned) {
            s.width = r.value.width;
            s.isSigned = r.value.signedType;
            relayout(s.hwnd);
            return;
        }
    } else {
        s.evalError = r.failure.code + ": " + r.failure.message;
    }
    InvalidateRect(s.hwnd, nullptr, TRUE);
}

// Change the active type from the toolbar or the settings panel. The bit
// pattern on display is kept (truncated, or extended - sign-extended when the
// old type was signed). A leading type prefix in the expression is rewritten
// so the next re-evaluation agrees with the click instead of reverting it.
void setType(HWND hwnd, int width, bool isSigned)
{
    EditorState& s = state();
    if (width == s.width && isSigned == s.isSigned)
        return;
    std::uint64_t v = s.value & binlab::maskFor(s.width);
    if (s.isSigned && width > s.width)
        v = static_cast<std::uint64_t>(binlab::asSigned(v, s.width));
    s.value = v & binlab::maskFor(width);
    s.width = width;
    s.isSigned = isSigned;
    if (s.focusBit >= width)
        s.focusBit = width - 1;

    if (s.edit) {
        const int len = GetWindowTextLengthW(s.edit);
        std::wstring w(static_cast<std::size_t>(len > 0 ? len : 0), L'\0');
        if (len > 0)
            GetWindowTextW(s.edit, w.data(), len + 1);
        const std::size_t b = w.find_first_not_of(L" \t");
        if (b != std::wstring::npos) {
            std::size_t e = w.find_first_of(L" \t", b);
            if (e == std::wstring::npos)
                e = w.size();
            int pw = 0;
            bool ps = false;
            if (binlab::isBitsType(utf8FromWide(w.substr(b, e - b)), pw, ps)) {
                w.replace(b, e - b, wideFromUtf8(binlab::typeNameFor(width, isSigned)));
                s.suppressEval = true;
                SetWindowTextW(s.edit, w.c_str());
                s.suppressEval = false;
            }
        }
    }
    winshun::debug("ra9fael.binarylab: setType " + binlab::typeNameFor(width, isSigned));
    relayout(hwnd);
}

LRESULT CALLBACK editProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    EditorState& s = state();
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            if (s.settingsOpen || s.helpOpen) {
                s.settingsOpen = s.helpOpen = false;
                InvalidateRect(s.hwnd, nullptr, TRUE);
            } else if (s.hwnd) {
                DestroyWindow(s.hwnd);
            }
            return 0;
        }
        if (wp == VK_TAB) {
            SetFocus(s.hwnd); // hand the keyboard back to the grid
            return 0;
        }
    }
    return CallWindowProc(s.origEditProc, h, msg, wp, lp);
}

// ---- overlay panels (drawn inside the client, closed by clicking away) ------

struct SettingRow {
    std::string label;
    std::string value;
    bool enabled;
};

std::vector<SettingRow> settingRows()
{
    EditorState& s = state();
    return {
        { s.zh ? "位宽" : "Width", std::to_string(s.width), true },
        { s.zh ? "符号" : "Signedness",
          s.isSigned ? (s.zh ? "有符号" : "signed") : (s.zh ? "无符号" : "unsigned"), true },
        // SpeedCrunch semantics choices arrive with the v0.3 expression work.
        { s.zh ? "^ 语义" : "^ semantics", "XOR", false },
        { s.zh ? "端序" : "Endianness", s.endian == "be" ? "BE" : "LE", false }, // bytes, v0.3b
        { "pack", s.pack == 0 ? "natural" : std::to_string(s.pack), false },    // struct, v0.3c
    };
}

constexpr int kOverlayRowH = 24;

RECT overlayRect(int rows, int widthHint)
{
    EditorState& s = state();
    const int h = 34 + rows * kOverlayRowH + 12;
    const int x = (s.clientW - widthHint) / 2;
    const int y = kTopH + 6;
    return { x, y, x + widthHint, y + h };
}

RECT settingsRect()
{
    return overlayRect(static_cast<int>(settingRows().size()), 260);
}

RECT helpRect()
{
    return overlayRect(8, 460);
}

void drawOverlayPanel(HDC dc, const RECT& rc)
{
    HBRUSH fill = CreateSolidBrush(kOverlay);
    FillRect(dc, &rc, fill);
    DeleteObject(fill);
    frameRect(dc, rc, kSeparator);
}

void paintSettings(HDC dc, const RECT& rc)
{
    EditorState& s = state();
    drawOverlayPanel(dc, rc);
    HFONT font = uiFont(12, false, s.zh);
    HGDIOBJ old = SelectObject(dc, font);
    RECT title { rc.left + 14, rc.top + 8, rc.right - 14, rc.top + 26 };
    drawLeftText(dc, title, wideFromUtf8(s.zh ? "设置" : "Settings"), kPanelText);
    const std::vector<SettingRow> rows = settingRows();
    int y = rc.top + 34;
    const int midX = rc.left + rcWidth(rc) / 2;
    for (const SettingRow& row : rows) {
        RECT rl { rc.left + 14, y, midX, y + kOverlayRowH };
        RECT rv { midX, y, rc.right - 14, y + kOverlayRowH };
        drawLeftText(dc, rl, wideFromUtf8(row.label), row.enabled ? kPanelText : kLabelText);
        SetTextColor(dc, row.enabled ? kAccent : kLabelText);
        DrawTextW(dc, wideFromUtf8(row.value).c_str(), -1, &rv,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        y += kOverlayRowH;
    }
    SelectObject(dc, old);
    DeleteObject(font);
}

void paintHelp(HDC dc, const RECT& rc)
{
    EditorState& s = state();
    drawOverlayPanel(dc, rc);
    HFONT font = uiFont(12, false, s.zh);
    HGDIOBJ old = SelectObject(dc, font);
    RECT title { rc.left + 14, rc.top + 8, rc.right - 14, rc.top + 26 };
    drawLeftText(dc, title, wideFromUtf8(s.zh ? "帮助 · 语法与快捷键" : "Help · syntax and keys"), kPanelText);
    const char* lines[] = {
        s.zh ? "表达式：0x/0b/0o 进制前缀 · & | ^ ~ << >>" : "Expr: 0x/0b/0o prefixes · & | ^ ~ << >>",
        s.zh ? "        rol/ror 循环移位 · @ 按位重解释 · 7:4 位段" : "        rol/ror rotates · @ reinterpret · 7:4 fields",
        s.zh ? "        get/set/clear/toggle 0xA5 7:4 [value]" : "        get/set/clear/toggle 0xA5 7:4 [value]",
        s.zh ? "鼠标：点击位格翻转 · 拖动涂抹 · 点击读数复制" : "Mouse: click toggles · drag paints · click a chip to copy",
        s.zh ? "键盘：方向键移动焦点 · 空格翻转" : "Keys: arrows move focus · Space toggles",
        s.zh ? "        Ctrl+C/B/D/O 复制 HEX/OCT/BIN/DEC" : "        Ctrl+C/B/D/O copy HEX/OCT/BIN/DEC",
        s.zh ? "        Tab 回到表达式 · Esc 关闭" : "        Tab returns to the expression · Esc closes",
        s.zh ? "设置：顶部位宽/符号按钮随时切换；类型前缀（如 u8）同步改写" : "Settings: toolbar switches width/sign; a type prefix (u8) is kept in sync",
    };
    int y = rc.top + 34;
    for (const char* line : lines) {
        RECT lr { rc.left + 14, y, rc.right - 14, y + 22 };
        drawLeftText(dc, lr, wideFromUtf8(line), kLabelText);
        y += 22;
    }
    SelectObject(dc, old);
    DeleteObject(font);
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
    const bool bits = s.mode == EditorSpec::Mode::Bits;

    HFONT tabFont = uiFont(12, false, s.zh);
    HFONT chipLabelFont = uiFont(11, false, s.zh);
    HFONT chipValueFont = monoFont(13, false);
    HFONT labelFont = monoFont(11, false);
    HFONT cellFont = monoFont(15, false);
    HFONT statusFont = uiFont(11, false, s.zh);
    HFONT exprLabelFont = uiFont(12, false, s.zh);

    // ---- top bar ----
    RECT topRect { 0, 0, w, kTopH };
    HBRUSH panel = CreateSolidBrush(kPanel);
    FillRect(mem, &topRect, panel);
    DeleteObject(panel);
    {
        HGDIOBJ old = SelectObject(mem, CreatePen(PS_SOLID, 1, kSeparator));
        MoveToEx(mem, 0, kTopH, nullptr);
        LineTo(mem, w, kTopH);
        DeleteObject(SelectObject(mem, old));
    }
    SelectObject(mem, tabFont);
    const char* tabs[3] = { s.zh ? "位运算" : "Bits",
                            s.zh ? "字节流" : "Bytes",
                            s.zh ? "结构体" : "Struct" };
    for (int i = 0; i < 3; ++i) {
        const bool active = static_cast<int>(s.mode) == i;
        drawCenteredText(mem, s.rcTabs[i], wideFromUtf8(tabs[i]), active ? kPanelText : kLabelText);
        if (active) {
            HGDIOBJ old = SelectObject(mem, CreatePen(PS_SOLID, 2, kAccent));
            MoveToEx(mem, s.rcTabs[i].left + 8, kTopH - 2, nullptr);
            LineTo(mem, s.rcTabs[i].right - 8, kTopH - 2);
            DeleteObject(SelectObject(mem, old));
        }
    }
    drawCenteredText(mem, s.rcSet, wideFromUtf8(s.zh ? "设置 ▾" : "Settings ▾"), kLabelText);
    drawCenteredText(mem, s.rcHelp, wideFromUtf8(s.zh ? "帮助 ?" : "Help ?"), kLabelText);

    // ---- expression row ----
    if (bits) {
        SelectObject(mem, exprLabelFont);
        RECT lab { 12, kTopH, 62, kTopH + kExprH };
        drawLeftText(mem, lab, wideFromUtf8(s.zh ? "表达式" : "Expr"), kLabelText);
        RECT border { s.rcExpr.left - 3, s.rcExpr.top - 3, s.rcExpr.right + 3, s.rcExpr.bottom + 3 };
        frameRect(mem, border, kSeparator);
    }

    // ---- result chips ----
    if (bits) {
        const binlab::Repr r = binlab::represent(s.value, s.width, s.isSigned);
        const char* labels[5] = { "HEX ", "OCT ", "BIN ", "DEC ", s.zh ? "全部" : "All" };
        const std::string values[5] = { r.hex, r.oct, r.bin, r.dec, "" };
        for (int i = 0; i < 5; ++i) {
            RECT c = s.rcChip[i];
            HBRUSH chipBg = CreateSolidBrush(kBitOff);
            FillRect(mem, &c, chipBg);
            DeleteObject(chipBg);
            frameRect(mem, c, kSeparator);
            SelectObject(mem, chipLabelFont);
            RECT lr { c.left + 10, c.top, c.right - 10, c.bottom };
            drawLeftText(mem, lr, wideFromUtf8(labels[i]), kLabelText);
            if (!values[i].empty()) {
                SelectObject(mem, chipValueFont);
                const int lwd = textWidth(mem, chipLabelFont, wideFromUtf8(labels[i]));
                RECT vr { c.left + 10 + lwd, c.top, c.right - 10, c.bottom };
                drawLeftText(mem, vr, wideFromUtf8(values[i]), kValueText);
            }
        }
    }

    // ---- width / signedness toolbar ----
    if (bits) {
        HFONT toolFont = uiFont(12, false, s.zh);
        SelectObject(mem, toolFont);
        RECT wl { kGridMargin, s.toolTop, s.rcWidthBtn[0].left, s.toolTop + kToolH };
        drawLeftText(mem, wl, wideFromUtf8(s.zh ? "位宽" : "Width"), kLabelText);
        RECT sgl { s.rcWidthBtn[3].right + 20, s.toolTop, s.rcSignBtn[0].left, s.toolTop + kToolH };
        drawLeftText(mem, sgl, wideFromUtf8(s.zh ? "符号" : "Sign"), kLabelText);
        auto segment = [&](const RECT& r, const std::wstring& text, bool active) {
            HBRUSH fill = CreateSolidBrush(active ? kAccent : kBitOff);
            FillRect(mem, &r, fill);
            DeleteObject(fill);
            frameRect(mem, r, kSeparator);
            drawCenteredText(mem, r, text, active ? kBitOnText : kBitOffText);
        };
        static const int kWidths[4] = { 8, 16, 32, 64 };
        for (int i = 0; i < 4; ++i)
            segment(s.rcWidthBtn[i], std::to_wstring(kWidths[i]), s.width == kWidths[i]);
        segment(s.rcSignBtn[0], wideFromUtf8(s.zh ? "无符号 u" : "unsigned u"), !s.isSigned);
        segment(s.rcSignBtn[1], wideFromUtf8(s.zh ? "有符号 i" : "signed i"), s.isSigned);
        SelectObject(mem, statusFont);
        DeleteObject(toolFont);
    }

    // ---- content area ----
    if (bits) {
        for (int row = 0; row < s.gridRows; ++row) {
            const int cellTop = s.gridTop + row * (kLabelH + kCellH + kRowGap);
            SelectObject(mem, labelFont);
            for (int col = 0; col < 8; ++col) {
                const int bit = bitAt(s.gridRows, 8, row, col);
                if (bit >= s.width)
                    continue;
                RECT cell { cellLeft(col), cellTop, cellLeft(col) + s.cellW - 4, cellTop + kLabelH };
                drawCenteredText(mem, cell, std::to_wstring(bit), kLabelText);
            }
            SelectObject(mem, cellFont);
            for (int col = 0; col < 8; ++col) {
                const int bit = bitAt(s.gridRows, 8, row, col);
                if (bit >= s.width)
                    continue;
                const bool on = getBit(s.value, bit) != 0;
                RECT cell { cellLeft(col), cellTop + kLabelH,
                            cellLeft(col) + s.cellW - 4, cellTop + kLabelH + kCellH };
                HBRUSH fill = CreateSolidBrush(on ? kBitOn : kBitOff);
                FillRect(mem, &cell, fill);
                DeleteObject(fill);
                drawCenteredText(mem, cell, on ? L"1" : L"0", on ? kBitOnText : kBitOffText);
                if (bit == s.focusBit) {
                    HGDIOBJ fp = SelectObject(mem, CreatePen(PS_SOLID, 2, kFocus));
                    RECT border { cell.left + 1, cell.top + 1, cell.right - 1, cell.bottom - 1 };
                    MoveToEx(mem, border.left, border.top, nullptr);
                    LineTo(mem, border.right, border.top);
                    LineTo(mem, border.right, border.bottom);
                    LineTo(mem, border.left, border.bottom);
                    LineTo(mem, border.left, border.top);
                    DeleteObject(SelectObject(mem, fp));
                }
            }
        }
    } else {
        SelectObject(mem, statusFont);
        const std::string line1 = s.mode == EditorSpec::Mode::Bytes
            ? (s.zh ? "字节流交互界面随 v0.3b 提供" : "The interactive byte viewer arrives in v0.3b")
            : (s.zh ? "结构体交互界面随 v0.3c 提供" : "The interactive struct viewer arrives in v0.3c");
        const std::string line2 = s.zh ? "当前可继续使用查询行的 bytes / struct 命令"
                                       : "The bytes / struct query keywords stay available";
        RECT l1 { 0, s.gridTop + 20, w, s.gridTop + 44 };
        RECT l2 { 0, s.gridTop + 48, w, s.gridTop + 72 };
        drawCenteredText(mem, l1, wideFromUtf8(line1), kPanelText);
        drawCenteredText(mem, l2, wideFromUtf8(line2), kLabelText);
    }

    // ---- status bar ----
    RECT statusRect { 0, h - kStatusH, w, h };
    panel = CreateSolidBrush(kPanel);
    FillRect(mem, &statusRect, panel);
    DeleteObject(panel);
    {
        HGDIOBJ old = SelectObject(mem, CreatePen(PS_SOLID, 1, kSeparator));
        MoveToEx(mem, 0, h - kStatusH, nullptr);
        LineTo(mem, w, h - kStatusH);
        DeleteObject(SelectObject(mem, old));
    }
    SelectObject(mem, statusFont);
    std::string left;
    if (bits) {
        left = binlab::typeNameFor(s.width, s.isSigned) + " · " + std::to_string(s.width) + " bit · "
             + (s.isSigned ? (s.zh ? "有符号" : "signed") : (s.zh ? "无符号" : "unsigned"));
    } else if (s.mode == EditorSpec::Mode::Bytes) {
        left = s.zh ? "字节流 · 端序 " : "bytes · endian ";
        left += s.endian == "be" ? "BE" : "LE";
    } else {
        left = std::string("pack=") + (s.pack == 0 ? "natural" : std::to_string(s.pack));
    }
    RECT sl { 12, statusRect.top, w / 2 - 70, h };
    drawLeftText(mem, sl, wideFromUtf8(left), kLabelText);

    std::string mid;
    if (bits) {
        // Live parity over the active width: popcount plus even/odd.
        const std::uint64_t v = s.value & binlab::maskFor(s.width);
        int ones = 0;
        for (int b = 0; b < s.width; ++b)
            ones += getBit(v, b);
        const bool even = ones % 2 == 0;
        mid = s.zh ? std::string("1 的个数 ") + std::to_string(ones) + " · " + (even ? "偶校验位 0" : "偶校验位 1")
                   : std::string("ones ") + std::to_string(ones) + " · " + (even ? "even, parity 0" : "odd, parity 1");
        if (s.focusBit >= 0)
            mid += (s.zh ? " · 位" : " · bit") + std::to_string(s.focusBit) + "="
                 + std::to_string(getBit(s.value, s.focusBit));
    }
    RECT sm { w / 2 - 70, statusRect.top, w - 190, h };
    drawCenteredText(mem, sm, wideFromUtf8(mid), kLabelText);

    // Right slot: a parse error persists (red); a copied message fades (green).
    const bool freshMsg = !s.msg.empty() && GetTickCount() - s.msgTick < kMsgFadeMs;
    RECT sr { w - 190, statusRect.top, w - 12, h };
    if (!s.evalError.empty()) {
        std::wstring err = wideFromUtf8(s.evalError);
        const int maxW = rcWidth(sr) - 6;
        SetTextColor(mem, kErrText);
        if (textWidth(mem, statusFont, err) > maxW) {
            // Trim from the left: the tail carries the message, the code
            // prefix is the least important part.
            while (err.size() > 4 && textWidth(mem, statusFont, L"…" + err) > maxW)
                err.erase(err.begin());
            err.insert(err.begin(), L'…');
        }
        DrawTextW(mem, err.c_str(), -1, &sr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else if (freshMsg) {
        drawCenteredText(mem, sr, wideFromUtf8(s.msg), kOkText);
    }

    // ---- overlays on top of everything ----
    if (s.settingsOpen)
        paintSettings(mem, settingsRect());
    if (s.helpOpen)
        paintHelp(mem, helpRect());

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(tabFont);
    DeleteObject(chipLabelFont);
    DeleteObject(chipValueFont);
    DeleteObject(labelFont);
    DeleteObject(cellFont);
    DeleteObject(statusFont);
    DeleteObject(exprLabelFont);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

void cycleWidth(HWND hwnd)
{
    EditorState& s = state();
    setType(hwnd, s.width == 8 ? 16 : s.width == 16 ? 32 : s.width == 32 ? 64 : 8, s.isSigned);
}

void handleSettingsClick(HWND hwnd, int x, int y)
{
    EditorState& s = state();
    const RECT rc = settingsRect();
    if (!inRect(rc, x, y)) {
        s.settingsOpen = false;
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }
    const std::vector<SettingRow> rows = settingRows();
    int rowY = rc.top + 34;
    for (std::size_t i = 0; i < rows.size(); ++i, rowY += kOverlayRowH) {
        if (y < rowY || y >= rowY + kOverlayRowH)
            continue;
        if (!rows[i].enabled)
            return;
        if (i == 0)
            cycleWidth(hwnd);
        else if (i == 1)
            setType(hwnd, s.width, !s.isSigned);
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == kTimerId) {
            EditorState& s = state();
            if (!s.msg.empty() && GetTickCount() - s.msgTick < kMsgFadeMs + 250)
                InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && reinterpret_cast<HWND>(lp) == state().edit)
            evaluate();
        return 0;
    case WM_LBUTTONDOWN: {
        EditorState& s = state();
        const int x = static_cast<int>(LOWORD(lp));
        const int y = static_cast<int>(HIWORD(lp));
        SetFocus(hwnd); // keyboard returns to the shell unless the click hits a control
        if (s.settingsOpen) {
            handleSettingsClick(hwnd, x, y);
            return 0;
        }
        if (s.helpOpen) {
            s.helpOpen = false;
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        for (int i = 0; i < 3; ++i) {
            if (inRect(s.rcTabs[i], x, y)) {
                s.mode = static_cast<EditorSpec::Mode>(i);
                s.focusBit = -1;
                s.dragging = false;
                relayout(hwnd);
                return 0;
            }
        }
        if (inRect(s.rcSet, x, y)) {
            s.settingsOpen = !s.settingsOpen;
            s.helpOpen = false;
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        if (inRect(s.rcHelp, x, y)) {
            s.helpOpen = !s.helpOpen;
            s.settingsOpen = false;
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        if (s.mode == EditorSpec::Mode::Bits) {
            static const int kWidths[4] = { 8, 16, 32, 64 };
            for (int i = 0; i < 4; ++i) {
                if (inRect(s.rcWidthBtn[i], x, y)) {
                    setType(hwnd, kWidths[i], s.isSigned);
                    return 0;
                }
            }
            for (int i = 0; i < 2; ++i) {
                if (inRect(s.rcSignBtn[i], x, y)) {
                    setType(hwnd, s.width, i == 1);
                    InvalidateRect(hwnd, nullptr, TRUE);
                    return 0;
                }
            }
            for (int i = 0; i < 5; ++i) {
                if (inRect(s.rcChip[i], x, y)) {
                    static const char* const kNames[5] = { "HEX", "OCT", "BIN", "DEC", "ALL" };
                    copyBase(hwnd, i == 4 ? 'A' : "HOBD"[i], kNames[i]);
                    return 0;
                }
            }
            const int bit = hitBit(x, y);
            if (bit >= 0) {
                s.focusBit = bit;
                s.dragPaint = getBit(s.value, bit) ? 0 : 1;
                s.dragging = true;
                setBit(s.value, s.width, bit, s.dragPaint);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        EditorState& s = state();
        if (s.dragging) {
            const int bit = hitBit(static_cast<int>(LOWORD(lp)), static_cast<int>(HIWORD(lp)));
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
            if (s.settingsOpen || s.helpOpen) {
                s.settingsOpen = s.helpOpen = false;
                InvalidateRect(hwnd, nullptr, TRUE);
            } else {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        if (wp == VK_TAB) {
            if (s.edit)
                SetFocus(s.edit);
            return 0;
        }
        if (ctrl) {
            if (wp == 'C' || wp == 'c') { copyBase(hwnd, 'H', "HEX"); return 0; }
            if (wp == 'O' || wp == 'o') { copyBase(hwnd, 'O', "OCT"); return 0; }
            if (wp == 'B' || wp == 'b') { copyBase(hwnd, 'B', "BIN"); return 0; }
            if (wp == 'D' || wp == 'd') { copyBase(hwnd, 'D', "DEC"); return 0; }
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
            f = std::max(0, std::min(f, s.width - 1));
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
        if (state().edit) {
            state().edit = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void threadMain(EditorSpec spec)
{
    EditorState& s = state();
    s.mode = spec.mode;
    s.value = spec.value;
    s.width = spec.width;
    s.isSigned = spec.isSigned;
    s.endian = spec.endian;
    s.pack = spec.pack;
    s.zh = spec.zh;
    s.focusBit = -1;
    s.dragging = false;
    s.settingsOpen = s.helpOpen = false;
    s.evalError.clear();
    s.msg.clear();

    ensureDpiAwareness();

    WNDCLASSW wc {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc); // ERROR_CLASS_ALREADY_EXISTS is fine; the class matches

    // Size the window from measured worst-case text, never guessed.
    {
        HDC dc = GetDC(nullptr);
        computeLayout(dc);
        ReleaseDC(nullptr, dc);
    }
    RECT rc { 0, 0, s.clientW, s.clientH };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    const std::wstring title = spec.zh ? L"二进制工作台" : L"Binary Lab";

    HWND hwnd = CreateWindowExW(0, kClassName, title.c_str(),
                                WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
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

    // Expression input: a real EDIT control so typing, caret and selection all
    // come for free. EN_CHANGE drives live evaluation.
    s.editFont = monoFont(13, false);
    s.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             s.rcExpr.left, s.rcExpr.top, rcWidth(s.rcExpr),
                             s.rcExpr.bottom - s.rcExpr.top,
                             hwnd, reinterpret_cast<HMENU>(1), wc.hInstance, nullptr);
    SendMessageW(s.edit, WM_SETFONT, reinterpret_cast<WPARAM>(s.editFont), TRUE);
    s.origEditProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(s.edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(editProc)));
    if (!spec.expression.empty()) {
        SetWindowTextW(s.edit, wideFromUtf8(spec.expression).c_str()); // fires EN_CHANGE
        evaluate();
    } else {
        std::wstring initial = wideFromUtf8("0x" + std::string((spec.width + 3) / 4, '0'));
        SetWindowTextW(s.edit, initial.c_str());
        evaluate();
    }

    logLayout();
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetTimer(hwnd, kTimerId, 250, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        try {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } catch (const std::exception& e) {
            winshun::debug(std::string("ra9fael.binarylab: editor loop: ") + e.what());
        } catch (...) {
            winshun::debug("ra9fael.binarylab: editor loop: unknown exception");
        }
    }

    if (s.editFont) {
        DeleteObject(s.editFont);
        s.editFont = nullptr;
    }
    s.edit = nullptr;
    s.origEditProc = nullptr;

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
        // The window did not close in time. Detach rather than join: a joinable
        // std::thread left in the static EditorState would call std::terminate()
        // at program exit. The detached thread finishes its own cleanup.
        s.worker.detach();
        winshun::debug("ra9fael.binarylab: editor close timed out; detached");
    }
}

} // namespace binlab
