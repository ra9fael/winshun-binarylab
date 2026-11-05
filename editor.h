// editor.h - the Binary Lab workbench window for the WinShun plugin.
//
// v0.3a: the single-purpose bit editor became a unified workbench shell.
// v0.3a3: five top-bar tabs (bits / bytes / struct | settings / help) and a
// two-row status bar. The shell owns:
//   * top bar      - 5 tabs; the first three are content modes, the last two
//                    are real panels that replace the content area (no more
//                    floating overlays that swallow the first tab click);
//   * input line   - a native EDIT control holding a SpeedCrunch-style
//                    expression re-evaluated on every change through the same
//                    evalBits() the query path uses, so the grid always edits
//                    the expression's result; a type prefix (u8/...) inside
//                    the expression overrides width/signedness;
//   * result row   - HEX/OCT/BIN/DEC chips + copy-all, all mouse-clickable;
//                    the Ctrl+C/B/D/O shortcuts stay;
//   * content area - the interactive bit grid, or a two-column panel;
//   * status bar   - row 1: context + focus/feedback; row 2: four labelled,
//                    clickable parity chips (or a full untruncated error).
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
    enum class Mode { Bits, Bytes, Struct, Settings, Help };
    Mode mode = Mode::Bits;
    std::uint64_t value = 0;       // initial value; the expression overrides it
    int width = 32;                // initial width unless the expression sets one
    bool isSigned = false;         // signedness for the DEC readout
    std::string expression;        // initial expression text (query args)
    // A blank-state entry (`bits ` with no words) intentionally starts with
    // no expression at all, so clicking a bit is not overridden by a stale
    // evaluation. Without this flag an empty expression is seeded with 0x0.
    bool blankExpression = false;
    std::string endian = "le";     // bytes mode (v0.3b)
    int pack = 0;                  // struct mode (v0.3c)
    bool zh = true;                // window language

    // Host settings snapshot shown read-only in the settings tab. The SDK
    // exposes settingsChanged() but no setter, so the window cannot write
    // these back; they are a snapshot taken when the window opens.
    int hostWidth = 32;
    std::string hostEndian = "le";
    int hostPack = 0;
    std::string hostBitsEnter = "copy";
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
    int hostWidth = 32;
    std::string hostEndian = "le";
    int hostPack = 0;
    std::string hostBitsEnter = "copy";
    int focusBit = -1;
    bool dragging = false;
    int dragPaint = 0;

    HWND edit = nullptr;
    WNDPROC origEditProc = nullptr;
    HFONT editFont = nullptr;
    std::string evalError;

    std::string msg;
    DWORD msgTick = 0;

    // Cached geometry, recomputed by computeLayout().
    int clientW = 0;
    int clientH = 0;
    RECT rcTabs[5] = {};
    int tabDividerX = 0;       // separator between content modes and tool tabs
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
    RECT rcParity[4] = {};     // status row 2: labelled parity chips
    int hoverParity = -1;      // chip under the cursor, for the definition tip
    RECT rcSetWidth[4] = {};   // settings tab: 8 | 16 | 32 | 64
    RECT rcSetSign[2] = {};    // settings tab: u | i
    int panelTop = 0;          // content area for the settings/help tabs
    int panelBottom = 0;
    int panelCol2 = 0;         // left edge of the right column
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
constexpr int kStatusH = 46;       // two rows: context | parity / messages
constexpr int kStatusRowH = 23;
constexpr int kPanelContentH = 200; // settings/help two-column content area
constexpr int kPanelRowH = 26;
constexpr int kMinClientH = kTopH + kPanelContentH + kStatusH; // 280

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

// A highlighted (or idle) segmented button, shared by the width toolbar and
// the settings tab so both always render the same way.
void drawSegment(HDC dc, const RECT& r, const std::wstring& text, bool active)
{
    HBRUSH fill = CreateSolidBrush(active ? kAccent : kBitOff);
    FillRect(dc, &r, fill);
    DeleteObject(fill);
    frameRect(dc, r, kSeparator);
    drawCenteredText(dc, r, text, active ? kBitOnText : kBitOffText);
}

const char* tabLabel(int i, bool zh)
{
    static const char* const kZh[5] = { "位运算", "字节流", "结构体", "设置", "帮助" };
    static const char* const kEn[5] = { "Bits", "Bytes", "Struct", "Settings", "Help" };
    return zh ? kZh[i] : kEn[i];
}

// One sentence per chip, shown on hover and in the help tab. Keep the two
// concepts apart: parity (odd/even of the count) and the bit to append.
std::string parityDefinition(int i, bool zh)
{
    static const char* const kZh[4] = {
        "置 1 位数：当前位宽内值为 1 的位数",
        "奇偶性：上面这个数是奇数还是偶数",
        "偶校验位：补上它，让 1 的总数为偶",
        "奇校验位：补上它，让 1 的总数为奇",
    };
    static const char* const kEn[4] = {
        "ones: count of 1 bits in the active width",
        "parity: whether that count is odd or even",
        "even-bit: append it to make the 1 count even",
        "odd-bit: append it to make the 1 count odd",
    };
    return zh ? kZh[i] : kEn[i];
}

int getBit(std::uint64_t value, int bit)
{
    return static_cast<int>((value >> bit) & 1u);
}

int popcount(std::uint64_t value, int width)
{
    int ones = 0;
    for (int b = 0; b < width; ++b)
        ones += getBit(value, b);
    return ones;
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
    for (int i = 0; i < 5; ++i) {
        if (i == 3) {
            s.tabDividerX = x + 2;
            x += 14; // visual gap between content modes and tool panels
        }
        const int tw = textWidth(dc, tabFont, wideFromUtf8(tabLabel(i, s.zh)));
        s.rcTabs[i] = { x, 0, x + tw + 2 * tabPad, kTopH };
        x += tw + 2 * tabPad + 2;
    }
    DeleteObject(tabFont);

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

    // Five tabs fit both languages in the narrowest window (measured:
    // 298 px zh / 287 px en at 480 px client width), so the right end of the
    // top bar is deliberately left empty.
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
    if (bits) {
        s.clientH = s.gridTop + s.gridRows * (kLabelH + kCellH + kRowGap) + 8 + kStatusH;
        s.panelTop = s.panelBottom = 0;
        s.panelCol2 = 0;
        for (RECT& r : s.rcSetWidth)
            r = {};
        for (RECT& r : s.rcSetSign)
            r = {};
    } else {
        // All non-bits tabs share one fixed content height, so switching
        // between them never produces a small resize jitter.
        s.clientH = kTopH + kPanelContentH + kStatusH;
        s.panelTop = kTopH + 8;
        s.panelBottom = kTopH + kPanelContentH;
        s.panelCol2 = s.clientW / 2 + 6;
        const int rowTop = s.panelTop + 34;
        const int bx = 16 + 62;
        for (int i = 0; i < 4; ++i)
            s.rcSetWidth[i] = { bx + i * 36, rowTop + 2,
                                bx + i * 36 + 34, rowTop + kPanelRowH - 2 };
        for (int i = 0; i < 2; ++i)
            s.rcSetSign[i] = { bx + i * 48, rowTop + kPanelRowH + 2,
                               bx + i * 48 + 44, rowTop + 2 * kPanelRowH - 2 };
    }
    s.clientH = std::max(s.clientH, kMinClientH);
    s.rcExpr = { 64, kTopH + (kExprH - 24) / 2, s.clientW - 12, kTopH + (kExprH + 24) / 2 };

    // Status row 2: four equal chips spanning the full client width.
    const int pcGap = 6;
    const int pcW = (s.clientW - 24 - 3 * pcGap) / 4;
    const int pcTop = s.clientH - kStatusRowH + 2;
    for (int i = 0; i < 4; ++i) {
        const int px = 12 + i * (pcW + pcGap);
        s.rcParity[i] = { px, pcTop, px + pcW, s.clientH - 2 };
    }
}

// Emit the geometry for the drive scripts (they parse stderr rather than
// duplicating the metrics above).
std::string rectStr(const RECT& r)
{
    char b[80];
    std::snprintf(b, sizeof(b), "%d,%d,%d,%d",
                  static_cast<int>(r.left), static_cast<int>(r.top),
                  static_cast<int>(rcWidth(r)), static_cast<int>(r.bottom - r.top));
    return b;
}

void logLayout()
{
    EditorState& s = state();
    const bool bits = s.mode == EditorSpec::Mode::Bits;
    std::string line = "ra9fael.binarylab: layout: clientW=" + std::to_string(s.clientW)
        + " clientH=" + std::to_string(s.clientH)
        + " gridLeft=" + std::to_string(s.gridLeft)
        + " gridTop=" + std::to_string(s.gridTop)
        + " cellW=" + std::to_string(s.cellW)
        + " mode=" + std::to_string(static_cast<int>(s.mode))
        + " statusTop=" + std::to_string(s.clientH - kStatusH)
        + " tab0=" + rectStr(s.rcTabs[0])
        + " tab1=" + rectStr(s.rcTabs[1])
        + " tab3=" + rectStr(s.rcTabs[3])
        + " tab4=" + rectStr(s.rcTabs[4])
        + " chip0=" + (bits ? rectStr(s.rcChip[0]) : std::string("none"));
    if (bits) {
        for (int i = 1; i < 5; ++i)
            line += " chip" + std::to_string(i) + "=" + rectStr(s.rcChip[i]);
        line += " w8=" + rectStr(s.rcWidthBtn[0])
              + " w64=" + rectStr(s.rcWidthBtn[3])
              + " sgn1=" + rectStr(s.rcSignBtn[1]);
        for (int i = 0; i < 4; ++i)
            line += " parity" + std::to_string(i) + "=" + rectStr(s.rcParity[i]);
    }
    line += " setw0=" + rectStr(s.rcSetWidth[0])
          + " setsgn1=" + rectStr(s.rcSetSign[1]);
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

// Status-bar parity chips copy just their value (5 / odd / 1 / 0), not a
// full sentence, so the result can be pasted straight into a register field.
void copyParity(HWND hwnd, int index, const std::string& value)
{
    EditorState& s = state();
    const bool ok = copyToClipboard(hwnd, value);
    static const char* const kNames[4] = { "ONES", "PARITY", "EVEN-BIT", "ODD-BIT" };
    winshun::debug(std::string("ra9fael.binarylab: copyParity ") + kNames[index]
                   + " value=" + value + " ok=" + (ok ? "1" : "0"));
    s.msg = ok ? (s.zh ? std::string("已复制 ") + kNames[index] : std::string("Copied ") + kNames[index])
               : (s.zh ? "复制失败" : "copy failed");
    s.msgTick = GetTickCount();
    InvalidateRect(hwnd, nullptr, TRUE);
}

void parityValues(std::string out[4])
{
    EditorState& s = state();
    const int ones = popcount(s.value & binlab::maskFor(s.width), s.width);
    const bool even = ones % 2 == 0;
    out[0] = std::to_string(ones);
    out[1] = s.zh ? (even ? "偶" : "奇") : (even ? "even" : "odd");
    out[2] = even ? "0" : "1"; // bit to append for an even number of ones
    out[3] = even ? "1" : "0"; // bit to append for an odd number of ones
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
        // The status bar shows this string verbatim; log it so the drive
        // script can assert that no character is trimmed away.
        winshun::debug("ra9fael.binarylab: evalError " + s.evalError);
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
            if (s.hwnd)
                DestroyWindow(s.hwnd);
            return 0;
        }
        if (wp == VK_TAB) {
            SetFocus(s.hwnd); // hand the keyboard back to the grid
            return 0;
        }
    }
    if (msg == WM_PAINT) {
        const LRESULT r = CallWindowProcW(s.origEditProc, h, msg, wp, lp);
        // EM_SETCUEBANNER needs visual styles this process does not enable,
        // so draw the empty-expression hint ourselves. The native EDIT keeps
        // its own white background; the hint uses a dark grey on that surface.
        if (GetWindowTextLengthW(h) == 0) {
            RECT rc {};
            GetClientRect(h, &rc);
            HDC dc = GetDC(h);
            HFONT font = uiFont(11, false, s.zh);
            HGDIOBJ old = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(128, 128, 128));
            RECT t { rc.left + 6, rc.top, rc.right - 6, rc.bottom };
            const std::wstring cue = s.zh ? L"输入表达式，如 u8 0xA5 & 0x0F"
                                          : L"Type an expression, e.g. u8 0xA5 & 0x0F";
            DrawTextW(dc, cue.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
            DeleteObject(font);
            ReleaseDC(h, dc);
        }
        return r;
    }
    return CallWindowProc(s.origEditProc, h, msg, wp, lp);
}

// ---- content-area panels (settings / help tabs) ---------------------------
// These replace the old floating overlays: they occupy the whole content area
// and never swallow a click meant for a tab.

void drawOverlayPanel(HDC dc, const RECT& rc)
{
    HBRUSH fill = CreateSolidBrush(kOverlay);
    FillRect(dc, &rc, fill);
    DeleteObject(fill);
    frameRect(dc, rc, kSeparator);
}

void drawRowStrip(HDC dc, const RECT& r)
{
    drawOverlayPanel(dc, r);
}

// A status-row chip: label on the left, value on the right, both in one flat
// surface. Text is measured by the font already selected into dc.
void drawParityChip(HDC dc, const RECT& r, const std::wstring& label, const std::wstring& value)
{
    HBRUSH fill = CreateSolidBrush(kBitOff);
    FillRect(dc, &r, fill);
    DeleteObject(fill);
    frameRect(dc, r, kSeparator);
    RECT t { r.left + 8, r.top, r.right - 8, r.bottom };
    drawLeftText(dc, t, label, kLabelText);
    SetTextColor(dc, kValueText);
    DrawTextW(dc, value.c_str(), -1, &t, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void paintSettingsContent(HDC dc)
{
    EditorState& s = state();
    HFONT font = uiFont(12, false, s.zh);
    HGDIOBJ old = SelectObject(dc, font);
    const int left = 16;
    const int right = s.clientW - 16;
    const int mid = s.panelCol2;
    const int colW = mid - left - 12;

    RECT t1 { left, s.panelTop, left + colW, s.panelTop + 24 };
    RECT t2 { mid, s.panelTop, right, s.panelTop + 24 };
    drawLeftText(dc, t1, wideFromUtf8(s.zh ? "本次会话（点击即时生效）" : "This session (click to apply)"), kPanelText);
    drawLeftText(dc, t2, wideFromUtf8(s.zh ? "插件设置（只读）" : "Plugin settings (read-only)"), kPanelText);

    const int rowTop = s.panelTop + 34;
    const char* labels[5] = {
        s.zh ? "位宽" : "Width", s.zh ? "符号" : "Sign", s.zh ? "^ 语义" : "^ semantics",
        s.zh ? "端序" : "Endian", s.zh ? "语言" : "Language",
    };
    const std::string values[5] = {
        "", "", "XOR",
        std::string(s.endian == "be" ? "BE" : "LE") + " · pack "
            + (s.pack == 0 ? "natural" : std::to_string(s.pack)),
        s.zh ? "跟随宿主" : "follows host",
    };
    for (int i = 0; i < 5; ++i) {
        const int y = rowTop + i * kPanelRowH;
        RECT r { left, y, left + colW, y + kPanelRowH - 2 };
        drawRowStrip(dc, r);
        RECT rl { r.left + 8, r.top, r.left + 62, r.bottom };
        drawLeftText(dc, rl, wideFromUtf8(labels[i]), kLabelText);
        if (i >= 2) {
            RECT rv { r.left + 62, r.top, r.right - 8, r.bottom };
            SetTextColor(dc, kPanelText);
            DrawTextW(dc, wideFromUtf8(values[i]).c_str(), -1, &rv,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }
    static const int kWidths[4] = { 8, 16, 32, 64 };
    for (int i = 0; i < 4; ++i)
        drawSegment(dc, s.rcSetWidth[i], std::to_wstring(kWidths[i]), s.width == kWidths[i]);
    drawSegment(dc, s.rcSetSign[0], L"u", !s.isSigned);
    drawSegment(dc, s.rcSetSign[1], L"i", s.isSigned);

    const char* hostLabels[4] = { "defaultWidth", "defaultEndian", "defaultPack", "bitsEnter" };
    const std::string hostValues[4] = {
        std::to_string(s.hostWidth), s.hostEndian,
        s.hostPack == 0 ? "0 (natural)" : std::to_string(s.hostPack), s.hostBitsEnter,
    };
    for (int i = 0; i < 4; ++i) {
        const int y = rowTop + i * kPanelRowH;
        RECT r { mid, y, right, y + kPanelRowH - 2 };
        drawRowStrip(dc, r);
        RECT rl { r.left + 8, r.top, r.right - 80, r.bottom };
        RECT rv { r.right - 80, r.top, r.right - 8, r.bottom };
        drawLeftText(dc, rl, wideFromUtf8(hostLabels[i]), kLabelText);
        SetTextColor(dc, kPanelText);
        DrawTextW(dc, wideFromUtf8(hostValues[i]).c_str(), -1, &rv,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    RECT hint { mid, rowTop + 4 * kPanelRowH, right, rowTop + 5 * kPanelRowH };
    drawLeftText(dc, hint, wideFromUtf8(s.zh ? "在 WinShun 插件设置中修改" : "Change in WinShun plugin settings"), kLabelText);
    SelectObject(dc, old);
    DeleteObject(font);
}

void paintHelpContent(HDC dc)
{
    EditorState& s = state();
    HFONT font = uiFont(11, false, s.zh);
    HFONT titleFont = uiFont(12, true, s.zh);
    HGDIOBJ old = SelectObject(dc, titleFont);
    const int left = 16;
    const int right = s.clientW - 16;
    const int mid = s.panelCol2;

    RECT t1 { left, s.panelTop, mid - 12, s.panelTop + 20 };
    RECT t2 { mid, s.panelTop, right, s.panelTop + 20 };
    drawLeftText(dc, t1, wideFromUtf8(s.zh ? "语法" : "Syntax"), kPanelText);
    drawLeftText(dc, t2, wideFromUtf8(s.zh ? "鼠标与键盘" : "Mouse and keys"), kPanelText);

    const char* l[4] = {
        s.zh ? "0x / 0b / 0o  进制前缀" : "0x / 0b / 0o radix prefixes",
        s.zh ? "& | ^ ~ << >>   (^ = XOR)" : "& | ^ ~ << >>    (^ = XOR)",
        s.zh ? "rol / ror  @  7:4  get/set" : "rol / ror  @  7:4  get/set",
        s.zh ? "u8/u16/u32/u64/i8... 前缀" : "u8/u16/u32/u64/i8... type prefix",
    };
    const char* r[4] = {
        s.zh ? "点击位格翻转 · 拖动涂抹" : "Click toggles · drag paints",
        s.zh ? "点击读数 / chip 复制" : "Click chips / parity to copy",
        s.zh ? "方向键移动 · 空格翻转" : "Arrows move · Space toggles",
        s.zh ? "Ctrl+C/B/D/O · Tab · Esc" : "Ctrl+C/B/D/O · Tab · Esc",
    };
    SelectObject(dc, font);
    for (int i = 0; i < 4; ++i) {
        const int y = s.panelTop + 22 + i * 22;
        RECT lr { left, y, mid - 12, y + 20 };
        RECT rr { mid, y, right, y + 20 };
        drawLeftText(dc, lr, wideFromUtf8(l[i]), kLabelText);
        drawLeftText(dc, rr, wideFromUtf8(r[i]), kLabelText);
    }

    const int sepY = s.panelTop + 112;
    HGDIOBJ pen = SelectObject(dc, CreatePen(PS_SOLID, 1, kSeparator));
    MoveToEx(dc, left, sepY, nullptr);
    LineTo(dc, right, sepY);
    DeleteObject(SelectObject(dc, pen));

    SelectObject(dc, titleFont);
    RECT t3 { left, s.panelTop + 116, right, s.panelTop + 136 };
    drawLeftText(dc, t3, wideFromUtf8(s.zh ? "校验术语" : "Parity terms"), kPanelText);
    const char* b[2] = {
        s.zh ? "置 1 位数 = 当前位宽内 1 的个数     奇偶性 = 该数是奇还是偶"
             : "ones = count of 1 bits in the width     parity = odd/even of that count",
        s.zh ? "偶校验位 = 补它使 1 的总数为偶      奇校验位 = 补它使 1 的总数为奇"
             : "even-bit = append for an even 1 count   odd-bit = append for an odd 1 count",
    };
    SelectObject(dc, font);
    for (int i = 0; i < 2; ++i) {
        RECT br { left, s.panelTop + 138 + i * 20, right, s.panelTop + 158 + i * 20 };
        drawLeftText(dc, br, wideFromUtf8(b[i]), kLabelText);
    }
    SelectObject(dc, old);
    DeleteObject(titleFont);
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
    for (int i = 0; i < 5; ++i) {
        const bool active = static_cast<int>(s.mode) == i;
        drawCenteredText(mem, s.rcTabs[i], wideFromUtf8(tabLabel(i, s.zh)), active ? kPanelText : kLabelText);
        if (active) {
            HGDIOBJ old = SelectObject(mem, CreatePen(PS_SOLID, 2, kAccent));
            MoveToEx(mem, s.rcTabs[i].left + 8, kTopH - 2, nullptr);
            LineTo(mem, s.rcTabs[i].right - 8, kTopH - 2);
            DeleteObject(SelectObject(mem, old));
        }
    }
    {
        HGDIOBJ old = SelectObject(mem, CreatePen(PS_SOLID, 1, kSeparator));
        MoveToEx(mem, s.tabDividerX, 8, nullptr);
        LineTo(mem, s.tabDividerX, kTopH - 8);
        DeleteObject(SelectObject(mem, old));
    }

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
        static const int kWidths[4] = { 8, 16, 32, 64 };
        for (int i = 0; i < 4; ++i)
            drawSegment(mem, s.rcWidthBtn[i], std::to_wstring(kWidths[i]), s.width == kWidths[i]);
        drawSegment(mem, s.rcSignBtn[0], wideFromUtf8(s.zh ? "无符号 u" : "unsigned u"), !s.isSigned);
        drawSegment(mem, s.rcSignBtn[1], wideFromUtf8(s.zh ? "有符号 i" : "signed i"), s.isSigned);
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
    } else if (s.mode == EditorSpec::Mode::Settings) {
        paintSettingsContent(mem);
    } else if (s.mode == EditorSpec::Mode::Help) {
        paintHelpContent(mem);
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

    // ---- status bar: row 1 = context + focus/feedback, row 2 = parity ----
    const int statusTop = h - kStatusH;
    RECT statusRect { 0, statusTop, w, h };
    panel = CreateSolidBrush(kPanel);
    FillRect(mem, &statusRect, panel);
    DeleteObject(panel);
    {
        HGDIOBJ old = SelectObject(mem, CreatePen(PS_SOLID, 1, kSeparator));
        MoveToEx(mem, 0, statusTop, nullptr);
        LineTo(mem, w, statusTop);
        MoveToEx(mem, 0, statusTop + kStatusRowH, nullptr);
        LineTo(mem, w, statusTop + kStatusRowH);
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
    } else if (s.mode == EditorSpec::Mode::Struct) {
        left = std::string("pack=") + (s.pack == 0 ? "natural" : std::to_string(s.pack));
    } else if (s.mode == EditorSpec::Mode::Settings) {
        left = s.zh ? "设置 · 本次会话生效" : "Settings · this session only";
    } else {
        left = s.zh ? "帮助 · 语法与快捷键" : "Help · syntax and keys";
    }
    // Hovering a parity chip swaps the context text for its definition. This
    // keeps the explanation inside the status bar instead of floating a box
    // over the bottom row of bit cells.
    if (bits && s.hoverParity >= 0 && s.evalError.empty())
        left = parityDefinition(s.hoverParity, s.zh);
    RECT sl { 12, statusTop, w - 200, statusTop + kStatusRowH };
    drawLeftText(mem, sl, wideFromUtf8(left), kLabelText);

    const bool freshMsg = !s.msg.empty() && GetTickCount() - s.msgTick < kMsgFadeMs;
    std::string right1;
    if (freshMsg)
        right1 = s.msg;
    else if (bits && s.focusBit >= 0)
        right1 = (s.zh ? "焦点 位 " : "focus bit ") + std::to_string(s.focusBit)
               + " = " + std::to_string(getBit(s.value, s.focusBit));
    else
        right1 = "v0.3.0";
    RECT sr1 { w - 200, statusTop, w - 12, statusTop + kStatusRowH };
    drawCenteredText(mem, sr1, wideFromUtf8(right1), freshMsg ? kOkText : kLabelText);

    const int row2 = statusTop + kStatusRowH;
    if (!s.evalError.empty()) {
        // Full text, no trimming: a typical error measures 229 px and the
        // narrowest window still offers 456 px on this row.
        RECT er { 12, row2, w - 12, h };
        drawLeftText(mem, er, wideFromUtf8(s.evalError), kErrText);
    } else if (bits) {
        std::string pv[4];
        parityValues(pv);
        const char* labels[4] = {
            s.zh ? "置 1 位数" : "ones", s.zh ? "奇偶性" : "parity",
            s.zh ? "偶校验位" : "even-bit", s.zh ? "奇校验位" : "odd-bit",
        };
        for (int i = 0; i < 4; ++i)
            drawParityChip(mem, s.rcParity[i], wideFromUtf8(labels[i]), wideFromUtf8(pv[i]));
    } else {
        const std::string hint = s.mode == EditorSpec::Mode::Bytes
            ? (s.zh ? "校验面板随 v0.3b 提供" : "Checksum panel arrives in v0.3b")
            : s.mode == EditorSpec::Mode::Struct
                ? (s.zh ? "结构体交互随 v0.3c 提供" : "Interactive struct viewer arrives in v0.3c")
                : s.mode == EditorSpec::Mode::Settings
                    ? (s.zh ? "左列改本次会话，右列为宿主设置快照（只读）" : "Left: this session. Right: read-only host settings")
                    : (s.zh ? "校验术语见上方，状态栏 chip 可点击复制" : "See parity terms above; status chips are clickable");
        RECT hr { 12, row2, w - 12, h };
        drawLeftText(mem, hr, wideFromUtf8(hint), kLabelText);
    }

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

// The settings tab reuses setType(), so a click there and a click on the
// bit-grid toolbar always produce exactly the same state (including the
// expression's type prefix rewrite).
void handleSettingsContentClick(HWND hwnd, int x, int y)
{
    EditorState& s = state();
    static const int kWidths[4] = { 8, 16, 32, 64 };
    for (int i = 0; i < 4; ++i) {
        if (inRect(s.rcSetWidth[i], x, y)) {
            setType(hwnd, kWidths[i], s.isSigned);
            return;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (inRect(s.rcSetSign[i], x, y)) {
            setType(hwnd, s.width, i == 1);
            InvalidateRect(hwnd, nullptr, TRUE);
            return;
        }
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
        for (int i = 0; i < 5; ++i) {
            if (inRect(s.rcTabs[i], x, y)) {
                s.mode = static_cast<EditorSpec::Mode>(i);
                s.focusBit = -1;
                s.dragging = false;
                s.hoverParity = -1;
                relayout(hwnd);
                return 0;
            }
        }
        if (s.mode == EditorSpec::Mode::Settings) {
            handleSettingsContentClick(hwnd, x, y);
            return 0;
        }
        if (s.mode == EditorSpec::Mode::Help)
            return 0;
        if (s.mode == EditorSpec::Mode::Bits) {
            std::string pv[4];
            parityValues(pv);
            for (int i = 0; i < 4; ++i) {
                if (inRect(s.rcParity[i], x, y)) {
                    copyParity(hwnd, i, pv[i]);
                    return 0;
                }
            }
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
        const int x = static_cast<int>(LOWORD(lp));
        const int y = static_cast<int>(HIWORD(lp));
        if (s.dragging) {
            const int bit = hitBit(x, y);
            if (bit >= 0)
                setBit(s.value, s.width, bit, s.dragPaint);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        int hover = -1;
        if (s.mode == EditorSpec::Mode::Bits && s.evalError.empty()) {
            for (int i = 0; i < 4; ++i)
                if (inRect(s.rcParity[i], x, y))
                    hover = i;
        }
        if (hover != s.hoverParity) {
            s.hoverParity = hover;
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
    s.hostWidth = spec.hostWidth;
    s.hostEndian = spec.hostEndian;
    s.hostPack = spec.hostPack;
    s.hostBitsEnter = spec.hostBitsEnter;
    s.focusBit = -1;
    s.dragging = false;
    s.hoverParity = -1;
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
    } else if (!spec.blankExpression) {
        std::wstring initial = wideFromUtf8("0x" + std::string((spec.width + 3) / 4, '0'));
        SetWindowTextW(s.edit, initial.c_str());
    }
    evaluate();

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
