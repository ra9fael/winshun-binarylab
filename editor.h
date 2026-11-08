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

#include "formats.h"
#include "logic.h"
#include "winshun.h"

#include <windows.h>
#include "window_icons.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <memory>
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
    bool cMode = false;            // evalMode=c: C integer promotions in the expression line
    bool zh = true;                // window language

    // Host settings snapshot shown read-only in the settings tab. The SDK
    // exposes settingsChanged() but no setter, so the window cannot write
    // these back; they are a snapshot taken when the window opens.
    int hostWidth = 32;
    std::string hostEndian = "le";
    int hostPack = 0;
    std::string hostBitsEnter = "copy";
    std::string hostEvalMode = "fixed";
    std::string hostFloatBar = "off"; // floatBar setting: seeds the in-window bar
    std::string dataDir;           // host dataDir; history.jsonl lives here
    // Record formats for the bytes page (fmt ...): a snapshot taken when the
    // window opens. A later settingsChanged reaches the next window.
    std::shared_ptr<const FormatSet> formats;
};

bool openEditorWindow(const EditorSpec& spec, std::string& error);
bool editorVisible();
void closeEditorAndWait(unsigned timeoutMs);

namespace detail {

constexpr int kTimerId = 1;
constexpr DWORD kMsgFadeMs = 2000;
constexpr int kHistBtnW = 34;       // expression history dropdown button
constexpr int kHistRows = 12;       // visible entries in the dropdown
constexpr int kHistRowH = 24;
constexpr int kMaxHistory = 200;    // capped, adjacent duplicates removed

struct HistoryEntry {
    std::string expr;
    std::string result;
    std::string type;
};

struct EditorState {
    WindowIcons windowIcons;
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
    std::string hostEvalMode = "fixed";
    std::string hostFloatBar = "off";
    bool cMode = false;
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
    int editH = 20;            // measured mono line height: no slack for the caret
    std::wstring killBuf;      // bash-style kill buffer (Ctrl+U/K/W/Y)
    RECT rcChip[5] = {};
    int resultRows = 1;
    int gridTop = 0;
    int gridLeft = 0;
    int gridRows = 1;
    int cellW = 40;            // adaptive: the 8 columns fill the client width
    int toolTop = 0;           // width / signedness toolbar row (bits mode)
    RECT rcWidthBtn[4] = {};   // 8 | 16 | 32 | 64
    RECT rcKindBtn[3] = {};    // unsigned u | signed i | float f
    RECT rcParity[4] = {};     // status row 2: labelled parity chips
    int hoverParity = -1;      // chip under the cursor, for the definition tip
    RECT rcBarToggle = {};     // top bar right end: show / hide the float bar
    RECT rcBarSeg[3] = {};     // composition bar, display order S | E | M
    int barTop = 0;            // top of the bar row (bits mode, 0 when hidden)
    int hoverSeg = -1;         // segment under the cursor, in bar *or* grid
    RECT rcSetWidth[4] = {};   // settings tab: 8 | 16 | 32 | 64
    RECT rcSetSign[2] = {};    // settings tab: u | i
    RECT rcSetMode[2] = {};    // settings tab: fixed | C
    int panelTop = 0;          // content area for the settings/help tabs
    int panelBottom = 0;
    int panelCol2 = 0;         // left edge of the right column
    bool suppressEval = false; // set while the toolbar rewrites the type prefix

    // Float result: `value` holds the IEEE-754 bit pattern and the DEC chip
    // shows floatText instead of the pattern's integer value.
    bool isFloat = false;
    std::string floatText;
    std::string unitNote;
    // The composition bar is a view, not a result property: its row height
    // follows this flag alone, never the type of what is on display.
    bool floatBar = false;

    // Expression history. A dropdown, not a sixth tab: history is an input
    // assistant and must stay usable while the bit grid is visible.
    std::string dataDir;
    std::vector<HistoryEntry> history;
    bool historyPersistent = true;
    int historyPos = -1;       // -1 = not browsing
    bool historyOpen = false;
    std::string histDraft;     // text typed before browsing back in time
    RECT rcHistoryBtn = {};
    RECT rcHistoryItem[kHistRows] = {};
    RECT rcClearHistory = {};

    // ---- bytes page (v0.3b) ----
    // Bits and bytes share the one EDIT control; each keeps its own text, saved
    // and restored on a tab switch.
    std::string bitsExpr;
    std::string bytesExpr;
    EditorSpec::Mode editOwner = EditorSpec::Mode::Bits; // whose text the EDIT holds
    std::shared_ptr<const FormatSet> formats;
    bool scalarHas = false;       // a good result of each kind, kept across errors
    bool recordsHas = false;
    bool bytesIsRecords = false;  // fmt ... (records) vs hex/bin/oct (scalar)
    BytesEval bytesEval;
    TypeSpec bytesType;
    RecordsEval recordsEval;
    const RecordFormat* recFmt = nullptr; // points into *formats
    std::vector<int> colChars;    // widest text per record column, in chars
    int bytesLaidOut = -1;        // layout key computeLayout() last used
    std::size_t selAnchor = 0;    // scalar selection: anchor and active byte
    std::size_t selActive = 0;
    bool byteDrag = false;
    std::size_t scrollRow = 0;    // first visible 16-byte line / record
    std::size_t selRecord = 0;
    std::size_t selField = 0;     // column index
    std::size_t firstCol = 0;     // first visible scrolling column
    bool formatOpen = false;
    int monoW = 7;                // Consolas 13 advance
    int byteTop = 0;              // header row of the grid / table
    int offW = 40;
    int byteW = 24;
    int asciiLeft = 0;
    RECT rcToolLabel[5] = {};     // toolbar captions: width, type, endian | format, order
    int formatScroll = 0;         // first dropdown entry shown
    RECT rcEndian[2] = {};        // LE | BE
    RECT rcFormatBtn = {};
    RECT rcOrder[2] = {};         // LSB | MSB
    RECT rcFormatItem[10] = {};
    RECT rcTable = {};
    RECT rcByteChip[3] = {};      // HEX | DEC/FLOAT | BIN of the active word
    RECT rcCellChip = {};         // records: the selected field
    RECT rcToBits = {};
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

// ---- expression history persistence ----------------------------------------
// JSONL, one append per submitted expression. A missing file is normal on the
// first run; an unwritable dataDir (strict sandbox) degrades to session-only.

std::string jsonEscape(const std::string& s)
{
    std::string out;
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) continue; // control characters cannot come from a single-line EDIT
        else out += static_cast<char>(c);
    }
    return out;
}

std::string jsonField(const std::string& line, const std::string& key)
{
    const std::string pat = "\"" + key + "\":\"";
    const std::size_t a = line.find(pat);
    if (a == std::string::npos)
        return {};
    std::size_t i = a + pat.size();
    std::string out;
    while (i < line.size() && line[i] != '"') {
        if (line[i] == '\\' && i + 1 < line.size()) {
            ++i;
            switch (line[i]) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default: out += line[i]; break;
            }
        } else {
            out += line[i];
        }
        ++i;
    }
    return out;
}

std::string historyPath()
{
    EditorState& s = state();
    if (s.dataDir.empty())
        return {};
    std::string p = s.dataDir;
    if (p.back() != '/' && p.back() != '\\')
        p += '\\';
    return p + "history.jsonl";
}

bool readHistoryFile(std::string& out)
{
    const std::string path = historyPath();
    if (path.empty())
        return false;
    HANDLE h = CreateFileW(wideFromUtf8(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    out.clear();
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0)
        out.append(buf, static_cast<std::size_t>(got));
    CloseHandle(h);
    return true;
}

bool writeHistoryLine(const std::string& line, bool truncate)
{
    const std::string path = historyPath();
    if (path.empty())
        return false;
    HANDLE h = CreateFileW(wideFromUtf8(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, truncate ? CREATE_ALWAYS : OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    if (!truncate)
        SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD written = 0;
    const BOOL ok = WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == line.size();
}

void loadHistory()
{
    EditorState& s = state();
    s.history.clear();
    s.historyPersistent = !s.dataDir.empty();
    std::string text;
    if (!readHistoryFile(text))
        return; // no file yet: the first successful submit creates it
    std::size_t a = 0;
    while (a < text.size()) {
        const std::size_t b = text.find('\n', a);
        const std::string line = text.substr(a, (b == std::string::npos ? text.size() : b) - a);
        a = b == std::string::npos ? text.size() : b + 1;
        if (line.empty() || line.size() > 2048)
            continue;
        HistoryEntry e { jsonField(line, "expr"), jsonField(line, "result"), jsonField(line, "type") };
        if (!e.expr.empty())
            s.history.push_back(std::move(e));
        if (s.history.size() >= kMaxHistory)
            break;
    }
}

void appendHistoryFile(const HistoryEntry& e)
{
    EditorState& s = state();
    if (!s.historyPersistent)
        return;
    const std::string line = "{\"expr\":\"" + jsonEscape(e.expr) + "\",\"result\":\""
                           + jsonEscape(e.result) + "\",\"type\":\"" + jsonEscape(e.type) + "\"}\n";
    if (writeHistoryLine(line, false)) {
        winshun::debug("ra9fael.binarylab: history append expr=" + e.expr);
    } else {
        s.historyPersistent = false;
        winshun::debug("ra9fael.binarylab: history is session-only (dataDir is not writable)");
    }
}

void clearHistory()
{
    EditorState& s = state();
    s.history.clear();
    s.historyPos = -1;
    s.historyOpen = false;
    if (s.historyPersistent && !writeHistoryLine(std::string(), true))
        s.historyPersistent = false;
    winshun::debug("ra9fael.binarylab: history cleared");
}

// "4Mi = 4 x 2^20 = 4194304" -> "4Mi -> 4194304" for the narrow status slot.
std::string compactNote(const std::string& note)
{
    const std::size_t first = note.find(" = ");
    const std::size_t last = note.rfind(" = ");
    if (first == std::string::npos || last == first)
        return note;
    return note.substr(0, first) + " -> " + note.substr(last + 3);
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
constexpr int kBarH = 34;          // float composition row: caption line + bar
constexpr int kBarCaptionH = 14;   // caption line inside the bar row
constexpr int kStatusH = 46;       // two rows: context | parity / messages
constexpr int kStatusRowH = 23;
constexpr int kPanelContentH = 200; // settings/help two-column content area
constexpr int kPanelRowH = 26;
constexpr int kMinClientH = kTopH + kPanelContentH + kStatusH; // 280
// Bytes page: a fixed number of rows, so the window never grows with the data;
// the wheel and the arrow keys scroll instead.
constexpr int kByteRows = 8;
constexpr int kByteRowH = 20;
constexpr int kByteHeadH = 18;
constexpr int kFormatRows = 10;   // format dropdown entries (rcFormatItem)
constexpr int kFormatRowH = 22;

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
// the settings tab so both always render the same way. A disabled segment
// keeps its state colour but dims the label (C mode derives the type).
void drawSegment(HDC dc, const RECT& r, const std::wstring& text, bool active, bool disabled = false)
{
    HBRUSH fill = CreateSolidBrush(active ? kAccent : kBitOff);
    FillRect(dc, &r, fill);
    DeleteObject(fill);
    frameRect(dc, r, kSeparator);
    drawCenteredText(dc, r, text, disabled ? kSeparator : (active ? kBitOnText : kBitOffText));
}

const char* tabLabel(int i, bool zh)
{
    static const char* const kZh[5] = { "位运算", "字节流", "结构体", "设置", "帮助" };
    static const char* const kEn[5] = { "Bits", "Bytes", "Struct", "Settings", "Help" };
    return zh ? kZh[i] : kEn[i];
}

// The type row: integer signedness and the float reading share one segment,
// because a result is exactly one of the three at a time.
const char* kindLabel(bool zh)
{
    return zh ? "类型" : "Type";
}

const char* kindName(int i, bool zh)
{
    static const char* const kZh[3] = { "无符号 u", "有符号 i", "浮点 f" };
    static const char* const kEn[3] = { "unsigned u", "signed i", "float f" };
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

// Sign / exponent / mantissa colour, shared by the grid band and the bar. The
// field a bit belongs to comes from the container table, so a new row there
// needs no change here.
COLORREF floatSegColorByField(int segment)
{
    switch (segment) {
    case 2: return RGB(190, 130, 240); // S
    case 1: return RGB(88, 166, 255);  // E
    default: return RGB(127, 209, 168); // M
    }
}

COLORREF floatSegColor(int bit, const binlab::FloatSpec& f)
{
    return floatSegColorByField(binlab::floatSegment(bit, f));
}

// num/den of `a` blended into `b`: used for the dimmed ghost band and for the
// hover glow, both of which must stay readable against the cell colours.
COLORREF mixColor(COLORREF a, COLORREF b, int num, int den)
{
    const int nr = (GetRValue(a) * (den - num) + GetRValue(b) * num) / den;
    const int ng = (GetGValue(a) * (den - num) + GetGValue(b) * num) / den;
    const int nb = (GetBValue(a) * (den - num) + GetBValue(b) * num) / den;
    return RGB(nr, ng, nb);
}

// How the pattern on display can be read as a float. `spec` is null when the
// width has no IEEE-754 container at all. `ghost` marks the hypothetical case:
// an integer result whose bits *would* form a valid float of that width, which
// the bar shows dimmed because it is not what the expression computed.
struct FloatView {
    const binlab::FloatSpec* spec = nullptr;
    bool ghost = false;
};

FloatView floatView()
{
    const EditorState& s = state();
    FloatView v;
    v.spec = binlab::floatByBits(s.width);
    v.ghost = v.spec != nullptr && !s.isFloat;
    if (v.ghost && !s.floatBar)
        v.spec = nullptr; // a hypothetical reading is only shown when asked for
    return v;
}

// Display order in the bar is MSB first (S | E | M); floatSegment() numbers the
// fields the other way round (0 = mantissa), and this is the only mapping.
int barIndexForSegment(int segment)
{
    return 2 - segment;
}

std::string fieldHex(std::uint64_t v)
{
    char b[24];
    std::snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

// One fixed caption line: what the bits are read as, or, while the cursor is on
// a field, that field's own numbers. Keeping it one line is what lets the bar
// row stay exactly kBarH tall for every result type.
std::string barCaption(const FloatView& v, int hoverSeg, bool zh)
{
    EditorState& s = state();
    if (!v.spec) {
        return zh ? std::to_string(s.width) + " 位无 IEEE-754 容器 · 改用 16/32/64 位或 "
                       + binlab::floatNameList()
                  : std::to_string(s.width) + "-bit: no IEEE-754 container · use 16/32/64 or "
                    + binlab::floatNameList();
    }
    const binlab::FloatSpec& f = *v.spec;
    const std::string layout = std::to_string(1) + "+" + std::to_string(f.expBits) + "+"
                             + std::to_string(f.mantBits);
    if (hoverSeg >= 0) {
        const binlab::FloatFields g = binlab::floatFields(s.value, f);
        const int lo = hoverSeg == 2 ? f.bits - 1 : f.mantBits;
        const int hi = hoverSeg == 0 ? f.mantBits - 1 : f.bits - 2;
        const char* name[3] = { "M", "E", "S" };
        std::string out = std::string(name[hoverSeg]) + " ";
        if (hoverSeg == 2) {
            out += zh ? "符号 1 位 · 位 " + std::to_string(lo) + " = " + std::to_string(g.sign)
                           + (g.sign ? (zh ? "（负）" : " (negative)") : (zh ? "（正）" : " (positive)"))
                      : "sign 1 bit · bit " + std::to_string(lo) + " = " + std::to_string(g.sign)
                           + (g.sign ? " (negative)" : " (positive)");
        } else if (hoverSeg == 1) {
            const long long unbiased = static_cast<long long>(g.exp) - f.bias;
            out += zh ? "指数 " + std::to_string(f.expBits) + " 位 · 位 " + std::to_string(lo) + "-"
                           + std::to_string(hi) + " = " + fieldHex(g.exp)
                           + "（去偏移 " + std::to_string(unbiased) + "，偏移 " + std::to_string(f.bias) + "）"
                      : "exponent " + std::to_string(f.expBits) + " bits · bit " + std::to_string(lo) + "-"
                           + std::to_string(hi) + " = " + fieldHex(g.exp) + " (unbiased "
                           + std::to_string(unbiased) + ", bias " + std::to_string(f.bias) + ")";
        } else {
            out += zh ? "尾数 " + std::to_string(f.mantBits) + " 位 · 位 0-" + std::to_string(hi)
                           + " = " + fieldHex(g.mant)
                           + (g.exp == 0 ? " · 隐含前导 0" : " · 隐含前导 1")
                      : "mantissa " + std::to_string(f.mantBits) + " bits · bit 0-" + std::to_string(hi)
                           + " = " + fieldHex(g.mant)
                           + (g.exp == 0 ? " · implicit leading 0" : " · implicit leading 1");
        }
        return out;
    }
    const std::string special = binlab::floatSpecialLabel(s.value, f);
    std::string out = v.ghost
        ? binlab::typeNameFor(s.width, s.isSigned) + " · "
              + (zh ? "假设按 " : "ghost-read as ") + f.name + (zh ? " 解释 · " : " · ")
        : std::string(f.name) + " · IEEE-754 " + f.ieee + " · ";
    out += layout;
    if (!special.empty())
        out += " · " + special;
    // A ghost reading is only a hypothesis about the same bits, and the caption
    // has to say so instead of showing a number that is not the result.
    out += v.ghost ? (zh ? " · 非当前结果" : " · not the current result")
                   : " · " + s.floatText;
    return out;
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

// A bit edit changes the pattern a float reading decodes, so the FLOAT text has
// to be re-derived from the new bits. Leaving it alone kept the number the
// expression produced before the edit: painting all 32 bits to 1 still showed
// the old "1.2" instead of NaN. The unit note describes the literal, not these
// bits, so it goes away with the edit.
void refreshFloatReadout()
{
    EditorState& s = state();
    if (!s.isFloat)
        return;
    const binlab::FloatSpec* f = binlab::floatByBits(s.width);
    if (f == nullptr)
        return;
    s.floatText = binlab::exactFloatText(s.value & binlab::maskFor(s.width), *f);
    s.unitNote.clear();
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

int bytesMinWidth(HDC dc);
void computeBytesLayout(HDC dc);
void logBytesLayout(std::string& line);
void evaluateBytes();

// Compute every cached geometry rect from the current state.
void computeLayout(HDC dc)
{
    EditorState& s = state();
    const bool bits = s.mode == EditorSpec::Mode::Bits;
    const bool bytes = s.mode == EditorSpec::Mode::Bytes;

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
    const std::wstring toolSignLabel = wideFromUtf8(kindLabel(s.zh));
    const int widthLabelW = textWidth(dc, toolFont, toolWidthLabel) + 10;
    const int signLabelW = textWidth(dc, toolFont, toolSignLabel) + 10;
    // Wide enough for the float names the same buttons show in float mode.
    const int widthBtnW = std::max(36, textWidth(dc, toolFont, L"f64") + 16);
    int signBtnW[3];
    for (int i = 0; i < 3; ++i)
        signBtnW[i] = textWidth(dc, toolFont, wideFromUtf8(kindName(i, s.zh))) + 16;
    DeleteObject(toolFont);
    const int toolbarW = kGridMargin + widthLabelW + 4 * widthBtnW + 20 + signLabelW
                       + signBtnW[0] + signBtnW[1] + signBtnW[2] + kGridMargin;

    int clientW = std::max(480, 2 * kGridMargin + 8 * kMinCellW + kNibbleGap);
    if (bits)
        clientW = std::max(clientW, toolbarW);
    if (bytes)
        clientW = std::max(clientW, bytesMinWidth(dc));
    if (!chips.empty()) {
        if (oneRow + 24 <= 616)
            clientW = std::max(clientW, oneRow + 24);
        else
            clientW = std::max(clientW, 616);
    }
    s.clientW = clientW;

    // Five tabs fit both languages in the narrowest window (measured:
    // 298 px zh / 287 px en at 480 px client width), so the right end of the
    // top bar is deliberately left empty. The float-bar pill lives there, and
    // is sized from measured label text for the same reason. The bar is a
    // bits-page tool, so the pill only exists in bits mode.
    if (bits) {
        HFONT pillFont = uiFont(12, false, s.zh);
        const int lw = textWidth(dc, pillFont, wideFromUtf8(s.zh ? "浮点条带" : "float bar"));
        DeleteObject(pillFont);
        const int pw = lw + 22;
        s.rcBarToggle = { s.clientW - 12 - pw, 6, s.clientW - 12, kTopH - 6 };
    } else {
        s.rcBarToggle = {};
    }
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
        for (int i = 0; i < 3; ++i) {
            s.rcKindBtn[i] = { tx, s.toolTop, tx + signBtnW[i], s.toolTop + kToolH };
            tx += signBtnW[i];
        }
        contentTop = s.toolTop + kToolH + 8;
    } else {
        s.toolTop = 0;
        for (RECT& r : s.rcWidthBtn)
            r = {};
        for (RECT& r : s.rcKindBtn)
            r = {};
    }
    s.gridTop = contentTop + kLabelH;
    s.gridLeft = kGridMargin;
    // Fill the width: 8 cells (each drawn 4px narrower than its pitch) plus
    // the nibble gap span exactly clientW - 2 * margin.
    s.cellW = std::max(kMinCellW, (s.clientW - 2 * kGridMargin - kNibbleGap + 4) / 8);
    if (bits) {
        // The composition bar is the last content row: the grid never moves
        // because of it, and its height follows the toggle alone.
        const int gridBottom = s.gridTop + s.gridRows * (kLabelH + kCellH + kRowGap);
        s.barTop = s.floatBar ? gridBottom + 8 : 0;
        s.clientH = gridBottom + (s.floatBar ? kBarH : 0) + 8 + kStatusH;
        for (RECT& r : s.rcBarSeg)
            r = {};
        if (s.floatBar) {
            const int W = s.clientW - 24;
            const int segTop = s.barTop + kBarCaptionH + 2;
            const int segBottom = s.barTop + kBarH - 2;
            const FloatView view = floatView();
            if (view.spec) {
                const binlab::FloatSpec& f = *view.spec;
                // Proportional to the field widths, with floors so a 1-bit sign
                // and a 5-bit exponent stay clickable and labelable at all.
                int wS = std::max(20, W / f.bits);
                int wE = std::max(46, W * f.expBits / f.bits);
                if (wS + wE > W - 46)
                    wE = std::max(46, W - 46 - wS);
                const int wM = W - wS - wE;
                const int x0 = 12;
                s.rcBarSeg[barIndexForSegment(2)] = { x0, segTop, x0 + wS, segBottom };
                s.rcBarSeg[barIndexForSegment(1)] = { x0 + wS, segTop, x0 + wS + wE, segBottom };
                s.rcBarSeg[barIndexForSegment(0)] = { x0 + wS + wE, segTop, x0 + wS + wE + wM, segBottom };
            }
            // Without a container no rect is filled, so nothing in the grey band
            // is hover-able and the caption alone carries the explanation.
        }
        s.panelTop = s.panelBottom = 0;
        s.panelCol2 = 0;
        for (RECT& r : s.rcSetWidth)
            r = {};
        for (RECT& r : s.rcSetSign)
            r = {};
        for (RECT& r : s.rcSetMode)
            r = {};
        s.rcClearHistory = {};
    } else if (bytes) {
        s.panelTop = s.panelBottom = 0;
        s.barTop = 0;
        for (RECT& r : s.rcBarSeg)
            r = {};
        computeBytesLayout(dc);
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
        for (int i = 0; i < 2; ++i)
            s.rcSetMode[i] = { bx + i * 56, rowTop + 5 * kPanelRowH + 2,
                               bx + i * 56 + 52, rowTop + 6 * kPanelRowH - 2 };
        s.rcClearHistory = { s.panelCol2, rowTop + 5 * kPanelRowH + 2,
                             s.panelCol2 + 132, rowTop + 6 * kPanelRowH - 2 };
        s.barTop = 0;
        for (RECT& r : s.rcBarSeg)
            r = {};
    }
    s.clientH = std::max(s.clientH, kMinClientH);
    // The single-line EDIT is made exactly one text line tall. A control taller
    // than its line centres the glyphs but pins the caret to the top of the
    // line box, which reads as a caret floating above the text; with zero slack
    // the caret and the characters share one box, and rcExpr centres that box in
    // the band. The empty-state hint uses the same metrics, so it agrees too.
    {
        HFONT editProbe = monoFont(13, false);
        HGDIOBJ oldProbe = SelectObject(dc, editProbe);
        TEXTMETRICW etm {};
        GetTextMetricsW(dc, &etm);
        SelectObject(dc, oldProbe);
        DeleteObject(editProbe);
        s.editH = std::max(12, static_cast<int>(etm.tmHeight));
    }
    // The bytes page has no history dropdown, so its line runs to the edge.
    s.rcExpr = { 64, kTopH + (kExprH - s.editH) / 2,
                 bytes ? s.clientW - 12 : s.clientW - 12 - kHistBtnW - 6, kTopH + (kExprH + s.editH) / 2 };
    s.rcHistoryBtn = { s.rcExpr.right + 6, s.rcExpr.top, s.clientW - 12, s.rcExpr.bottom };
    {
        // Full-width panel: a narrow right-aligned dropdown would cut through
        // the middle of a result chip and leave half of its text visible.
        const int hw = s.clientW - 24;
        const int hx = 12;
        const int hy = s.rcExpr.bottom + 4;
        for (int i = 0; i < kHistRows; ++i)
            s.rcHistoryItem[i] = { hx, hy + i * kHistRowH, hx + hw, hy + (i + 1) * kHistRowH };
    }

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
              + " sgn1=" + rectStr(s.rcKindBtn[1])
              + " kind0=" + rectStr(s.rcKindBtn[0])
              + " kind2=" + rectStr(s.rcKindBtn[2]);
        for (int i = 0; i < 4; ++i)
            line += " parity" + std::to_string(i) + "=" + rectStr(s.rcParity[i]);
        for (int i = 0; i < 3; ++i)
            line += " bar" + std::to_string(i) + "=" + rectStr(s.rcBarSeg[i]);
        line += " barTop=" + std::to_string(s.barTop);
    }
    if (s.mode == EditorSpec::Mode::Bytes)
        logBytesLayout(line);
    line += " expr=" + rectStr(s.rcExpr)
          + " histBtn=" + rectStr(s.rcHistoryBtn)
          + " barToggle=" + rectStr(s.rcBarToggle)
          + " hist0=" + rectStr(s.rcHistoryItem[0])
          + " clearHist=" + rectStr(s.rcClearHistory)
          + " setw0=" + rectStr(s.rcSetWidth[0])
          + " setsgn1=" + rectStr(s.rcSetSign[1])
          + " setmode1=" + rectStr(s.rcSetMode[1])
          + " cmode=" + std::to_string(s.cMode ? 1 : 0)
          + " float=" + std::to_string(s.isFloat ? 1 : 0)
          + " floatbar=" + std::to_string(s.floatBar ? 1 : 0)
          + " histCount=" + std::to_string(s.history.size())
          + " histPersist=" + std::to_string(s.historyPersistent ? 1 : 0);
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
        const bool hasLine = s.mode == EditorSpec::Mode::Bits || s.mode == EditorSpec::Mode::Bytes;
        ShowWindow(s.edit, hasLine ? SW_SHOWNOACTIVATE : SW_HIDE);
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
    case 'D': return s.isFloat ? s.floatText : r.dec;
    case 'A': {
        std::string out = "HEX " + r.hex + "\nOCT " + r.oct + "\nBIN " + r.bin + "\n";
        out += s.isFloat ? "FLOAT " + s.floatText : "DEC " + r.dec;
        if (!s.unitNote.empty())
            out += "\nunit " + s.unitNote;
        return out;
    }
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

// A bar block copies its own field, like every other clickable readout here.
void copyBarSegment(HWND hwnd, int segment)
{
    EditorState& s = state();
    const FloatView view = floatView();
    if (!view.spec || !s.evalError.empty())
        return;
    const binlab::FloatFields g = binlab::floatFields(s.value, *view.spec);
    const std::uint64_t v = segment == 2 ? g.sign : segment == 1 ? g.exp : g.mant;
    static const char* const kNames[3] = { "M", "E", "S" };
    const std::string text = fieldHex(v);
    const bool ok = copyToClipboard(hwnd, text);
    winshun::debug(std::string("ra9fael.binarylab: copyBar ") + kNames[segment]
                   + " value=" + text + " ok=" + (ok ? "1" : "0"));
    s.msg = ok ? (s.zh ? std::string("已复制 ") + kNames[segment] : std::string("Copied ") + kNames[segment])
               : (s.zh ? "复制失败" : "copy failed");
    s.msgTick = GetTickCount();
    InvalidateRect(hwnd, nullptr, TRUE);
}

void evaluate(); // defined below; commit/history helpers force a re-run
void setKind(HWND hwnd, int kind);

void showMsg(HWND hwnd, const std::string& text)
{
    EditorState& s = state();
    s.msg = text;
    s.msgTick = GetTickCount();
    InvalidateRect(hwnd, nullptr, TRUE);
}

// Replace the whole expression line; EN_CHANGE re-evaluates it, which in turn
// adopts the width and float flag of the new prefix and relays the window out.
void replaceExpression(const std::string& text)
{
    EditorState& s = state();
    if (!s.edit)
        return;
    const std::wstring w = wideFromUtf8(text);
    SetWindowTextW(s.edit, w.c_str());
    SendMessageW(s.edit, 0x00B1 /* EM_SETSEL */, static_cast<WPARAM>(w.size()),
                 static_cast<LPARAM>(w.size()));
}

std::string expressionText()
{
    EditorState& s = state();
    if (!s.edit)
        return {};
    const int len = GetWindowTextLengthW(s.edit);
    std::wstring w(static_cast<std::size_t>(len > 0 ? len : 0), L'\0');
    if (len > 0)
        GetWindowTextW(s.edit, w.data(), len + 1);
    return binlab::trim(utf8FromWide(w));
}

// Enter = submit: force one re-evaluation, then record a CONFIRMED expression.
// Live EN_CHANGE evaluation stays in place, so typing never needs Enter; the
// history therefore contains intentional expressions, not every keystroke.
void submitHistory(HWND hwnd)
{
    EditorState& s = state();
    const std::string expr = expressionText();
    if (expr.empty() || !s.evalError.empty()) {
        s.msg = s.zh ? "表达式无效，未记入历史" : "invalid expression, not saved";
        s.msgTick = GetTickCount();
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }
    if (s.history.empty() || s.history.back().expr != expr) {
        const binlab::Repr r = binlab::represent(s.value, s.width, s.isSigned);
        HistoryEntry e { expr, s.isFloat ? s.floatText : r.hex,
                         binlab::typeNameFor(s.width, s.isSigned) };
        s.history.push_back(e);
        if (s.history.size() > kMaxHistory)
            s.history.erase(s.history.begin());
        appendHistoryFile(e);
    }
    s.historyPos = -1;
    s.historyOpen = false;
    s.msg = s.zh ? "已提交 · 记入历史" : "submitted · saved to history";
    s.msgTick = GetTickCount();
    InvalidateRect(hwnd, nullptr, TRUE);
}

void commitExpression(HWND hwnd)
{
    EditorState& s = state();
    s.suppressEval = false;
    evaluate();
    submitHistory(hwnd);
    winshun::debug("ra9fael.binarylab: commit expr=" + expressionText());
}

// Up/Down browse submitted history; returning past the newest entry restores
// whatever was typed before browsing began.
void historyStep(HWND hwnd, bool up)
{
    EditorState& s = state();
    if (s.history.empty() || !s.edit)
        return;
    if (s.historyPos < 0) {
        if (!up)
            return;
        s.histDraft = expressionText();
        s.historyPos = static_cast<int>(s.history.size()) - 1;
    } else if (up) {
        if (s.historyPos > 0)
            --s.historyPos;
    } else {
        if (s.historyPos + 1 < static_cast<int>(s.history.size())) {
            ++s.historyPos;
        } else {
            s.historyPos = -1;
            s.suppressEval = true;
            SetWindowTextW(s.edit, wideFromUtf8(s.histDraft).c_str());
            s.suppressEval = false;
            evaluate();
            InvalidateRect(hwnd, nullptr, TRUE);
            return;
        }
    }
    const std::string& text = s.history[static_cast<std::size_t>(s.historyPos)].expr;
    SetWindowTextW(s.edit, wideFromUtf8(text).c_str());
    SendMessageW(s.edit, 0x00B1 /* EM_SETSEL */, static_cast<WPARAM>(text.size()),
                 static_cast<LPARAM>(text.size()));
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
    const auto r = binlab::evalBits(expr, s.width, s.isSigned,
                                    s.cMode ? binlab::EvalMode::C : binlab::EvalMode::Fixed);
    if (r.success) {
        s.evalError.clear();
        s.value = r.value.result;
        // A float literal makes HEX/OCT/BIN show its IEEE-754 bit pattern and
        // the DEC chip show the decoded decimal text instead.
        s.isFloat = r.value.isFloat;
        s.floatText = r.value.floatText;
        s.unitNote = r.value.unitNote;
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
        s.isFloat = false;
        s.floatText.clear();
        s.unitNote.clear();
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
    if (s.isFloat) {
        if (isSigned != s.isSigned) {
            setKind(hwnd, isSigned ? 1 : 0);
            return;
        }
        // Between containers the *value* is what the user is looking at, so
        // it is re-encoded: the shortest round-trip text of the old pattern
        // becomes the literal under the new prefix (f32 1.5 -> f64 1.5).
        // Rounding and range loss land in the detail like any other literal.
        const binlab::FloatSpec* to = binlab::floatByBits(width);
        if (to == nullptr) {
            showMsg(hwnd, s.zh ? std::to_string(width) + " 位没有 IEEE-754 容器"
                               : std::to_string(width) + "-bit has no IEEE-754 container");
            return;
        }
        std::string text = s.floatText;
        if (text == "+Inf")
            text = "Inf";
        replaceExpression(std::string(to->name) + " " + text);
        showMsg(hwnd, s.zh ? std::string("按数值重编码为 ") + to->name
                           : std::string("re-encoded as ") + to->name);
        winshun::debug(std::string("ra9fael.binarylab: setType ") + to->name);
        return;
    }
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

// Integer <-> float keeps the bits and changes only how they are read, the
// same reading the float bar offers as a ghost. Between containers setType()
// re-encodes the value instead. kind: 0 = unsigned, 1 = signed, 2 = float.
void setKind(HWND hwnd, int kind)
{
    EditorState& s = state();
    const int current = s.isFloat ? 2 : (s.isSigned ? 1 : 0);
    if (kind == current)
        return;
    if (kind < 2 && !s.isFloat) {
        setType(hwnd, s.width, kind == 1);
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }
    const std::string hex = binlab::hexUpper(s.value & binlab::maskFor(s.width), s.width);
    if (kind == 2) {
        const binlab::FloatSpec* f = binlab::floatByBits(s.width);
        if (f == nullptr) {
            showMsg(hwnd, s.zh ? std::to_string(s.width) + " 位没有 IEEE-754 容器"
                               : std::to_string(s.width) + "-bit has no IEEE-754 container");
            return;
        }
        replaceExpression(std::string(f->name) + " " + hex);
        showMsg(hwnd, s.zh ? std::string("位不变 · 按 ") + f->name + " 解释"
                           : std::string("same bits · read as ") + f->name);
        winshun::debug(std::string("ra9fael.binarylab: setKind ") + f->name);
        return;
    }
    // '@' marks a raw pattern, so a signed type accepts a set top bit.
    const std::string type = binlab::typeNameFor(s.width, kind == 1);
    replaceExpression(type + (kind == 1 ? " @" : " ") + hex);
    showMsg(hwnd, s.zh ? "位不变 · 按 " + type + " 解释" : "same bits · read as " + type);
    winshun::debug("ra9fael.binarylab: setKind " + type);
}

// ---- bash-style expression-line editing ------------------------------------
// Word semantics follow bash: a run of separator characters plus the adjacent
// run of word characters is one word, so killing backward from "0xA5 &|"
// removes "& " and then "0xA5". Kills land in the kill buffer (Ctrl+Y yanks)
// and go through EM_REPLACESEL, which keeps them on the native undo stack.

// Not constexpr: std::iswalnum is not a constant expression in the standard,
// and MSVC rejects the declaration outright (C3615) even though GCC accepts it.
bool isExprWordChar(wchar_t c)
{
    return std::iswalnum(static_cast<wint_t>(c)) || c == L'_' || c == L'.';
}

int wordStartBackward(const std::wstring& t, int caret)
{
    int i = caret;
    while (i > 0 && !isExprWordChar(t[static_cast<std::size_t>(i - 1)]))
        --i;
    while (i > 0 && isExprWordChar(t[static_cast<std::size_t>(i - 1)]))
        --i;
    return i;
}

int wordEndForward(const std::wstring& t, int caret)
{
    const int n = static_cast<int>(t.size());
    int i = caret;
    while (i < n && !isExprWordChar(t[static_cast<std::size_t>(i)]))
        ++i;
    while (i < n && isExprWordChar(t[static_cast<std::size_t>(i)]))
        ++i;
    return i;
}

struct EditBuffer {
    std::wstring text;
    int caret = 0; // active selection end: kills anchor here
};

EditBuffer editBuffer(HWND h)
{
    const int len = GetWindowTextLengthW(h);
    EditBuffer b;
    b.text.assign(static_cast<std::size_t>(len > 0 ? len : 0), L'\0');
    if (len > 0)
        GetWindowTextW(h, b.text.data(), len + 1);
    DWORD a = 0, z = 0;
    SendMessageW(h, 0x00B0 /* EM_GETSEL */, reinterpret_cast<WPARAM>(&a), reinterpret_cast<LPARAM>(&z));
    b.caret = static_cast<int>(z);
    return b;
}

void killRange(HWND h, int from, int to)
{
    const int n = static_cast<int>(editBuffer(h).text.size());
    from = std::max(0, std::min(from, n));
    to = std::max(from, std::min(to, n));
    if (from == to)
        return;
    EditorState& s = state();
    EditBuffer b = editBuffer(h);
    s.killBuf = b.text.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
    SendMessageW(h, 0x00B1 /* EM_SETSEL */, static_cast<WPARAM>(from), static_cast<LPARAM>(to));
    SendMessageW(h, 0x00C2 /* EM_REPLACESEL */, TRUE, reinterpret_cast<LPARAM>(L""));
}

LRESULT CALLBACK editProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    EditorState& s = state();
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            if (s.historyOpen) {
                s.historyOpen = false;
                InvalidateRect(s.hwnd, nullptr, TRUE);
                return 0;
            }
            const EditBuffer b = editBuffer(h);
            if (b.text.empty()) {
                if (s.hwnd)
                    DestroyWindow(s.hwnd);
                return 0;
            }
            killRange(h, 0, static_cast<int>(b.text.size())); // Esc clears first
            return 0;
        }
        if (wp == VK_RETURN) {
            if (s.mode == EditorSpec::Mode::Bytes) {
                evaluateBytes();
                return 0;
            }
            commitExpression(s.hwnd);
            return 0;
        }
        if (wp == VK_TAB) {
            SetFocus(s.hwnd); // hand the keyboard back to the grid
            return 0;
        }
        if (wp == VK_DELETE && GetKeyState(VK_CONTROL) < 0) {
            const EditBuffer b = editBuffer(h);
            killRange(h, b.caret, wordEndForward(b.text, b.caret));
            return 0;
        }
    }
    if (msg == WM_SYSKEYDOWN) {
        // Alt+Up / Alt+Down browse history (plain arrows stay native caret keys).
        if (wp == VK_UP) {
            historyStep(s.hwnd, true);
            return 0;
        }
        if (wp == VK_DOWN) {
            historyStep(s.hwnd, false);
            return 0;
        }
    }
    if (msg == WM_SYSCHAR && wp == 0x08) { // Alt+Backspace = backward kill word
        const EditBuffer b = editBuffer(h);
        killRange(h, wordStartBackward(b.text, b.caret), b.caret);
        return 0;
    }
    // Bash control characters. They arrive here (not WM_KEYDOWN): returning 0
    // from a key handler cannot stop TranslateMessage, so intercepting the
    // WM_CHAR codes is the only way to keep the native EDIT from double-
    // processing them. Plain chars fall through untouched, as do the native
    // Ctrl+Z (0x1A undo) / Ctrl+X / C / V. 0x7F is Ctrl+Backspace.
    if (msg == WM_CHAR && (wp < 0x20 || wp == 0x7F)) {
        const EditBuffer b = editBuffer(h);
        switch (wp) {
        case 0x7F: // Ctrl+Backspace
        case 0x17: // Ctrl+W
            killRange(h, wordStartBackward(b.text, b.caret), b.caret);
            return 0;
        case 0x15: // Ctrl+U: kill to line start
            killRange(h, 0, b.caret);
            return 0;
        case 0x0B: // Ctrl+K: kill to line end
            killRange(h, b.caret, static_cast<int>(b.text.size()));
            return 0;
        case 0x19: // Ctrl+Y: yank the kill buffer
            SendMessageW(h, 0x00C2 /* EM_REPLACESEL */, TRUE,
                         reinterpret_cast<LPARAM>(s.killBuf.c_str()));
            return 0;
        case 0x0C: // Ctrl+L: clear the line (yankable)
            killRange(h, 0, static_cast<int>(b.text.size()));
            return 0;
        case 0x01: // Ctrl+A: line start
            SendMessageW(h, 0x00B1 /* EM_SETSEL */, 0, 0);
            return 0;
        case 0x05: { // Ctrl+E: line end
            const int n = static_cast<int>(b.text.size());
            SendMessageW(h, 0x00B1 /* EM_SETSEL */, static_cast<WPARAM>(n), static_cast<LPARAM>(n));
            return 0;
        }
        }
    }
    if (msg == WM_PAINT) {
        const LRESULT r = CallWindowProcW(s.origEditProc, h, msg, wp, lp);
        // EM_SETCUEBANNER needs visual styles this process does not enable,
        // so draw the empty-expression hint ourselves. Same mono font and rect
        // as the real text, so the hint sits on the identical baseline.
        if (GetWindowTextLengthW(h) == 0) {
            RECT rc {};
            GetClientRect(h, &rc);
            HDC dc = GetDC(h);
            HFONT font = monoFont(13, false);
            HGDIOBJ old = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(128, 128, 128));
            RECT t { rc.left + 8, rc.top, rc.right - 6, rc.bottom };
            const std::wstring cue = s.mode == EditorSpec::Mode::Bytes
                ? (s.zh ? L"hex u16le 34 12 78 56 · fmt blm_rawdata_256 <hex...>"
                        : L"hex u16le 34 12 78 56 · fmt blm_rawdata_256 <hex...>")
                : (s.zh ? L"输入表达式 · u8 0xA5 & 0x0F · 4M · f64 1.5"
                        : L"Expr · u8 0xA5 & 0x0F · 4M · f64 1.5");
            DrawTextW(dc, cue.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
            DeleteObject(font);
            ReleaseDC(h, dc);
        }
        return r;
    }
    return CallWindowProc(s.origEditProc, h, msg, wp, lp);
}

// ---- bytes page (v0.3b) -------------------------------------------------------
// One expression line, two readings: `hex u16le 34 12 ...` is the scalar
// decoder (evalBytes, the query's own function) and `fmt <name> ...` is the
// record decoder (evalRecords). The page keeps no bytes of its own: every
// toolbar click rewrites the expression, so the screen always shows what the
// line says and the line can be pasted back into the query box.

std::string joinTokens(const std::vector<std::string>& t)
{
    std::string out;
    for (const std::string& x : t)
        out += (out.empty() ? "" : " ") + x;
    return out;
}

bool isOrderWord(const std::string& t)
{
    const std::string o = binlab::lower(t);
    return o == "msb" || o == "lsb" || o == "msb-first" || o == "lsb-first";
}

bool exprIsRecords()
{
    const std::vector<std::string> t = binlab::splitWhitespace(expressionText());
    return !t.empty() && binlab::lower(t[0]) == "fmt";
}

COLORREF colorFromHex(const std::string& c, COLORREF fallback)
{
    if (!binlab::validColor(c))
        return fallback;
    const auto byte = [&](std::size_t i) { return binlab::digitValue(c[i]) * 16 + binlab::digitValue(c[i + 1]); };
    return RGB(byte(1), byte(3), byte(5));
}

std::string hex2(std::uint8_t b)
{
    char cell[4];
    std::snprintf(cell, sizeof cell, "%02X", b);
    return cell;
}

std::string hex4(std::size_t v)
{
    char cell[12];
    std::snprintf(cell, sizeof cell, "0x%04X", static_cast<unsigned>(v));
    return cell;
}

// Re-evaluate the bytes expression line. Each kind keeps its last good result:
// a failed `fmt ...` does not blank the scalar view, and the toolbar switches
// kinds without re-deriving bytes.
void evaluateBytes()
{
    EditorState& s = state();
    if (!s.edit || s.mode != EditorSpec::Mode::Bytes || s.suppressEval)
        return;
    const std::string expr = expressionText();
    s.bytesExpr = expr;
    if (expr.empty()) {
        s.evalError.clear();
        s.bytesIsRecords = false;
        return;
    }
    const std::vector<std::string> tokens = binlab::splitWhitespace(expr);
    const bool records = binlab::lower(tokens[0]) == "fmt";
    s.bytesIsRecords = records;
    if (records) {
        if (!s.formats) {
            s.evalError = "E_FORMAT: no record formats loaded";
            return;
        }
        if (tokens.size() == 1) {
            s.evalError.clear();
            s.recordsHas = false;
            s.recFmt = nullptr;
            relayout(s.hwnd);
            return;
        }
        const auto r = binlab::evalRecords(expr, *s.formats);
        if (r.success) {
            s.evalError.clear();
            s.recordsEval = r.value;
            s.recFmt = s.formats->find(r.value.formatName);
            s.recordsHas = true;
            if (s.selRecord >= r.value.records.size())
                s.selRecord = 0;
            if (s.recFmt && s.selField >= s.recFmt->columns.size())
                s.selField = 0;
            s.scrollRow = 0;
            s.firstCol = 0;
            relayout(s.hwnd);
            return;
        }
        s.evalError = r.failure.code + ": " + r.failure.message;
        winshun::debug("ra9fael.binarylab: evalError " + s.evalError);
        InvalidateRect(s.hwnd, nullptr, TRUE);
        return;
    }
    const auto r = binlab::evalBytes(expr, s.endian);
    if (r.success) {
        s.evalError.clear();
        s.bytesEval = r.value;
        s.bytesType = binlab::TypeSpec {};
        binlab::resolveType(tokens.size() > 1 ? tokens[1] : "u8", s.endian, s.bytesType);
        s.scalarHas = true;
        if (s.selAnchor >= r.value.data.size())
            s.selAnchor = 0;
        if (s.selActive >= r.value.data.size())
            s.selActive = std::min<std::size_t>(r.value.data.size(), 1) - (r.value.data.empty() ? 0 : 1);
        if (s.selActive >= r.value.data.size())
            s.selActive = 0;
        s.scrollRow = 0;
        relayout(s.hwnd);
        return;
    }
    s.evalError = r.failure.code + ": " + r.failure.message;
    winshun::debug("ra9fael.binarylab: evalError " + s.evalError);
    InvalidateRect(s.hwnd, nullptr, TRUE);
}

// ---- bytes geometry -----------------------------------------------------------

// One record row, offset line and byte cell, in client coordinates. Row 0 is
// the header; the wheel scrolls.
RECT byteCell(std::size_t byteIndex)
{
    const EditorState& s = state();
    const std::size_t row = byteIndex / 16 - s.scrollRow;
    const int col = static_cast<int>(byteIndex % 16);
    const int left = s.offW + 8 + col * s.byteW + (col >= 8 ? kNibbleGap : 0);
    const int top = s.byteTop + static_cast<int>(row) * kByteRowH + kByteHeadH;
    return { left, top, left + s.byteW - 2, top + kByteRowH - 2 };
}

int recColumnLeft(std::size_t col)
{
    const EditorState& s = state();
    int x = s.offW + 8;
    for (std::size_t c = s.firstCol; c < col; ++c)
        x += (c < s.colChars.size() ? s.colChars[c] * s.monoW + 24 : 0);
    return x;
}

int recColumnWidth(std::size_t col)
{
    const EditorState& s = state();
    return col < s.colChars.size() ? s.colChars[col] * s.monoW + 24 : 0;
}

std::size_t recColumns()
{
    const EditorState& s = state();
    return s.recFmt ? s.recFmt->columns.size() : (s.recordsEval.records.empty() ? 0 : s.recordsEval.records[0].cells.size());
}

std::size_t recordsFit()
{
    // 8 grid rows: one header line is painted above, so 7 records fit. Kept at
    // least 1 so an empty table still has a row to draw its hint in.
    return std::max<std::size_t>(1, kByteRows - 1);
}

std::size_t bytesFitRows()
{
    return kByteRows;  // fixed row count: the window height never follows the data
}

std::size_t visibleBytes()
{
    return bytesFitRows() * 16;
}

std::size_t bytesRowsTotal()
{
    const EditorState& s = state();
    return (s.bytesEval.data.size() + 15) / 16;
}

std::size_t maxScrollRow()
{
    const EditorState& s = state();
    if (s.bytesIsRecords) {
        const std::size_t total = s.recordsEval.records.size();
        const std::size_t fit = recordsFit();
        return total > fit ? total - fit : 0;
    }
    const std::size_t total = bytesRowsTotal();
    return total > bytesFitRows() ? total - bytesFitRows() : 0;
}

int bytesMinWidth(HDC dc)
{
    EditorState& s = state();
    HFONT f = monoFont(12, false);
    HGDIOBJ old = SelectObject(dc, f);
    // 16 byte cells, one nibble gap, an offset column and an ASCII tail.
    const int gw = 16 * (s.byteW + 2) + kNibbleGap + s.offW + 8 + 16 * s.monoW + 16;
    int tw = gw;
    // The toolbar: two label + segment groups.
    HFONT toolFont = uiFont(12, false, s.zh);
    SelectObject(dc, toolFont);
    const auto labelW = [&](const std::string& t) { return textWidth(dc, toolFont, wideFromUtf8(t)) + 10; };
    const int segW = std::max(textWidth(dc, toolFont, L"BE"), textWidth(dc, toolFont, L"f64")) + 16;
    // The kind buttons are sized from their labels — a fixed width truncates
    // "unsigned u" at zh text widths.
    int kindW = 0;
    for (int i = 0; i < 3; ++i)
        kindW = std::max(kindW, textWidth(dc, toolFont, wideFromUtf8(kindName(i, s.zh))) + 16);
    tw = std::max(tw, kGridMargin + labelW(s.zh ? "位宽" : "Width") + 4 * 36 + 16 + labelW(s.zh ? "类型" : "Type")
                           + 3 * kindW + 12 + labelW("LE/BE") + 2 * segW + kGridMargin);
    tw = std::max(tw, 560);
    // Word readout strip at the widest type (u64): HEX + DEC share row 1, the
    // BIN chip and the handoff button wrap to row 2.
    HFONT monoP = monoFont(12, false);
    SelectObject(dc, monoP);
    const int hexVW = textWidth(dc, monoP, wideFromUtf8("0x" + std::string(16, 'F')));
    const int decVW = textWidth(dc, monoP, wideFromUtf8(std::string(20, '8')));
    const int binVW = textWidth(dc, monoP, wideFromUtf8("0b" + std::string(64, '1')));
    SelectObject(dc, toolFont);
    const auto chipW = [&](const char* label, int valueW) {
        return 8 + textWidth(dc, toolFont, wideFromUtf8(label)) + 4 + valueW + 8;
    };
    const int btnW = textWidth(dc, toolFont, wideFromUtf8(s.zh ? "送到位运算页" : "Send to Bits")) + 24;
    tw = std::max(tw, 12 + chipW("HEX ", hexVW) + 6 + chipW("DEC ", decVW) + 12);
    tw = std::max(tw, 12 + chipW("BIN ", binVW) + 8 + btnW + 12);
    DeleteObject(monoP);
    SelectObject(dc, old);
    DeleteObject(toolFont);
    DeleteObject(f);
    return tw;
}

// Bytes-page layout. Everything is derived inside so the two readings stay in
// step: the toolbar row, then the fixed kByteRows rows, then the word readout
// row, with the status bar at the very bottom.
void computeBytesLayout(HDC dc)
{
    EditorState& s = state();
    HFONT probe = monoFont(12, false);
    HGDIOBJ old = SelectObject(dc, probe);
    TEXTMETRICW tm {};
    GetTextMetricsW(dc, &tm);
    s.monoW = std::max(6, static_cast<int>(tm.tmAveCharWidth));
    SelectObject(dc, old);
    DeleteObject(probe);

    const int contentTop = kTopH + kExprH + 8;
    // toolbar row
    s.toolTop = contentTop;
    HFONT toolFont = uiFont(12, false, s.zh);
    old = SelectObject(dc, toolFont);
    const auto labelW = [&](const std::string& t) { return textWidth(dc, toolFont, wideFromUtf8(t)) + 10; };
    const int segW = std::max(textWidth(dc, toolFont, L"BE"), textWidth(dc, toolFont, L"f64")) + 16;
    int x = kGridMargin;
    const bool records = s.bytesIsRecords;
    // (Re)measure the kind labels: in scalar mode they are u/i/f, in record
    // mode the row is the format dropdown + order buttons.
    if (!records) {
        s.rcToolLabel[0] = { x, s.toolTop, x += labelW(s.zh ? "位宽" : "Width"), s.toolTop + kToolH };
        for (int i = 0; i < 4; ++i) {
            s.rcWidthBtn[i] = { x, s.toolTop, x + 36, s.toolTop + kToolH };
            x += 36;
        }
        x += 12;
        s.rcToolLabel[1] = { x, s.toolTop, x += labelW(s.zh ? "类型" : "Type"), s.toolTop + kToolH };
        int kindW = 0;
        for (int i = 0; i < 3; ++i)
            kindW = std::max(kindW, textWidth(dc, toolFont, wideFromUtf8(kindName(i, s.zh))) + 16);
        for (int i = 0; i < 3; ++i) {
            s.rcKindBtn[i] = { x, s.toolTop, x + kindW, s.toolTop + kToolH };
            x += kindW;
        }
        x += 12;
        s.rcToolLabel[2] = { x, s.toolTop, x += labelW("LE/BE"), s.toolTop + kToolH };
        s.rcEndian[0] = { x, s.toolTop, x + segW, s.toolTop + kToolH };
        x += segW;
        s.rcEndian[1] = { x, s.toolTop, x + segW, s.toolTop + kToolH };
        x += segW;
        s.rcFormatBtn = {};
        for (RECT& r : s.rcOrder)
            r = {};
        for (RECT& r : s.rcFormatItem)
            r = {};
    } else {
        s.rcToolLabel[3] = { x, s.toolTop, x += labelW(s.zh ? "格式" : "Format"), s.toolTop + kToolH };
        HFONT probeFont = monoFont(12, false);
        SelectObject(dc, probeFont);
        int widest = 40;
        if (s.formats)
            for (const FormatEntry& e : s.formats->entries)
                widest = std::max(widest, textWidth(dc, probeFont, wideFromUtf8(e.name)) + 30);
        SelectObject(dc, toolFont);
        DeleteObject(probeFont);
        s.rcFormatBtn = { x, s.toolTop, x + widest, s.toolTop + kToolH };
        x = s.rcFormatBtn.right + 12;
        s.rcToolLabel[4] = { x, s.toolTop, x += labelW(s.zh ? "记录序" : "Order"), s.toolTop + kToolH };
        for (int i = 0; i < 2; ++i) {
            s.rcOrder[i] = { x, s.toolTop, x + segW, s.toolTop + kToolH };
            x += segW;
        }
        for (RECT& r : s.rcWidthBtn)
            r = {};
        for (RECT& r : s.rcKindBtn)
            r = {};
        for (RECT& r : s.rcEndian)
            r = {};
        // dropdown items below the button
        const int n = s.formats ? std::min<std::size_t>(kFormatRows, s.formats->entries.size()) : 0;
        for (int i = 0; i < n; ++i)
            s.rcFormatItem[i] = { s.rcFormatBtn.left, s.rcFormatBtn.bottom + 2 + i * kFormatRowH,
                                  s.rcFormatBtn.right, s.rcFormatBtn.bottom + 2 + (i + 1) * kFormatRowH };
        for (int i = n; i < kFormatRows; ++i)
            s.rcFormatItem[i] = {};
    }
    SelectObject(dc, old);
    DeleteObject(toolFont);

    // grid / table header
    s.byteTop = s.toolTop + kToolH + 6;
    s.offW = 40;
    s.byteW = 2 * s.monoW + 10;
    s.asciiLeft = s.offW + 8 + 16 * s.byteW + kNibbleGap + 4;

    // word/record readout row + handoff button; in scalar mode the strip
    // wraps BIN to a second row when the three chips cannot share the width.
    s.gridTop = s.byteTop + kByteHeadH + kByteRows * kByteRowH + 4;
    HFONT uiProbe = uiFont(12, false, s.zh);
    old = SelectObject(dc, uiProbe);
    const std::string toBits = s.zh ? "送到位运算页" : "Send to Bits";
    const int btnW = textWidth(dc, uiProbe, wideFromUtf8(toBits)) + 24;
    SelectObject(dc, old);
    DeleteObject(uiProbe);
    const int stripTop = s.gridTop;
    const int stripH = 26;
    const int strip2Top = stripTop + stripH + 4;
    s.rcToBits = { s.clientW - 12 - btnW, stripTop, s.clientW - 12, stripTop + stripH };
    if (!s.bytesIsRecords) {
        // Chip widths come from the current type's worst-case value: HEX
        // 0x + nibbles, DEC up to 20 digits (FLOAT keeps the same budget),
        // BIN 0b + every bit. Fixed widths clipped exactly the values the
        // chips exist to show.
        HFONT chipMono = monoFont(12, false);
        HFONT chipUi = uiFont(11, false, s.zh);
        old = SelectObject(dc, chipMono);
        const int typeBits = s.bytesType.size * 8; // worst-case value width in bits
        const int nibbles = std::max(1, (typeBits + 3) / 4);
        const int hexVW = textWidth(dc, chipMono,
                                    wideFromUtf8("0x" + std::string(static_cast<std::size_t>(nibbles), 'F')));
        const int decVW = textWidth(dc, chipMono, wideFromUtf8(std::string(20, '8')));
        const int binVW = textWidth(dc, chipMono,
                                    wideFromUtf8("0b" + std::string(static_cast<std::size_t>(typeBits), '1')));
        SelectObject(dc, chipUi);
        const auto cw = [&](const char* label, int valueW) {
            return 8 + textWidth(dc, chipUi, wideFromUtf8(label)) + 4 + valueW + 8;
        };
        const int hx = cw("HEX ", hexVW);
        const int dx = cw(s.bytesType.flt ? "FLOAT " : "DEC ", decVW);
        const int bx = cw("BIN ", binVW);
        SelectObject(dc, old);
        DeleteObject(chipMono);
        DeleteObject(chipUi);
        const bool wrap = 12 + hx + 6 + dx + 6 + bx + 8 + btnW + 12 > s.clientW;
        if (!wrap) {
            s.rcByteChip[0] = { 12, stripTop, 12 + hx, stripTop + stripH };
            s.rcByteChip[1] = { 12 + hx + 6, stripTop, 12 + hx + 6 + dx, stripTop + stripH };
            s.rcByteChip[2] = { 12 + hx + 6 + dx + 6, stripTop, 12 + hx + 6 + dx + 6 + bx, stripTop + stripH };
            s.clientH = std::max(s.clientH, stripTop + stripH + 10 + kStatusH);
        } else {
            s.rcByteChip[0] = { 12, stripTop, 12 + hx, stripTop + stripH };
            s.rcByteChip[1] = { 12 + hx + 6, stripTop, 12 + hx + 6 + dx, stripTop + stripH };
            s.rcByteChip[2] = { 12, strip2Top, 12 + bx, strip2Top + stripH };
            s.clientH = std::max(s.clientH, strip2Top + stripH + 10 + kStatusH);
        }
        s.rcCellChip = {};
    } else {
        s.rcByteChip[0] = {};
        s.rcByteChip[1] = {};
        s.rcByteChip[2] = {};
        s.rcCellChip = { 12, stripTop, s.rcToBits.left - 8, stripTop + stripH };
        s.clientH = std::max(s.clientH, stripTop + stripH + 10 + kStatusH);
    }
    // The interactive table area (records and scalar pages share the geometry).
    s.rcTable = { s.offW + 8, s.byteTop + kByteHeadH, s.clientW - 12,
                  s.byteTop + kByteHeadH + kByteRows * kByteRowH };
    s.bytesLaidOut = s.clientW * 4 + (s.bytesIsRecords ? 2 : 1) + static_cast<int>(s.clientH);
    (void)dc;
}

void logBytesLayout(std::string& line)
{
    EditorState& s = state();
    line += " bytesMode=" + std::string(s.bytesIsRecords ? "records" : "scalar")
          + " byteTop=" + std::to_string(s.byteTop)
          + " byteW=" + std::to_string(s.byteW)
          + " offW=" + std::to_string(s.offW)
          + " asciiLeft=" + std::to_string(s.asciiLeft)
          + " scrollRow=" + std::to_string(s.scrollRow)
          + " monoW=" + std::to_string(s.monoW);
    if (s.bytesIsRecords) {
        line += " table=" + rectStr(s.rcTable)
              + " fmtBtn=" + rectStr(s.rcFormatBtn)
              + " ord0=" + rectStr(s.rcOrder[0])
              + " ord1=" + rectStr(s.rcOrder[1])
              + " cellChip=" + rectStr(s.rcCellChip)
              + " firstCol=" + std::to_string(s.firstCol)
              + " selRecord=" + std::to_string(s.selRecord)
              + " selField=" + std::to_string(s.selField);
    } else {
        line += " endian=" + s.endian
              + " endianBtn0=" + rectStr(s.rcEndian[0])
              + " endianBtn1=" + rectStr(s.rcEndian[1])
              + " byte0=" + rectStr(byteCell(s.selActive < s.bytesEval.data.size() ? s.selActive : 0))
              + " chip0=" + rectStr(s.rcByteChip[0])
              + " toBits=" + rectStr(s.rcToBits)
              + " selBegin=" + std::to_string(std::min(s.selAnchor, s.selActive))
              + " selEnd=" + std::to_string(std::max(s.selAnchor, s.selActive));
    }
}

// Widest cell text per record column: header and the visible records.
void measureColumns(HDC dc)
{
    EditorState& s = state();
    s.colChars.assign(recColumns(), 4);
    if (!s.recFmt)
        return;
    for (std::size_t c = 0; c < s.colChars.size(); ++c)
        s.colChars[c] = std::max<std::size_t>(4, s.recFmt->columns[c].size());
    const std::size_t shown = std::min<std::size_t>(s.recordsEval.records.size(), recordsFit() + s.scrollRow);
    for (std::size_t r = s.scrollRow; r < shown; ++r) {
        const Record& rec = s.recordsEval.records[r];
        for (std::size_t c = 0; c < rec.cells.size() && c < s.colChars.size(); ++c)
            s.colChars[c] = std::max<int>(s.colChars[c], static_cast<int>(rec.cells[c].text.size()));
    }
    (void)dc;
}

// Byte index under the point, or SIZE_MAX.
std::size_t hitByte(int x, int y)
{
    const EditorState& s = state();
    if (y < s.byteTop + kByteHeadH || y >= s.byteTop + kByteHeadH + kByteRows * kByteRowH)
        return SIZE_MAX;
    for (std::size_t i = s.scrollRow * 16; i < s.bytesEval.data.size(); ++i) {
        if (i >= (s.scrollRow + bytesFitRows()) * 16)
            break;
        const RECT r = byteCell(i);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return i;
    }
    return SIZE_MAX;
}

// Record / column under the point.
bool hitRecordCell(int x, int y, std::size_t& rec, std::size_t& col)
{
    const EditorState& s = state();
    if (y < s.byteTop + kByteHeadH || y >= s.byteTop + kByteHeadH + kByteRows * kByteRowH)
        return false;
    const std::size_t fit = recordsFit();
    if (s.scrollRow + (y - s.byteTop - kByteHeadH) / kByteRowH >= s.recordsEval.records.size())
        return false;
    rec = s.scrollRow + static_cast<std::size_t>(y - s.byteTop - kByteHeadH) / kByteRowH;
    if (rec - s.scrollRow >= fit)
        return false;
    for (std::size_t c = s.firstCol; c < recColumns(); ++c) {
        const int left = recColumnLeft(c);
        if (x >= left && x < left + recColumnWidth(c)) {
            col = c;
            return true;
        }
    }
    return false;
}

std::size_t activeByte()
{
    const EditorState& s = state();
    return std::max(s.selAnchor, s.selActive);
}

// The word that covers the active byte: type width, little-endian value. The
// byte order of the reading follows the toolbar endian, so "select two bytes"
// is the LE/BE reading the user sees.
binlab::ByteWord wordAt(std::size_t byteIndex)
{
    const EditorState& s = state();
    binlab::ByteWord w;
    const int size = std::max(1, s.bytesType.size);
    const std::size_t width = static_cast<std::size_t>(size) * 8;
    w.offset = byteIndex;
    w.width = static_cast<int>(width);
    w.typeName = s.bytesType.name;
    w.isFloat = s.bytesType.flt != nullptr;
    std::uint64_t raw = 0;
    std::ostringstream bytes;
    for (int i = 0; i < size; ++i) {
        const std::size_t idx = byteIndex + i;
        std::uint8_t b = idx < s.bytesEval.data.size() ? s.bytesEval.data[idx] : 0;
        if (idx >= s.bytesEval.data.size())
            w.partial = true;
        const int shift = s.bytesType.big ? (size - 1 - i) : i;
        raw |= static_cast<std::uint64_t>(b) << (8 * shift);
        bytes << (i ? " " : "") << hex2(b);
    }
    w.bitsHex = binlab::hexUpper(raw & binlab::maskFor(static_cast<int>(width)), static_cast<int>(width));
    w.primary = w.isFloat ? binlab::formatFloat(raw, *s.bytesType.flt)
                          : (s.bytesType.signedType
                                 ? binlab::represent(raw, static_cast<int>(width), true).dec
                                 : w.bitsHex);
    std::ostringstream d;
    d << w.typeName << " @" << byteIndex << "  bytes " << bytes.str() << "\n"
      << (w.isFloat ? "FLOAT " + w.primary : "HEX " + w.bitsHex) << "\n"
      << "bits " << binlab::binGrouped(raw, static_cast<int>(width));
    w.detail = d.str();
    w.chain = "bits " + binlab::typeNameFor(static_cast<int>(width), false) + " " + w.bitsHex;
    return w;
}

std::string selectionSummary(const EditorState& s)
{
    const std::size_t b0 = std::min(s.selAnchor, s.selActive);
    const std::size_t b1 = std::max(s.selAnchor, s.selActive);
    if (s.bytesEval.data.empty() || b0 >= s.bytesEval.data.size())
        return {};
    std::string hex;
    for (std::size_t i = b0; i <= b1 && i < s.bytesEval.data.size(); ++i)
        hex += hex2(s.bytesEval.data[i]);
    return hex4(b0) + "\xE2\x80\x93" + hex4(b1) + " (" + hex + ")";
}

// The record cell under the selection, with raw bytes and rule text.
std::string recordCellDetail(const EditorState& s)
{
    if (!s.recordsHas || !s.recFmt || s.recordsEval.records.empty())
        return {};
    const std::size_t rec = std::min(s.selRecord, s.recordsEval.records.size() - 1);
    const Record& r = s.recordsEval.records[rec];
    if (s.selField >= r.cells.size())
        return {};
    const FieldSpec& fs = s.recFmt->fields[0];
    (void)fs;
    std::ostringstream d;
    d << "#" << rec << " @" << r.offset << " · " << s.recFmt->columns[s.selField] << "\n";
    d << "value " << r.cells[s.selField].text << "\n";
    d << "bits " << binlab::binGrouped(r.cells[s.selField].bits, 64);
    if (r.rule >= 0)
        d << "\nrule " << s.recFmt->rules[static_cast<std::size_t>(r.rule)].name;
    if (!r.ruleError.empty())
        d << "\nrule error: " << r.ruleError;
    return d.str();
}

std::string bytesStatusLeft(const EditorState& s)
{
    if (s.bytesIsRecords) {
        if (!s.recordsHas || !s.recFmt)
            return s.zh ? "记录格式 · 在表达式行输入 fmt <名称>" : "records · type fmt <name>";
        std::string left = s.recFmt->name + " · " + (s.recordsEval.msbFirst ? "msb-first" : "lsb-first")
                         + " · " + std::to_string(s.recordsEval.records.size())
                         + (s.zh ? " 条 · 尾 " : " records · trailing ")
                         + std::to_string(s.recordsEval.trailing);
        if (!s.recFmt->rules.empty())
            left += (s.zh ? " · 命中 " : " · hits ") + std::to_string(s.recordsEval.ruleHits)
                  + (s.zh ? " 错 " : " err ") + std::to_string(s.recordsEval.ruleErrors);
        if (!s.recordsEval.records.empty())
            left += " | #" + std::to_string(s.selRecord) + " "
                  + s.recFmt->columns[std::min(s.selField, s.recFmt->columns.size() - 1)];
        return left;
    }
    if (!s.scalarHas)
        return s.zh ? "字节流 · 例：hex u16le 34 12 78 56" : "bytes · e.g. hex u16le 34 12 78 56";
    std::string left = s.bytesType.name + " · " + std::to_string(s.bytesEval.data.size())
                     + (s.zh ? " 字节 · 尾 " : " bytes · trailing ") + std::to_string(s.bytesEval.trailing);
    const std::string sel = selectionSummary(s);
    if (!sel.empty())
        left += " | " + sel;
    return left;
}

// ---- bytes painting -----------------------------------------------------------

void paintBytesToolbar(HDC dc)
{
    EditorState& s = state();
    HFONT toolFont = uiFont(12, false, s.zh);
    SelectObject(dc, toolFont);
    static const int kWidths[4] = { 8, 16, 32, 64 };
    if (!s.bytesIsRecords) {
        drawLeftText(dc, s.rcToolLabel[0], wideFromUtf8(s.zh ? "位宽" : "Width"), kLabelText);
        for (int i = 0; i < 4; ++i)
            drawSegment(dc, s.rcWidthBtn[i], std::to_wstring(kWidths[i]), s.bytesType.size * 8 == kWidths[i]);
        drawLeftText(dc, s.rcToolLabel[1], wideFromUtf8(s.zh ? "类型" : "Type"), kLabelText);
        const int kind = s.bytesType.flt ? 2 : (s.bytesType.signedType ? 1 : 0);
        for (int i = 0; i < 3; ++i)
            drawSegment(dc, s.rcKindBtn[i], wideFromUtf8(kindName(i, s.zh)), kind == i);
        drawLeftText(dc, s.rcToolLabel[2], L"LE/BE", kLabelText);
        drawSegment(dc, s.rcEndian[0], L"LE", s.endian != "be");
        drawSegment(dc, s.rcEndian[1], L"BE", s.endian == "be");
    } else {
        drawLeftText(dc, s.rcToolLabel[3], wideFromUtf8(s.zh ? "格式" : "Format"), kLabelText);
        const RECT b = s.rcFormatBtn;
        HBRUSH fill = CreateSolidBrush(s.formatOpen ? kAccent : kOverlay);
        FillRect(dc, &b, fill);
        DeleteObject(fill);
        frameRect(dc, b, s.formatOpen ? kAccent : RGB(80, 90, 102));
        HFONT mono = monoFont(12, false);
        SelectObject(dc, mono);
        const std::string name = s.recFmt ? s.recFmt->name : (s.zh ? "选择格式 ▾" : "choose ▾");
        RECT t { b.left + 8, b.top, b.right - 24, b.bottom };
        drawLeftText(dc, t, wideFromUtf8(name), s.formatOpen ? kBitOnText : kPanelText);
        drawCenteredText(dc, RECT { b.right - 22, b.top, b.right - 4, b.bottom }, L"\u25BE", kPanelText);
        DeleteObject(mono);
        SelectObject(dc, toolFont);
        drawLeftText(dc, s.rcToolLabel[4], wideFromUtf8(s.zh ? "记录序" : "Order"), kLabelText);
        const bool msb = s.recordsHas ? s.recordsEval.msbFirst : true;
        drawSegment(dc, s.rcOrder[0], L"LSB", !msb);
        drawSegment(dc, s.rcOrder[1], L"MSB", msb);
    }
    DeleteObject(toolFont);
}

// One chip: a label and a value in one flat surface, like the bits page.
void drawBytesChip(HDC dc, const RECT& r, const std::string& label, const std::string& value, bool enabled)
{
    HBRUSH fill = CreateSolidBrush(kBitOff);
    FillRect(dc, &r, fill);
    DeleteObject(fill);
    frameRect(dc, r, kSeparator);
    HFONT lf = uiFont(11, false, state().zh);
    SelectObject(dc, lf);
    RECT lr { r.left + 8, r.top, r.right - 8, r.bottom };
    drawLeftText(dc, lr, wideFromUtf8(label), kLabelText);
    const int lw = textWidth(dc, lf, wideFromUtf8(label));
    HFONT vf = monoFont(12, false);
    SelectObject(dc, vf);
    RECT vr { r.left + 8 + lw, r.top, r.right - 8, r.bottom };
    SetTextColor(dc, enabled ? kValueText : kLabelText);
    DrawTextW(dc, wideFromUtf8(value).c_str(), -1, &vr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DeleteObject(lf);
    DeleteObject(vf);
}

void paintBytesScalar(HDC dc)
{
    EditorState& s = state();
    HFONT mono = monoFont(12, false);
    HFONT monoB = monoFont(12, true);
    SelectObject(dc, mono);
    const bool has = s.scalarHas && !s.bytesEval.data.empty();
    const std::size_t b0 = std::min(s.selAnchor, s.selActive);
    const std::size_t b1 = std::max(s.selAnchor, s.selActive);
    // header: column numbers 0..F
    RECT hl { s.offW + 8, s.byteTop, s.offW + 8 + 16 * (s.byteW + 2), s.byteTop + kByteHeadH };
    SetTextColor(dc, kLabelText);
    for (int c = 0; c < 16; ++c) {
        const int x = s.offW + 8 + c * s.byteW + (c >= 8 ? kNibbleGap : 0);
        RECT t { x, s.byteTop, x + s.byteW, s.byteTop + kByteHeadH };
        drawCenteredText(dc, t, wideFromUtf8(std::string(1, "0123456789ABCDEF"[c])), kLabelText);
    }
    drawLeftText(dc, RECT { 12, s.byteTop, s.offW + 4, s.byteTop + kByteHeadH },
                 wideFromUtf8(s.zh ? "偏移" : "off"), kLabelText);
    drawLeftText(dc, RECT { s.asciiLeft, s.byteTop, s.clientW - 12, s.byteTop + kByteHeadH },
                 wideFromUtf8(s.zh ? "ASCII" : "ASCII"), kLabelText);
    (void)hl;
    for (int row = 0; row < kByteRows; ++row) {
        const std::size_t base = (s.scrollRow + static_cast<std::size_t>(row)) * 16;
        if (base >= s.bytesEval.data.size())
            break;
        const int top = s.byteTop + kByteHeadH + row * kByteRowH;
        SelectObject(dc, mono);
        RECT off { 12, top, s.offW + 4, top + kByteRowH - 2 };
        drawLeftText(dc, off, wideFromUtf8(hex4(base).substr(2)), kLabelText);
        std::string ascii;
        for (std::size_t c = 0; c < 16 && base + c < s.bytesEval.data.size(); ++c) {
            const std::size_t idx = base + c;
            const std::uint8_t bt = s.bytesEval.data[idx];
            const RECT r = byteCell(idx);
            const bool selected = idx >= b0 && idx <= b1;
            HBRUSH fill = CreateSolidBrush(selected ? RGB(52, 76, 104) : kBitOff);
            FillRect(dc, &r, fill);
            DeleteObject(fill);
            frameRect(dc, r, selected ? kFocus : kSeparator);
            SelectObject(dc, mono);
            drawCenteredText(dc, r, wideFromUtf8(hex2(bt)), selected ? kBitOnText : kBitOffText);
            ascii += (bt >= 0x20 && bt <= 0x7E) ? static_cast<char>(bt) : '.';
        }
        SelectObject(dc, monoB);
        RECT ar { s.asciiLeft + 8, top, s.asciiLeft + 8 + static_cast<int>(ascii.size()) * s.monoW + 4, top + kByteRowH - 2 };
        HBRUSH ab = CreateSolidBrush(kBitOff);
        FillRect(dc, &ar, ab);
        DeleteObject(ab);
        drawLeftText(dc, RECT { ar.left + 4, ar.top, ar.right, ar.bottom }, wideFromUtf8(ascii), kPanelText);
    }
    if (!has) {
        SelectObject(dc, uiFont(12, false, s.zh));
        RECT t { 0, s.byteTop + 40, s.clientW, s.byteTop + 64 };
        drawCenteredText(dc, t, wideFromUtf8(s.zh ? "输入 hex 字节流（deadbeefdeadbeef 也可），或 fmt <格式名> <hex...> 解码记录"
                                                 : "Enter hex bytes (deadbeefdeadbeef works too), or fmt <name> <hex...> to decode records"),
                         kLabelText);
    }
    // word readout for the active byte
    if (has) {
        const binlab::ByteWord w = wordAt(activeByte());
        static const char* kNames[3] = { "HEX", "DEC", "BIN" };
        const char* labels[3] = { "HEX ", s.bytesType.flt ? "FLOAT " : "DEC ", "BIN " };
        std::string values[3];
        if (s.bytesType.flt) {
            values[0] = w.bitsHex;
            values[1] = w.primary;
            values[2] = binlab::binPlain(std::strtoull(w.bitsHex.c_str(), nullptr, 16), w.width);
        } else {
            const binlab::Repr r = binlab::represent(std::strtoull(w.bitsHex.c_str(), nullptr, 16), w.width,
                                                     s.bytesType.signedType);
            values[0] = r.hex;
            values[1] = r.dec;
            values[2] = r.bin;
        }
        (void)kNames;
        for (int i = 0; i < 3; ++i)
            drawBytesChip(dc, s.rcByteChip[i], labels[i], values[i], !w.partial);
    } else {
        for (int i = 0; i < 3; ++i)
            drawBytesChip(dc, s.rcByteChip[i], i == 0 ? "HEX " : i == 1 ? "DEC " : "BIN ", "-", false);
    }
    drawSegment(dc, s.rcToBits, wideFromUtf8(s.zh ? "送到位运算页" : "Send to Bits"), false);
    DeleteObject(mono);
    DeleteObject(monoB);
}

void paintBytesRecords(HDC dc)
{
    EditorState& s = state();
    HFONT mono = monoFont(12, false);
    HFONT ui = uiFont(12, false, s.zh);
    const std::size_t cols = recColumns();
    const std::size_t fit = recordsFit();
    const int headerTop = s.byteTop;
    // Header: the record index plus the format's column names, offset by the
    // horizontal scroll, so the first visible header cell is always labelled.
    SelectObject(dc, mono);
    {
        int x = static_cast<int>(s.offW) + 8;
        const int y = headerTop;
        for (std::size_t c = s.firstCol; c < cols; ++c) {
            const int w = recColumnWidth(c);
            if (x >= s.clientW - 12)
                break;
            RECT r { x, y, std::min(x + w, s.clientW - 12), y + kByteHeadH };
            drawLeftText(dc, r, wideFromUtf8(s.recFmt ? s.recFmt->columns[c] : std::to_string(c)), kLabelText);
            x += w;
        }
    }
    drawLeftText(dc, RECT { 12, headerTop, static_cast<int>(s.offW) + 4, headerTop + kByteHeadH },
                 wideFromUtf8("#"), kLabelText);

    for (std::size_t row = 0; row < fit; ++row) {
        const std::size_t rec = s.scrollRow + row;
        if (rec >= s.recordsEval.records.size())
            break;
        const Record& r = s.recordsEval.records[rec];
        const int top = s.byteTop + kByteHeadH + static_cast<int>(row) * kByteRowH;
        COLORREF rowBg = kBackground;
        COLORREF rowFg = kPanelText;
        if (r.rule >= 0 && s.recFmt) {
            const RuleSpec& rs = s.recFmt->rules[static_cast<std::size_t>(r.rule)];
            if (!rs.bg.empty())
                rowBg = colorFromHex(rs.bg, kBackground);
            if (!rs.fg.empty())
                rowFg = colorFromHex(rs.fg, kPanelText);
        }
        if (!r.ruleError.empty())
            rowFg = kErrText;
        SelectObject(dc, ui);
        RECT idx { 12, top, static_cast<int>(s.offW) + 4, top + kByteRowH };
        if (rowBg != kBackground) {
            HBRUSH b = CreateSolidBrush(rowBg);
            RECT band { 8, top, s.clientW - 12, top + kByteRowH - 1 };
            FillRect(dc, &band, b);
            DeleteObject(b);
        }
        drawLeftText(dc, idx, wideFromUtf8(std::to_string(rec)), r.rule >= 0 ? rowFg : kLabelText);
        SelectObject(dc, mono);
        for (std::size_t c = s.firstCol; c < cols && c < r.cells.size(); ++c) {
            const int w = recColumnWidth(c);
            const int x = recColumnLeft(c);
            if (x >= s.clientW - 12)
                break;
            RECT cell { x + 2, top, std::min(x + w - 2, s.clientW - 12), top + kByteRowH - 1 };
            const bool selected = rec == s.selRecord && c == s.selField;
            if (selected) {
                HBRUSH b = CreateSolidBrush(RGB(52, 76, 104));
                FillRect(dc, &cell, b);
                DeleteObject(b);
                frameRect(dc, cell, kFocus);
            }
            drawLeftText(dc, RECT { cell.left + 6, cell.top, cell.right - 2, cell.bottom },
                         wideFromUtf8(r.cells[c].text), selected ? kBitOnText : rowFg);
        }
    }
    if (s.recordsEval.records.empty()) {
        SelectObject(dc, ui);
        RECT t { 0, s.byteTop + 40, s.clientW, s.byteTop + 64 };
        drawCenteredText(dc, t, wideFromUtf8(s.zh ? "粘贴一条或多条记录：fmt blm_rawdata_256 01 00 …"
                                                 : "Paste records: fmt blm_rawdata_256 01 00 …"),
                         kLabelText);
    }
    // the selected cell strip
    const std::string detail = recordCellDetail(s);
    {
        HBRUSH b = CreateSolidBrush(kBitOff);
        FillRect(dc, &s.rcCellChip, b);
        DeleteObject(b);
        frameRect(dc, s.rcCellChip, kSeparator);
        if (!detail.empty()) {
            SelectObject(dc, ui);
            std::string line;
            for (char c : detail)
                line += (c == '\n' ? ' ' : c);
            RECT t { s.rcCellChip.left + 8, s.rcCellChip.top, s.rcCellChip.right - 8, s.rcCellChip.bottom };
            drawLeftText(dc, t, wideFromUtf8(line), kPanelText);
        }
    }
    drawSegment(dc, s.rcToBits, wideFromUtf8(s.zh ? "送到位运算页" : "Send to Bits"), false);
    DeleteObject(mono);
    DeleteObject(ui);
}

void paintBytesFormatDropdown(HDC dc)
{
    EditorState& s = state();
    if (!s.formatOpen || !s.formats)
        return;
    HFONT mono = monoFont(12, false);
    HFONT ui = uiFont(11, false, s.zh);
    const std::size_t n = std::min<std::size_t>(kFormatRows, s.formats->entries.size());
    if (n == 0) {
        RECT r = s.rcFormatBtn;
        r.top = r.bottom + 2;
        r.bottom = r.top + kFormatRowH;
        HBRUSH b = CreateSolidBrush(RGB(20, 24, 30));
        FillRect(dc, &r, b);
        DeleteObject(b);
        frameRect(dc, r, RGB(90, 110, 130));
        SelectObject(dc, ui);
        drawLeftText(dc, RECT { r.left + 8, r.top, r.right - 8, r.bottom },
                     wideFromUtf8(s.zh ? "未加载任何格式" : "no formats loaded"), kLabelText);
        DeleteObject(mono);
        DeleteObject(ui);
        return;
    }
    RECT panel { s.rcFormatItem[0].left - 2, s.rcFormatItem[0].top - 2,
                 s.rcFormatItem[n - 1].right + 2, s.rcFormatItem[n - 1].bottom + 2 };
    HBRUSH pb = CreateSolidBrush(RGB(20, 24, 30));
    FillRect(dc, &panel, pb);
    DeleteObject(pb);
    frameRect(dc, panel, RGB(90, 110, 130));
    for (std::size_t i = 0; i < n; ++i) {
        const FormatEntry& e = s.formats->entries[i];
        const RECT r = s.rcFormatItem[i];
        if (e.ok && s.recFmt && e.name == s.recFmt->name) {
            HBRUSH hl = CreateSolidBrush(RGB(50, 62, 76));
            FillRect(dc, &r, hl);
            DeleteObject(hl);
        }
        SelectObject(dc, e.ok ? mono : ui);
        RECT t { r.left + 8, r.top, r.right - 8, r.bottom };
        const std::string text = e.ok ? e.name : (e.source + (s.zh ? " · 加载失败" : " · failed"));
        drawLeftText(dc, t, wideFromUtf8(text), e.ok ? kPanelText : kErrText);
    }
    DeleteObject(mono);
    DeleteObject(ui);
}

// ---- bytes actions -------------------------------------------------------------

// Rewrite the type token inside a scalar expression (prefix + data). Returns
// false when there is no type token to rewrite.
bool rewriteScalarType(const std::string& newType)
{
    const std::vector<std::string> tokens = binlab::splitWhitespace(expressionText());
    if (tokens.size() < 2 || binlab::lower(tokens[0]) == "fmt")
        return false;
    replaceExpression(tokens[0] + " " + newType + " " + joinTokens(std::vector<std::string>(tokens.begin() + 2, tokens.end())));
    return true;
}

// Rewrite the fmt order word: `fmt NAME lsb HEX...` -> `fmt NAME msb HEX...`.
void rewriteOrder(bool msb)
{
    EditorState& s = state();
    const std::vector<std::string> tokens = binlab::splitWhitespace(expressionText());
    if (tokens.size() < 2)
        return;
    std::vector<std::string> out { tokens[0], tokens[1] };
    std::size_t at = 2;
    if (at < tokens.size() && isOrderWord(tokens[at]))
        ++at;
    out.push_back(msb ? "msb" : "lsb");
    for (std::size_t i = at; i < tokens.size(); ++i)
        out.push_back(tokens[i]);
    s.suppressEval = true;
    replaceExpression(joinTokens(out));
    s.suppressEval = false;
    evaluateBytes();
    showMsg(s.hwnd, s.zh ? (msb ? "记录序：msb-first" : "记录序：lsb-first")
                         : (msb ? "record order: msb-first" : "record order: lsb-first"));
    winshun::debug(std::string("ra9fael.binarylab: setOrder ") + (msb ? "msb" : "lsb"));
}

void chooseFormat(const FormatEntry& e)
{
    EditorState& s = state();
    s.formatOpen = false;
    if (!e.ok) {
        showMsg(s.hwnd, e.error);
        return;
    }
    const std::vector<std::string> tokens = binlab::splitWhitespace(expressionText());
    std::vector<std::string> out { "fmt", e.name };
    std::size_t at = 2;
    if (tokens.size() > 2 && isOrderWord(tokens[2]))
        at = 3;
    if (tokens.size() > at) {
        bool msb = s.recordsHas ? s.recordsEval.msbFirst : true;
        for (std::size_t i = 2; i < tokens.size(); ++i)
            if (isOrderWord(tokens[i])) {
                msb = binlab::lower(tokens[i]).rfind("msb", 0) == 0;
                break;
            }
        out.push_back(msb ? "msb" : "lsb");
        for (std::size_t i = at; i < tokens.size(); ++i)
            out.push_back(tokens[i]);
    }
    replaceExpression(joinTokens(out));
    winshun::debug("ra9fael.binarylab: setFormat " + e.name);
}

// Scalar toolbar: width / signedness / float kind / endian. Each rewrites the
// type word and re-derives the whole page from the same bytes.
void setScalarType(HWND hwnd, int width, int kind, bool endianBe)
{
    EditorState& s = state();
    const std::string oldEndian = s.endian;
    if (endianBe != (s.endian == "be"))
        s.endian = endianBe ? "be" : "le";
    if (kind >= 0 && width > 0) {
        std::string name = kind == 2 ? (width == 16 ? "f16" : width == 32 ? "f32" : "f64")
                                     : binlab::typeNameFor(width, kind == 1);
        rewriteScalarType(name + s.endian);
        showMsg(hwnd, s.zh ? "类型改写为 " + name + " " + s.endian : "type rewritten to " + name + " " + s.endian);
        return;
    }
    if (oldEndian != s.endian) {
        // The endian lives in the expression's type suffix, so the rewrite
        // keeps the line and the readout in agreement (u16le -> u16be).
        const std::vector<std::string> tokens = binlab::splitWhitespace(expressionText());
        bool signedTmp = false;
        int widthTmp = 0;
        if (tokens.size() >= 2 && !binlab::isBitsType(tokens[1], widthTmp, signedTmp)) {
            // tokens[1] is not a plain int name (float or suffixed): rewrite it.
            std::string base = tokens[1];
            if (base.size() > 2 && (base.compare(base.size() - 2, 2, "le") == 0
                                    || base.compare(base.size() - 2, 2, "be") == 0))
                base = base.substr(0, base.size() - 2);
            rewriteScalarType(base + s.endian);
        }
        winshun::debug(std::string("ra9fael.binarylab: setBytesEndian ") + s.endian);
        evaluateBytes();
        showMsg(hwnd, s.zh ? "端序：" + std::string(s.endian == "be" ? "BE" : "LE")
                           : "endian: " + std::string(s.endian == "be" ? "BE" : "LE"));
        return;
    }
    evaluateBytes();
}

// "送到位运算页": carry the active word into the bits page as `u<width> 0x…`.
void sendToBits(HWND hwnd)
{
    EditorState& s = state();
    if (s.bytesIsRecords) {
        if (!s.recordsHas || !s.recFmt || s.recordsEval.records.empty() || s.selField >= recColumns())
            return;
        const Record& r = s.recordsEval.records[std::min(s.selRecord, s.recordsEval.records.size() - 1)];
        const Cell& c = r.cells[s.selField];
        std::string expr;
        if (c.kind == Cell::Float)
            expr = "f64 " + binlab::hexUpper(c.bits, 64);
        else if (c.kind == Cell::Signed)
            expr = "u64 " + binlab::hexUpper(c.bits, 64);
        else
            expr = "u64 " + binlab::hexUpper(c.u, 64);
        replaceExpression(expr);
        evaluate();
        s.mode = EditorSpec::Mode::Bits;
        s.editOwner = EditorSpec::Mode::Bits;
        relayout(hwnd);
        winshun::debug("ra9fael.binarylab: sendToBits from records: " + expr);
        return;
    }
    if (!s.scalarHas || s.bytesEval.data.empty())
        return;
    const binlab::ByteWord w = wordAt(activeByte());
    const std::string expr = (s.bytesType.flt ? std::string(s.bytesType.flt->name) : binlab::typeNameFor(w.width, false))
                           + " 0x" + w.bitsHex.substr(2);
    replaceExpression(expr);
    evaluate();
    s.mode = EditorSpec::Mode::Bits;
    s.editOwner = EditorSpec::Mode::Bits;
    relayout(hwnd);
    winshun::debug("ra9fael.binarylab: sendToBits: " + expr);
}

void bytesClick(HWND hwnd, int x, int y)
{
    EditorState& s = state();
    // toolbar
    if (s.bytesIsRecords) {
        for (int i = 0; i < 2; ++i) {
            if (inRect(s.rcOrder[i], x, y)) {
                rewriteOrder(i == 1);
                return;
            }
        }
        if (inRect(s.rcFormatBtn, x, y)) {
            s.formatOpen = !s.formatOpen;
            InvalidateRect(hwnd, nullptr, TRUE);
            return;
        }
        for (std::size_t i = 0; i < kFormatRows && s.formats && i < s.formats->entries.size(); ++i) {
            if (inRect(s.rcFormatItem[i], x, y)) {
                chooseFormat(s.formats->entries[i]);
                return;
            }
        }
        std::size_t rec = 0, col = 0;
        if (hitRecordCell(x, y, rec, col)) {
            s.selRecord = rec;
            s.selField = col;
            logLayout(); // the drive scripts read selection state from this line
            InvalidateRect(hwnd, nullptr, TRUE);
            return;
        }
        if (inRect(s.rcToBits, x, y)) {
            sendToBits(hwnd);
            return;
        }
        if (s.formatOpen) {
            s.formatOpen = false;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return;
    }
    static const int kWidths[4] = { 8, 16, 32, 64 };
    for (int i = 0; i < 4; ++i) {
        if (inRect(s.rcWidthBtn[i], x, y)) {
            setScalarType(hwnd, kWidths[i], -1, s.endian == "be");
            return;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (inRect(s.rcKindBtn[i], x, y)) {
            const int w = i == 2 ? (binlab::floatByBits(s.bytesType.size * 8)
                                       ? s.bytesType.size * 8
                                       : 32)
                                 : s.bytesType.size * 8;
            if (i == 2 && binlab::floatByBits(w) == nullptr) {
                showMsg(hwnd, s.zh ? std::to_string(w) + " 位没有 IEEE-754 容器"
                                   : std::to_string(w) + "-bit has no IEEE-754 container");
                return;
            }
            setScalarType(hwnd, w, i, s.endian == "be");
            return;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (inRect(s.rcEndian[i], x, y)) {
            setScalarType(hwnd, -1, -1, i == 1);
            return;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (inRect(s.rcByteChip[i], x, y) && s.scalarHas && !s.bytesEval.data.empty()) {
            const binlab::ByteWord w = wordAt(activeByte());
            const char* names[3] = { "HEX", s.bytesType.flt ? "FLOAT" : "DEC", "BIN" };
            const std::string text = i == 0 ? w.bitsHex : i == 1 ? w.primary
                : binlab::binPlain(std::strtoull(w.bitsHex.c_str(), nullptr, 16), w.width);
            if (copyToClipboard(hwnd, text)) {
                s.msg = s.zh ? std::string("已复制 ") + names[i] : std::string("Copied ") + names[i];
                s.msgTick = GetTickCount();
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return;
        }
    }
    if (inRect(s.rcToBits, x, y)) {
        sendToBits(hwnd);
        return;
    }
    const std::size_t idx = hitByte(x, y);
    if (idx != SIZE_MAX) {
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
            s.selActive = idx; // extend
        else {
            s.selAnchor = idx;
            s.selActive = idx;
        }
        s.byteDrag = true;
        logLayout(); // the drive scripts read selection state from this line
        InvalidateRect(hwnd, nullptr, TRUE);
    }
}

// @@BYTES_NEXT@@

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
    drawLeftText(dc, t2, wideFromUtf8(s.zh ? "插件设置（只读 · 在 WinShun 中修改）"
                                           : "Plugin settings (read-only · change in WinShun)"), kPanelText);

    const int rowTop = s.panelTop + 34;
    const char* labels[6] = {
        s.zh ? "位宽" : "Width", s.zh ? "符号" : "Sign", s.zh ? "^ 语义" : "^ semantics",
        s.zh ? "端序" : "Endian", s.zh ? "语言" : "Language", s.zh ? "运算模式" : "Eval mode",
    };
    const std::string values[6] = {
        "", "", "XOR",
        std::string(s.endian == "be" ? "BE" : "LE") + " · pack "
            + (s.pack == 0 ? "natural" : std::to_string(s.pack)),
        s.zh ? "跟随宿主" : "follows host",
        "", // the fixed/C segments themselves show the mode
    };
    for (int i = 0; i < 6; ++i) {
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
        drawSegment(dc, s.rcSetWidth[i], std::to_wstring(kWidths[i]), s.width == kWidths[i], s.cMode);
    drawSegment(dc, s.rcSetSign[0], L"u", !s.isSigned, s.cMode);
    drawSegment(dc, s.rcSetSign[1], L"i", s.isSigned, s.cMode);
    drawSegment(dc, s.rcSetMode[0], wideFromUtf8(s.zh ? "固定宽度" : "fixed"), !s.cMode);
    drawSegment(dc, s.rcSetMode[1], L"C", s.cMode);

    const char* hostLabels[5] = { "defaultWidth", "defaultEndian", "defaultPack", "bitsEnter",
                                  "evalMode" };
    const std::string hostValues[5] = {
        std::to_string(s.hostWidth), s.hostEndian,
        s.hostPack == 0 ? "0 (natural)" : std::to_string(s.hostPack), s.hostBitsEnter,
        s.hostEvalMode,
    };
    for (int i = 0; i < 5; ++i) {
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
    drawSegment(dc, s.rcClearHistory, wideFromUtf8(s.zh ? "清空历史" : "Clear history"), false);
    const int histY = rowTop + 5 * kPanelRowH;
    RECT hs { mid + 140, histY, right, histY + kPanelRowH - 2 };
    const std::string histState = (s.zh ? "历史 " : "History ")
        + std::to_string(s.history.size()) + "/" + std::to_string(kMaxHistory)
        + (s.historyPersistent ? (s.zh ? " · 落盘" : " · saved")
                               : (s.zh ? " · 会话" : " · session"));
    drawLeftText(dc, hs, wideFromUtf8(histState), kLabelText);
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

    const auto textLine = [&](const RECT& r, const std::string& text, COLORREF color) {
        SetTextColor(dc, color);
        DrawTextW(dc, wideFromUtf8(text).c_str(), -1, const_cast<LPRECT>(&r),
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    };

    // The content area is kPanelContentH=200 tall starting at panelTop=42.
    // Everything below must fit inside +192 (the status bar starts there).
    RECT t1 { left, s.panelTop, mid - 12, s.panelTop + 18 };
    RECT t2 { mid, s.panelTop, right, s.panelTop + 18 };
    drawLeftText(dc, t1, wideFromUtf8(s.zh ? "语法" : "Syntax"), kPanelText);
    drawLeftText(dc, t2, wideFromUtf8(s.zh ? "鼠标与键盘" : "Mouse and keys"), kPanelText);

    const char* l[4] = {
        s.zh ? "0x/0b/0o · & | ^ ~ << >> ( ) + - * / % **" : "0x/0b/0o · & | ^ ~ << >> ( ) + - * / % **",
        s.zh ? "rol/ror · @ · 7:4 · get/set · 运算模式设置页可切" : "rol/ror · @ · 7:4 · get/set · eval mode in settings",
        s.zh ? "u8..u64/i8.. · f16 f32 f64 · 1.5 .5 1e6 Inf NaN" : "u8..u64/i8.. · f16 f32 f64 · 1.5 .5 1e6 Inf NaN",
        s.zh ? "单位 k K M G T · Ki Mi Gi Ti · KB/MB/GB" : "Units k K M G T · Ki Mi Gi Ti · KB/MB/GB",
    };
    const char* r[4] = {
        s.zh ? "点击位格翻转 · 拖动涂抹" : "Click toggles · drag paints",
        s.zh ? "点击 chips / 校验 / 条带字段复制" : "Click chips / parity / field to copy",
        s.zh ? "Enter 提交并记历史 · ↑↓ 翻阅 · ▾ 下拉" : "Enter submits · Up/Down browse · ▾ dropdown",
        s.zh ? "方向键 · 空格 · Ctrl+C/B/D/O · Tab · Esc" : "Arrows · Space · Ctrl+C/B/D/O · Tab · Esc",
    };
    SelectObject(dc, font);
    for (int i = 0; i < 4; ++i) {
        const int y = s.panelTop + 20 + i * 17;
        RECT lr { left, y, mid - 12, y + 16 };
        RECT rr { mid, y, right, y + 16 };
        textLine(lr, l[i], kLabelText);
        textLine(rr, r[i], kLabelText);
    }

    const int sepY = s.panelTop + 90;
    HGDIOBJ pen = SelectObject(dc, CreatePen(PS_SOLID, 1, kSeparator));
    MoveToEx(dc, left, sepY, nullptr);
    LineTo(dc, right, sepY);
    DeleteObject(SelectObject(dc, pen));

    SelectObject(dc, titleFont);
    RECT t3 { left, s.panelTop + 92, right, s.panelTop + 108 };
    drawLeftText(dc, t3, wideFromUtf8(s.zh ? "校验术语 · 单位 · 浮点" : "Parity · units · floats"), kPanelText);

    const char* b[5] = {
        s.zh ? "校验：置1位数 · 奇偶性 · 偶/奇校验位 = 补成偶/奇总数"
             : "Parity: ones · odd/even · even-bit/odd-bit = appended bit",
        s.zh ? "单位 k/K/M/G/T = 10^n · Ki/Mi/Gi/Ti = 2^n · KB/MB/GB 按 1000"
             : "Units k/K/M/G/T = 10^n · Ki/Mi/Gi/Ti = 2^n · KB/MB/GB = 1000",
        s.zh ? "u = micro(10^-6)，不是 unsigned；微符号请用 u / micro / 1e-6"
             : "u = micro(10^-6), not unsigned; write u / micro / 1e-6 instead",
        s.zh ? "浮点 1.5 .5 1e6 Inf NaN -> IEEE-754 位模式；f16/f32/f64 选容器"
             : "Floats 1.5 .5 1e6 Inf NaN -> IEEE-754 bits; f16/f32/f64 pick the container",
        s.zh ? "位格色带：紫=S · 蓝=E · 绿=M；「浮点条带」展开比例条，点块复制字段"
             : "Grid band: purple=S · blue=E · green=M; the float-bar pill opens it, click copies a field",
    };
    SelectObject(dc, font);
    for (int i = 0; i < 5; ++i) {
        const int y = s.panelTop + 110 + i * 16;
        RECT br { left, y, right, y + 15 };
        textLine(br, b[i], kLabelText);
    }
    SelectObject(dc, old);
    DeleteObject(titleFont);
    DeleteObject(font);
}

// ---- float composition bar ---------------------------------------------------
// One row under the grid: a caption line plus three proportional blocks in MSB
// order (S | E | M). The row is exactly kBarH tall in every state, including the
// two that show no fields at all, so the toggle is the only thing that can
// resize the window - the same anti-jitter rule the tabs follow.
void paintFloatBar(HDC dc)
{
    EditorState& s = state();
    if (!s.floatBar || s.barTop <= 0)
        return;
    const FloatView view = floatView();
    const int segTop = s.barTop + kBarCaptionH + 2;
    const int segBottom = s.barTop + kBarH - 2;
    const RECT band { 12, segTop, s.clientW - 12, segBottom };

    HFONT capFont = uiFont(11, false, s.zh);
    SelectObject(dc, capFont);
    const bool broken = !s.evalError.empty();
    RECT cr { 12, s.barTop, s.clientW - 12, s.barTop + kBarCaptionH };
    // The row is a fixed 14 px line in a 456 px wide window, so a long caption
    // (hovered exponent, no-container hint) is clipped with an ellipsis rather
    // than wrapping into the bar or growing the row.
    SetTextColor(dc, broken ? kErrText : kLabelText);
    const std::wstring caption = wideFromUtf8(broken
                                                  ? (s.zh ? "求值失败 · 位模式是最近一次有效结果"
                                                          : "evaluation failed · pattern is the last good result")
                                                  : barCaption(view, s.hoverSeg, s.zh));
    DrawTextW(dc, caption.c_str(), -1, &cr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!view.spec || broken) {
        HBRUSH b = CreateSolidBrush(kBitOff);
        FillRect(dc, &band, b);
        DeleteObject(b);
        frameRect(dc, band, kSeparator);
        DeleteObject(capFont);
        return;
    }

    const binlab::FloatSpec& f = *view.spec;
    const binlab::FloatFields g = binlab::floatFields(s.value, f);
    HFONT segFont = monoFont(11, false);
    SelectObject(dc, segFont);
    for (int segment = 0; segment < 3; ++segment) {
        const int i = barIndexForSegment(segment);
        const RECT r = s.rcBarSeg[i];
        if (rcWidth(r) <= 0)
            continue;
        const std::uint64_t fieldValue = segment == 2 ? g.sign : segment == 1 ? g.exp : g.mant;
        const int fieldBits = segment == 2 ? 1 : segment == 1 ? f.expBits : f.mantBits;
        const char* letter = segment == 2 ? "S" : segment == 1 ? "E" : "M";
        // Full label, then the shortenings that still say something. The width
        // floors in computeLayout guarantee the bare letter always fits.
        const std::string full = std::string(letter) + std::to_string(fieldBits) + " "
                               + fieldHex(fieldValue);
        const std::string plain = std::string(letter) + " " + fieldHex(fieldValue);
        std::string text = plain;
        if (textWidth(dc, segFont, wideFromUtf8(full)) <= rcWidth(r) - 8)
            text = full;
        else if (textWidth(dc, segFont, wideFromUtf8(plain)) > rcWidth(r) - 8)
            text = letter;
        // Ghost readings start dimmed because they are hypothetical; while one
        // field is hovered the other two step back, which is the half of the
        // bar <-> grid linkage that is visible in the bar itself.
        int mix = view.ghost ? 45 : 0;
        if (s.hoverSeg >= 0 && s.hoverSeg != segment)
            mix += (100 - mix) * 35 / 100;
        const COLORREF fillC = mixColor(floatSegColorByField(segment), kBackground, mix, 100);
        HBRUSH b = CreateSolidBrush(fillC);
        FillRect(dc, &r, b);
        DeleteObject(b);
        frameRect(dc, r, kSeparator);
        drawCenteredText(dc, r, wideFromUtf8(text), mix == 0 ? RGB(18, 20, 24) : kPanelText);
        if (s.hoverSeg == segment) {
            frameRect(dc, RECT { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 }, kFocus);
            frameRect(dc, RECT { r.left + 2, r.top + 2, r.right - 2, r.bottom - 2 }, kFocus);
        }
    }
    DeleteObject(segFont);
    DeleteObject(capFont);
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
    // Float-bar toggle: the free right end of the top bar, drawn as a pill so it
    // reads as a switch rather than a sixth tab. Bits-page only: the bar row
    // itself exists nowhere else, so the pill must not either.
    if (bits) {
        const RECT r = s.rcBarToggle;
        HBRUSH pb = CreateSolidBrush(s.floatBar ? kAccent : kBitOff);
        HPEN pp = CreatePen(PS_SOLID, 1, s.floatBar ? kAccent : kSeparator);
        HGDIOBJ oldB = SelectObject(mem, pb);
        HGDIOBJ oldP = SelectObject(mem, pp);
        RoundRect(mem, r.left, r.top, r.right, r.bottom, 12, 12);
        SelectObject(mem, oldB);
        SelectObject(mem, oldP);
        DeleteObject(pb);
        DeleteObject(pp);
        SelectObject(mem, tabFont);
        drawCenteredText(mem, r, wideFromUtf8(s.zh ? "浮点条带" : "float bar"),
                         s.floatBar ? RGB(18, 20, 24) : kLabelText);
    }

    // ---- expression row ----
    if (bits || s.mode == EditorSpec::Mode::Bytes) {
        SelectObject(mem, exprLabelFont);
        RECT lab { 12, kTopH, 62, kTopH + kExprH };
        drawLeftText(mem, lab, wideFromUtf8(s.zh ? "表达式" : "Expr"), kLabelText);
        RECT border { s.rcExpr.left - 3, s.rcExpr.top - 3, s.rcExpr.right + 3, s.rcExpr.bottom + 3 };
        frameRect(mem, border, kSeparator);
        if (bits) {
            RECT hb = s.rcHistoryBtn;
            HBRUSH hbg = CreateSolidBrush(s.historyOpen ? kAccent : kOverlay);
            FillRect(mem, &hb, hbg);
            DeleteObject(hbg);
            frameRect(mem, hb, s.historyOpen ? kAccent : RGB(80, 90, 102));
            drawCenteredText(mem, hb, L"\u25BE", s.historyOpen ? kBitOnText : kPanelText);
        }
    }

    // ---- result chips ----
    if (bits) {
        const binlab::Repr r = binlab::represent(s.value, s.width, s.isSigned);
        const char* labels[5] = { "HEX ", "OCT ", "BIN ",
                                  s.isFloat ? "FLOAT " : "DEC ", s.zh ? "全部" : "All" };
        const std::string values[5] = { r.hex, r.oct, r.bin,
                                        s.isFloat ? s.floatText : r.dec, "" };
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
                // Long exact float text is ellipsised rather than overflowing
                // the chip; copying still returns the full value.
                SetTextColor(mem, kValueText);
                DrawTextW(mem, wideFromUtf8(values[i]).c_str(), -1, &vr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
    }

    // ---- width / signedness toolbar ----
    if (bits) {
        HFONT toolFont = uiFont(12, false, s.zh);
        SelectObject(mem, toolFont);
        RECT wl { kGridMargin, s.toolTop, s.rcWidthBtn[0].left, s.toolTop + kToolH };
        drawLeftText(mem, wl, wideFromUtf8(s.zh ? "位宽" : "Width"), kLabelText);
        RECT sgl { s.rcWidthBtn[3].right + 20, s.toolTop, s.rcKindBtn[0].left, s.toolTop + kToolH };
        drawLeftText(mem, sgl, wideFromUtf8(kindLabel(s.zh)), kLabelText);
        static const int kWidths[4] = { 8, 16, 32, 64 };
        // In float mode the width buttons name the containers; 8 has none, so
        // it is drawn disabled rather than hidden (the row must not reflow).
        for (int i = 0; i < 4; ++i) {
            const binlab::FloatSpec* f = s.isFloat ? binlab::floatByBits(kWidths[i]) : nullptr;
            const std::wstring label = f ? wideFromUtf8(f->name) : std::to_wstring(kWidths[i]);
            drawSegment(mem, s.rcWidthBtn[i], label, s.width == kWidths[i],
                        s.cMode || (s.isFloat && f == nullptr));
        }
        const int kind = s.isFloat ? 2 : (s.isSigned ? 1 : 0);
        for (int i = 0; i < 3; ++i)
            drawSegment(mem, s.rcKindBtn[i], wideFromUtf8(kindName(i, s.zh)), kind == i,
                        s.cMode || (i == 2 && binlab::floatByBits(s.width) == nullptr && !s.isFloat));
        SelectObject(mem, statusFont);
        DeleteObject(toolFont);
    }

    // ---- content area ----
    const FloatView view = floatView();
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
                COLORREF cellBg = on ? kBitOn : kBitOff;
                const int bitSeg = view.spec ? binlab::floatSegment(bit, *view.spec) : -1;
                if (bitSeg >= 0 && s.hoverSeg == bitSeg)
                    cellBg = mixColor(cellBg, floatSegColorByField(bitSeg), 30, 100);
                HBRUSH fill = CreateSolidBrush(cellBg);
                FillRect(mem, &cell, fill);
                DeleteObject(fill);
                if (view.spec) {
                    // The 2px band is the always-on half of the float story: the
                    // bar explains it, but the field boundary is never hidden.
                    RECT band { cell.left, cell.top, cell.right, cell.top + 2 };
                    COLORREF c = floatSegColor(bit, *view.spec);
                    if (view.ghost)
                        c = mixColor(c, kBackground, 45, 100);
                    HBRUSH sb = CreateSolidBrush(c);
                    FillRect(mem, &band, sb);
                    DeleteObject(sb);
                }
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
    } else if (s.mode == EditorSpec::Mode::Bytes) {
        paintBytesToolbar(mem);
        measureColumns(mem);
        if (s.bytesIsRecords)
            paintBytesRecords(mem);
        else
            paintBytesScalar(mem);
        paintBytesFormatDropdown(mem);
    } else {
        SelectObject(mem, statusFont);
        const std::string line1 = (s.zh ? "结构体交互界面随 v0.3c 提供" : "The interactive struct viewer arrives in v0.3c");
        const std::string line2 = s.zh ? "当前可继续使用查询行的 bytes / struct 命令"
                                       : "The bytes / struct query keywords stay available";
        RECT l1 { 0, s.gridTop + 20, w, s.gridTop + 44 };
        RECT l2 { 0, s.gridTop + 48, w, s.gridTop + 72 };
        drawCenteredText(mem, l1, wideFromUtf8(line1), kPanelText);
        drawCenteredText(mem, l2, wideFromUtf8(line2), kLabelText);
    }

    // ---- float composition bar ----
    if (bits)
        paintFloatBar(mem);

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
        if (s.isFloat) {
            // Named from the container table, so a new row there is spoken for
            // here without touching this line.
            const binlab::FloatSpec* f = binlab::floatByBits(s.width);
            left = f ? std::string(f->name) + " · IEEE-754 " + f->ieee + " · " + s.floatText
                     : std::to_string(s.width) + " bit float · " + s.floatText;
        } else if (s.cMode)
            left = "C · " + binlab::typeNameFor(s.width, s.isSigned) + " · "
                 + std::to_string(s.width) + " bit · "
                 + (s.isSigned ? (s.zh ? "有符号" : "signed") : (s.zh ? "无符号" : "unsigned"));
        else
            left = binlab::typeNameFor(s.width, s.isSigned) + " · " + std::to_string(s.width) + " bit · "
                 + (s.isSigned ? (s.zh ? "有符号" : "signed") : (s.zh ? "无符号" : "unsigned"));
    } else if (s.mode == EditorSpec::Mode::Bytes) {
        left = bytesStatusLeft(s);
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
    const bool freshMsg = !s.msg.empty() && GetTickCount() - s.msgTick < kMsgFadeMs;
    // A fresh message gets the width it measures (up to half the row), so a
    // long hint is never clipped at its left edge; the context text yields.
    int rightW = 188;
    if (freshMsg)
        rightW = std::min(w / 2, std::max(rightW, textWidth(mem, statusFont, wideFromUtf8(s.msg)) + 12));
    RECT sl { 12, statusTop, w - 12 - rightW, statusTop + kStatusRowH };
    SetTextColor(mem, kLabelText);
    DrawTextW(mem, wideFromUtf8(left).c_str(), -1, &sl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    std::string right1;
    if (freshMsg)
        right1 = s.msg;
    else if (bits && !s.unitNote.empty())
        right1 = compactNote(s.unitNote); // "4Mi -> 4194304"
    else if (bits && s.focusBit >= 0)
        right1 = (s.zh ? "焦点 位 " : "focus bit ") + std::to_string(s.focusBit)
               + " = " + std::to_string(getBit(s.value, s.focusBit));
    else
        right1 = "GDI v" BINARYLAB_VERSION;
    RECT sr1 { w - 12 - rightW, statusTop, w - 12, statusTop + kStatusRowH };
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
            ? (s.bytesIsRecords ? (s.zh ? "点击格子选中 · 滚轮翻页 · Shift+←→ 横向滚动" : "click selects · wheel pages · Shift+arrows scroll")
                                : (s.zh ? "拖动选字节 · 点芯片复制 · 送到位运算页做位级分析" : "drag selects bytes · chips copy · Send to Bits hands off"))
            : s.mode == EditorSpec::Mode::Struct
                ? (s.zh ? "结构体交互随 v0.3c 提供" : "Interactive struct viewer arrives in v0.3c")
                : s.mode == EditorSpec::Mode::Settings
                    ? (s.zh ? "左列改本次会话，右列为宿主设置快照（只读）" : "Left: this session. Right: read-only host settings")
                    : (s.zh ? "校验术语见上方，状态栏 chip 可点击复制" : "See parity terms above; status chips are clickable");
        RECT hr { 12, row2, w - 12, h };
        drawLeftText(mem, hr, wideFromUtf8(hint), kLabelText);
    }

    // ---- expression history dropdown (drawn last, above the content) ----
    if (bits && s.historyOpen) {
        SelectObject(mem, statusFont);
        // Darker than the chips underneath and framed in a brighter line, so
        // the covered area reads as one panel instead of mixed-up text.
        const auto panelSurface = [&](const RECT& r) {
            HBRUSH pb = CreateSolidBrush(RGB(20, 24, 30));
            FillRect(mem, &r, pb);
            DeleteObject(pb);
            frameRect(mem, r, RGB(90, 110, 130));
        };
        const int n = std::min<int>(kHistRows, static_cast<int>(s.history.size()));
        if (n == 0) {
            RECT r = s.rcHistoryItem[0];
            r.bottom = r.top + kHistRowH;
            panelSurface(r);
            RECT t { r.left + 8, r.top, r.right - 8, r.bottom };
            drawLeftText(mem, t,
                         wideFromUtf8(s.zh ? "暂无历史 · 在表达式行按 Enter 提交"
                                           : "No history yet · press Enter on an expression"),
                         kLabelText);
        } else {
            RECT panel { s.rcHistoryItem[0].left, s.rcHistoryItem[0].top - 2,
                         s.rcHistoryItem[0].right, s.rcHistoryItem[n - 1].bottom + 2 };
            panelSurface(panel);
            for (int i = 0; i < n; ++i) {
                const int idx = static_cast<int>(s.history.size()) - 1 - i;
                const HistoryEntry& e = s.history[static_cast<std::size_t>(idx)];
                RECT r = s.rcHistoryItem[i];
                if (idx == s.historyPos) {
                    HBRUSH hl = CreateSolidBrush(RGB(50, 62, 76));
                    FillRect(mem, &r, hl);
                    DeleteObject(hl);
                }
                RECT lr { r.left + 8, r.top, r.right - 150, r.bottom };
                RECT rr { r.right - 150, r.top, r.right - 8, r.bottom };
                SetTextColor(mem, kPanelText);
                DrawTextW(mem, wideFromUtf8(e.expr).c_str(), -1, &lr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                drawLeftText(mem, rr, wideFromUtf8(e.result + " · " + e.type), kLabelText);
            }
        }
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
    for (int i = 0; i < 2; ++i) {
        if (inRect(s.rcSetMode[i], x, y)) {
            const bool c = i == 1;
            if (c == s.cMode)
                return;
            s.cMode = c;
            s.msg = s.cMode ? (s.zh ? "运算模式：C 升格" : "eval mode: C promotions")
                            : (s.zh ? "运算模式：固定宽度" : "eval mode: fixed width");
            s.msgTick = GetTickCount();
            evaluate();
            InvalidateRect(hwnd, nullptr, TRUE);
            winshun::debug(std::string("ra9fael.binarylab: setEvalMode ")
                           + (c ? "c" : "fixed"));
            return;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (inRect(s.rcSetWidth[i], x, y)) {
            if (s.cMode) {
                s.msg = s.zh ? "C 模式类型由表达式推导" : "C mode derives the type";
                s.msgTick = GetTickCount();
                InvalidateRect(hwnd, nullptr, TRUE);
            } else {
                setType(hwnd, kWidths[i], s.isSigned);
            }
            return;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (inRect(s.rcSetSign[i], x, y)) {
            if (s.cMode) {
                s.msg = s.zh ? "C 模式类型由表达式推导" : "C mode derives the type";
                s.msgTick = GetTickCount();
                InvalidateRect(hwnd, nullptr, TRUE);
            } else {
                setType(hwnd, s.width, i == 1);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return;
        }
    }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DPICHANGED:
        state().windowIcons.update(hwnd, HIWORD(wp));
        return DefWindowProcW(hwnd, msg, wp, lp);
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
        if (HIWORD(wp) == EN_CHANGE && reinterpret_cast<HWND>(lp) == state().edit) {
            if (state().mode == EditorSpec::Mode::Bytes)
                evaluateBytes();
            else
                evaluate();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        EditorState& s = state();
        const int x = static_cast<int>(LOWORD(lp));
        const int y = static_cast<int>(HIWORD(lp));
        SetFocus(hwnd); // keyboard returns to the shell unless the click hits a control
        if (s.mode == EditorSpec::Mode::Bits && s.historyOpen) {
            const int n = std::min<int>(kHistRows, static_cast<int>(s.history.size()));
            winshun::debug("ra9fael.binarylab: histClick x=" + std::to_string(x)
                           + " y=" + std::to_string(y) + " n=" + std::to_string(n)
                           + " row0=" + rectStr(s.rcHistoryItem[0]));
            for (int i = 0; i < n; ++i) {
                if (!inRect(s.rcHistoryItem[i], x, y))
                    continue;
                const int idx = static_cast<int>(s.history.size()) - 1 - i;
                SetWindowTextW(s.edit,
                               wideFromUtf8(s.history[static_cast<std::size_t>(idx)].expr).c_str());
                s.historyOpen = false;
                s.historyPos = -1;
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            s.historyOpen = false;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        for (int i = 0; i < 5; ++i) {
            if (inRect(s.rcTabs[i], x, y)) {
                const EditorSpec::Mode next = static_cast<EditorSpec::Mode>(i);
                if (next != s.mode) {
                    // The two content modes share one EDIT control; each keeps
                    // its own text across the switch.
                    if (s.editOwner == EditorSpec::Mode::Bits)
                        s.bitsExpr = expressionText();
                    else if (s.editOwner == EditorSpec::Mode::Bytes)
                        s.bytesExpr = expressionText();
                    s.mode = next;
                    if (next == EditorSpec::Mode::Bits || next == EditorSpec::Mode::Bytes) {
                        s.editOwner = next;
                        s.suppressEval = true;
                        SetWindowTextW(s.edit, wideFromUtf8(next == EditorSpec::Mode::Bits ? s.bitsExpr : s.bytesExpr).c_str());
                        s.suppressEval = false;
                        if (next == EditorSpec::Mode::Bytes)
                            evaluateBytes();
                        else
                            evaluate();
                    }
                }
                s.focusBit = -1;
                s.dragging = false;
                s.byteDrag = false;
                s.hoverParity = -1;
                s.hoverSeg = -1;
                s.historyOpen = false;
                s.formatOpen = false;
                relayout(hwnd);
                return 0;
            }
        }
        if (s.mode == EditorSpec::Mode::Bits && inRect(s.rcBarToggle, x, y)) {
            s.floatBar = !s.floatBar;
            s.hoverSeg = -1;
            winshun::debug(std::string("ra9fael.binarylab: setFloatBar ") + (s.floatBar ? "on" : "off"));
            relayout(hwnd); // the bar row is the only height this can change
            return 0;
        }
        if (s.mode == EditorSpec::Mode::Settings) {
            if (inRect(s.rcClearHistory, x, y)) {
                clearHistory();
                s.msg = s.zh ? "历史已清空" : "history cleared";
                s.msgTick = GetTickCount();
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            handleSettingsContentClick(hwnd, x, y);
            return 0;
        }
        if (s.mode == EditorSpec::Mode::Help)
            return 0;
        if (s.mode == EditorSpec::Mode::Bytes) {
            bytesClick(hwnd, x, y);
            return 0;
        }
        if (s.mode == EditorSpec::Mode::Bits) {
            if (inRect(s.rcHistoryBtn, x, y)) {
                s.historyOpen = !s.historyOpen;
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
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
                    if (s.cMode) {
                        s.msg = s.zh ? "C 模式类型由表达式推导" : "C mode derives the type";
                        s.msgTick = GetTickCount();
                        InvalidateRect(hwnd, nullptr, TRUE);
                    } else {
                        setType(hwnd, kWidths[i], s.isSigned);
                    }
                    return 0;
                }
            }
            for (int i = 0; i < 3; ++i) {
                if (inRect(s.rcKindBtn[i], x, y)) {
                    if (s.cMode) {
                        s.msg = s.zh ? "C 模式类型由表达式推导" : "C mode derives the type";
                        s.msgTick = GetTickCount();
                        InvalidateRect(hwnd, nullptr, TRUE);
                    } else {
                        setKind(hwnd, i);
                    }
                    return 0;
                }
            }
            for (int segment = 0; segment < 3; ++segment) {
                if (inRect(s.rcBarSeg[barIndexForSegment(segment)], x, y)) {
                    copyBarSegment(hwnd, segment);
                    return 0;
                }
            }
            for (int i = 0; i < 5; ++i) {
                if (inRect(s.rcChip[i], x, y)) {
                    static const char* const kNames[5] = { "HEX", "OCT", "BIN", "DEC", "ALL" };
                    const char* label = (i == 3 && s.isFloat) ? "FLOAT" : kNames[i];
                    copyBase(hwnd, i == 4 ? 'A' : "HOBD"[i], label);
                    return 0;
                }
            }
            const int bit = hitBit(x, y);
            if (bit >= 0) {
                s.focusBit = bit;
                s.dragPaint = getBit(s.value, bit) ? 0 : 1;
                s.dragging = true;
                setBit(s.value, s.width, bit, s.dragPaint);
                refreshFloatReadout();
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
            if (bit >= 0) {
                setBit(s.value, s.width, bit, s.dragPaint);
                refreshFloatReadout();
            }
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        if (s.mode == EditorSpec::Mode::Bytes && s.byteDrag && !s.bytesIsRecords && s.scalarHas) {
            const std::size_t idx = hitByte(x, y);
            if (idx != SIZE_MAX && idx != s.selActive) {
                s.selActive = idx;
                logLayout(); // keep the drive scripts on the newest selection
                InvalidateRect(hwnd, nullptr, TRUE);
            }
        }
        int hover = -1;
        int seg = -1;
        if (s.mode == EditorSpec::Mode::Bits && s.evalError.empty()) {
            for (int i = 0; i < 4; ++i)
                if (inRect(s.rcParity[i], x, y))
                    hover = i;
            // Bar and grid share one hovered field: pointing at a block names
            // the bits it owns, pointing at a bit names the block it belongs to.
            const FloatView view = floatView();
            for (int segment = 0; segment < 3; ++segment)
                if (view.spec && inRect(s.rcBarSeg[barIndexForSegment(segment)], x, y))
                    seg = segment;
            const int bit = hitBit(x, y);
            if (seg < 0 && bit >= 0 && view.spec)
                seg = binlab::floatSegment(bit, *view.spec);
        }
        if (hover != s.hoverParity || seg != s.hoverSeg) {
            s.hoverParity = hover;
            s.hoverSeg = seg;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        EditorState& s = state();
        if (s.mode != EditorSpec::Mode::Bytes)
            return 0;
        const int delta = static_cast<short>(HIWORD(wp));
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0 && s.bytesIsRecords && !s.recordsEval.records.empty()) {
            // horizontal scroll: one column per notch, clamped to the last column that shows
            std::size_t fc = s.firstCol;
            if (delta < 0 && s.firstCol + 1 < recColumns())
                fc = s.firstCol + 1;
            else if (delta > 0)
                fc = s.firstCol > 0 ? s.firstCol - 1 : 0;
            s.firstCol = fc;
        } else {
            const std::size_t step = delta < 0 ? 1 : (s.scrollRow > 0 ? 1 : 0);
            if (delta > 0)
                s.scrollRow = s.scrollRow > 0 ? s.scrollRow - 1 : 0;
            else if (delta < 0)
                s.scrollRow = std::min(s.scrollRow + 1, maxScrollRow());
            (void)step;
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_LBUTTONUP:
        state().dragging = false;
        state().byteDrag = false;
        return 0;
    case WM_KEYDOWN: {
        EditorState& s = state();
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wp == VK_ESCAPE) {
            if (s.historyOpen || s.formatOpen) {
                s.historyOpen = false;
                s.formatOpen = false;
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        if (wp == VK_TAB) {
            if (s.edit)
                SetFocus(s.edit);
            return 0;
        }
        if (ctrl) {
            if (s.mode == EditorSpec::Mode::Bytes) {
                if ((wp == 'C' || wp == 'c') && s.scalarHas && !s.bytesEval.data.empty()) {
                    const std::string sel = selectionSummary(s);
                    if (!sel.empty() && copyToClipboard(hwnd, sel))
                        showMsg(hwnd, s.zh ? "已复制选区" : "copied selection");
                }
                return 0;
            }
            if (wp == 'C' || wp == 'c') { copyBase(hwnd, 'H', "HEX"); return 0; }
            if (wp == 'O' || wp == 'o') { copyBase(hwnd, 'O', "OCT"); return 0; }
            if (wp == 'B' || wp == 'b') { copyBase(hwnd, 'B', "BIN"); return 0; }
            if (wp == 'D' || wp == 'd') { copyBase(hwnd, 'D', s.isFloat ? "FLOAT" : "DEC"); return 0; }
            return 0;
        }
        if (wp == VK_SPACE) {
            if (s.focusBit >= 0) {
                setBit(s.value, s.width, s.focusBit, getBit(s.value, s.focusBit) ? 0 : 1);
                refreshFloatReadout();
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        if (s.mode == EditorSpec::Mode::Bytes) {
            if (!s.bytesIsRecords && s.scalarHas && !s.bytesEval.data.empty()
                && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN)) {
                const bool extend = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                std::size_t a = extend ? s.selActive : s.selActive;
                if (wp == VK_LEFT && a > 0) --a;
                else if (wp == VK_RIGHT && a + 1 < s.bytesEval.data.size()) ++a;
                else if (wp == VK_UP && a >= 16) a -= 16;
                else if (wp == VK_DOWN && a + 16 < s.bytesEval.data.size()) a += 16;
                s.selActive = a;
                if (!extend) s.selAnchor = a;
                // keep the active byte on screen
                if (a / 16 < s.scrollRow)
                    s.scrollRow = a / 16;
                if (a / 16 >= s.scrollRow + bytesFitRows())
                    s.scrollRow = a / 16 - bytesFitRows() + 1;
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
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
    s.hostEvalMode = spec.hostEvalMode;
    s.hostFloatBar = spec.hostFloatBar;
    s.cMode = spec.cMode;
    s.floatBar = spec.hostFloatBar == "on";
    s.focusBit = -1;
    s.dragging = false;
    s.hoverParity = -1;
    s.hoverSeg = -1;
    s.evalError.clear();
    s.msg.clear();
    s.isFloat = false;
    s.floatText.clear();
    s.unitNote.clear();
    s.dataDir = spec.dataDir;
    s.history.clear();
    s.historyPos = -1;
    s.historyOpen = false;
    s.historyPersistent = true;
    loadHistory();

    // bytes page state (v0.3b)
    s.formats = spec.formats;
    s.bitsExpr.clear();
    s.bytesExpr.clear();
    s.editOwner = spec.mode;
    s.scalarHas = false;
    s.recordsHas = false;
    s.bytesIsRecords = false;
    s.recFmt = nullptr;
    s.selAnchor = 0;
    s.selActive = 0;
    s.byteDrag = false;
    s.scrollRow = 0;
    s.firstCol = 0;
    s.selRecord = 0;
    s.selField = 0;
    s.formatOpen = false;

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
    const std::wstring title = spec.zh ? L"二进制工作台 · GDI" : L"Binary Lab · GDI";

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

    s.windowIcons.update(hwnd);

    // Expression input: a real EDIT control so typing, caret and selection all
    // come for free. EN_CHANGE drives live evaluation.
    s.editFont = monoFont(13, false);
    s.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             s.rcExpr.left, s.rcExpr.top, rcWidth(s.rcExpr),
                             s.rcExpr.bottom - s.rcExpr.top,
                             hwnd, reinterpret_cast<HMENU>(1), wc.hInstance, nullptr);
    SendMessageW(s.edit, WM_SETFONT, reinterpret_cast<WPARAM>(s.editFont), TRUE);
    // 8px left inset and a 4px right inset. The control height comes from the
    // measured mono glyph metrics (computeLayout), so the text is vertically
    // centred instead of leaving dead space below it.
    SendMessageW(s.edit, 0x00D3 /* EM_SETMARGINS */, 0x0003 /* EC_LEFTMARGIN | EC_RIGHTMARGIN */,
                 static_cast<LPARAM>(MAKELONG(8, 4)));
    s.origEditProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(s.edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(editProc)));
    if (!spec.expression.empty()) {
        SetWindowTextW(s.edit, wideFromUtf8(spec.expression).c_str()); // fires EN_CHANGE
    } else if (!spec.blankExpression) {
        std::wstring initial = wideFromUtf8("0x" + std::string((spec.width + 3) / 4, '0'));
        SetWindowTextW(s.edit, initial.c_str());
    }
    if (spec.mode == EditorSpec::Mode::Bytes)
        evaluateBytes();
    else
        evaluate();

    logLayout();
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    // Auto-focus the expression line so typing starts immediately; Tab moves
    // to the bit grid and back.
    if (s.edit)
        SetFocus(s.edit);
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

    s.windowIcons.clear();
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
