#include "BitsModel.h"

#include "editor.h"
#include "../winshun.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>

#include <algorithm>
#include <cstdio>

namespace binlab {

namespace {

constexpr int kMaxHistory = 200;
constexpr int kMaxHistoryLine = 2048;

QString utf8(const std::string& text)
{
    return QString::fromStdString(text);
}

// The history file's own JSON dialect, shared with the Win32 workbench: one
// {"expr","result","type"} object per line, hand-escaped. A line the old
// workbench wrote must read back here and the other way round.
std::string jsonEscape(const std::string& s)
{
    std::string out;
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) continue; // control characters cannot come from a single-line input
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

std::string fieldHex(std::uint64_t v)
{
    char b[24];
    std::snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

} // namespace

BitsModel::BitsModel(const EditorSpec& spec, QObject* parent)
    : QObject(parent)
    , m_value(spec.value)
    , m_width(spec.width > 0 && spec.width <= 64 ? spec.width : 32)
    , m_signed(spec.isSigned)
    , m_cMode(spec.cMode)
    , m_zh(spec.zh)
    , m_blank(spec.blankExpression)
    , m_floatBarOn(spec.hostFloatBar == "on")
{
    if (!spec.dataDir.empty()) {
        QString dir = utf8(spec.dataDir);
        if (!dir.endsWith('/') && !dir.endsWith('\\'))
            dir += '/';
        m_historyPath = dir + "history.jsonl";
    }
    loadHistory();
    // The workbench always evaluates in Fixed mode unless the host's evalMode
    // says C and the expression carries no type prefix (evalBits decides that
    // itself: a prefix promises an exact width and keeps Fixed semantics).
    if (!m_blank) {
        m_expression = utf8(spec.expression);
        evaluate();
    }
    refresh();
}

void BitsModel::setExpression(const QString& text)
{
    if (m_expression == text)
        return;
    m_expression = text;
    m_blank = false;
    m_histPos = -1; // typing leaves the browse position
    evaluate();
    refresh();
    emit stateChanged();
}

void BitsModel::evaluate()
{
    m_error.clear();
    m_hint.clear();
    m_isFloat = false;
    m_floatText.clear();
    m_unitNote.clear();
    if (m_expression.trimmed().isEmpty())
        return; // blank state: the value stays what the grid last made it
    const std::string input = m_expression.toStdString();
    const auto r = evalBits(input, m_width, m_signed, m_cMode ? EvalMode::C : EvalMode::Fixed);
    if (!r.success) {
        m_error = utf8(r.failure.code);
        m_hint = utf8(r.failure.message);
        return; // a good result of the last evaluation stays on the grid
    }
    const BitsEval& e = r.value;
    m_value = e.result;
    m_width = std::clamp(e.width, 1, 64);
    m_signed = e.signedType;
    m_isFloat = e.isFloat;
    m_floatText = utf8(e.floatText);
    m_unitNote = utf8(e.unitNote);
}

void BitsModel::refresh()
{
    m_ones = 0;
    for (int i = 0; i < m_width; ++i)
        m_ones += static_cast<int>((m_value >> i) & 1);
    m_typeName = m_isFloat ? utf8("f" + std::to_string(m_width))
                           : utf8(typeNameFor(m_width, m_signed));
}

QString BitsModel::unitNoteCompact() const
{
    // "4Mi = 4 x 2^20 = 4194304" -> "4Mi -> 4194304" for the narrow status slot.
    const std::string note = m_unitNote.toStdString();
    const std::size_t first = note.find(" = ");
    const std::size_t last = note.rfind(" = ");
    if (first == std::string::npos || last == first)
        return m_unitNote;
    return utf8(note.substr(0, first) + " -> " + note.substr(last + 3));
}

QString BitsModel::hexText() const
{
    return utf8(hexUpper(m_value, m_width));
}

QString BitsModel::octText() const
{
    const Repr r = represent(m_value, m_width, m_signed);
    return utf8(r.oct);
}

QString BitsModel::binText() const
{
    const Repr r = represent(m_value, m_width, m_signed);
    return utf8(r.bin);
}

QString BitsModel::binGroupedText() const
{
    return utf8(binGrouped(m_value, m_width));
}

QString BitsModel::decText() const
{
    if (m_isFloat)
        return m_floatText;
    const Repr r = represent(m_value, m_width, m_signed);
    return utf8(r.dec);
}

QString BitsModel::unsignedText() const
{
    const Repr r = represent(m_value, m_width, false);
    return utf8(r.unsignedDec);
}

QVariantList BitsModel::readouts() const
{
    QVariantList chips;
    chips.push_back(QVariantMap { { "label", "HEX" }, { "value", hexText() }, { "name", "HEX" } });
    chips.push_back(QVariantMap { { "label", "OCT" }, { "value", octText() }, { "name", "OCT" } });
    chips.push_back(QVariantMap { { "label", "BIN" }, { "value", binText() }, { "name", "BIN" } });
    if (m_isFloat) {
        chips.push_back(QVariantMap { { "label", "FLOAT" }, { "value", m_floatText }, { "name", "FLOAT" } });
        chips.push_back(QVariantMap { { "label", "DEC" }, { "value", unsignedText() }, { "name", "DEC" } });
    } else {
        chips.push_back(QVariantMap { { "label", "DEC" }, { "value", decText() }, { "name", "DEC" } });
    }
    const QString all = m_zh ? "全部" : "ALL";
    chips.push_back(QVariantMap { { "label", all }, { "value", allText() }, { "name", all }, { "all", true } });
    return chips;
}

QVariantList BitsModel::parity() const
{
    // Six chips, the Win32 status row's four parity reads merged with the
    // prototype's TZ/LZ. Each carries its one-sentence definition (the hover
    // swap) and the copy name ("已复制 ONES").
    const bool even = m_ones % 2 == 0;
    const char* const kDefZh[6] = {
        "置 1 位数：当前位宽内值为 1 的位数",
        "奇偶性：上面这个数是奇数还是偶数",
        "偶校验位：补上它，让 1 的总数为偶",
        "奇校验位：补上它，让 1 的总数为奇",
        "尾部 0 的个数：从最低位起连续的 0",
        "首部 0 的个数：从最高位起连续的 0",
    };
    const char* const kDefEn[6] = {
        "ones: count of 1 bits in the active width",
        "parity: whether that count is odd or even",
        "even-bit: append it to make the 1 count even",
        "odd-bit: append it to make the 1 count odd",
        "trailing zeros: consecutive 0 bits from the LSB",
        "leading zeros: consecutive 0 bits from the MSB",
    };
    auto def = [&](int i) { return utf8(m_zh ? kDefZh[i] : kDefEn[i]); };
    QVariantList chips;
    chips.push_back(QVariantMap { { "label", m_zh ? "置 1 位数" : "ones" },
        { "value", QString::number(m_ones) }, { "definition", def(0) }, { "name", "ONES" } });
    chips.push_back(QVariantMap { { "label", m_zh ? "奇偶性" : "parity" },
        { "value", even ? (m_zh ? "偶" : "even") : (m_zh ? "奇" : "odd") },
        { "definition", def(1) }, { "name", "PARITY" } });
    chips.push_back(QVariantMap { { "label", m_zh ? "偶校验位" : "even-bit" },
        { "value", even ? "0" : "1" }, { "definition", def(2) }, { "name", "EVEN-BIT" } });
    chips.push_back(QVariantMap { { "label", m_zh ? "奇校验位" : "odd-bit" },
        { "value", even ? "1" : "0" }, { "definition", def(3) }, { "name", "ODD-BIT" } });
    chips.push_back(QVariantMap { { "label", "TZ" },
        { "value", QString::number(trailingZeros()) }, { "definition", def(4) }, { "name", "TZ" } });
    chips.push_back(QVariantMap { { "label", "LZ" },
        { "value", QString::number(leadingZeros()) }, { "definition", def(5) }, { "name", "LZ" } });
    return chips;
}

int BitsModel::trailingZeros() const
{
    if (m_value == 0)
        return m_width;
    int n = 0;
    while (n < m_width && ((m_value >> n) & 1) == 0)
        ++n;
    return n;
}

int BitsModel::leadingZeros() const
{
    if (m_value == 0)
        return m_width;
    int n = 0;
    for (int i = m_width - 1; i >= 0 && ((m_value >> i) & 1) == 0; --i)
        ++n;
    return n;
}

int BitsModel::bitAt(int index) const
{
    if (index < 0 || index >= m_width)
        return 0;
    return static_cast<int>((m_value >> index) & 1);
}

QVariantList BitsModel::bitValues() const
{
    QVariantList bits;
    bits.reserve(64);
    for (int i = 0; i < m_width; ++i)
        bits.push_back(static_cast<int>((m_value >> i) & 1));
    return bits;
}

void BitsModel::toggleBit(int index)
{
    if (index < 0 || index >= m_width)
        return;
    m_value ^= std::uint64_t(1) << index;
    refreshFloatReadout();
    m_histPos = -1;
    refresh();
    emit stateChanged();
}

void BitsModel::dragPaint(int index, int value)
{
    if (index < 0 || index >= m_width)
        return;
    const std::uint64_t mask = std::uint64_t(1) << index;
    const std::uint64_t wanted = value ? mask : 0;
    if ((m_value & mask) == wanted)
        return;
    m_value = value ? (m_value | mask) : (m_value & ~mask);
    refreshFloatReadout();
    m_histPos = -1;
    refresh();
    emit stateChanged();
}

void BitsModel::refreshFloatReadout()
{
    if (!m_isFloat)
        return;
    const FloatSpec* f = container();
    if (!f)
        return;
    m_floatText = utf8(exactFloatText(m_value & maskFor(m_width), *f));
    m_unitNote.clear();
}

QString BitsModel::noContainerMessage(int width) const
{
    return m_zh ? QString::number(width) + " 位没有 IEEE-754 容器"
                : QString::number(width) + "-bit has no IEEE-754 container";
}

// The rewritten line, or a null QString when the line does not start with a
// type prefix. Only the token is replaced; evaluation stays suppressed so the
// sign-extended value the toolbar just computed is not re-parsed (an "i8
// @0xFF" widened to i16 would read back as 255).
QString BitsModel::rewriteTypePrefix(int width, bool signed_) const
{
    qsizetype b = 0;
    while (b < m_expression.size() && (m_expression[b] == ' ' || m_expression[b] == '\t'))
        ++b;
    if (b >= m_expression.size())
        return {};
    qsizetype e = b;
    while (e < m_expression.size() && m_expression[e] != ' ' && m_expression[e] != '\t')
        ++e;
    int pw = 0;
    bool ps = false;
    if (!isBitsType(m_expression.mid(b, e - b).toStdString(), pw, ps))
        return {};
    QString out = m_expression;
    out.replace(b, e - b, utf8(typeNameFor(width, signed_)));
    return out;
}

void BitsModel::setType(int width, bool signed_)
{
    if (width != 8 && width != 16 && width != 32 && width != 64)
        return;
    if (width == m_width && signed_ == m_signed)
        return;
    if (m_isFloat) {
        if (signed_ != m_signed) {
            setKind(signed_ ? 1 : 0);
            return;
        }
        // Between containers the *value* is what the user is looking at, so
        // it is re-encoded: the shortest round-trip text of the old pattern
        // becomes the literal under the new prefix (f32 1.5 -> f64 1.5).
        const FloatSpec* to = floatByBits(width);
        if (to == nullptr) {
            say(noContainerMessage(width));
            return;
        }
        QString text = m_floatText;
        if (text == "+Inf")
            text = "Inf";
        replaceExpression(utf8(to->name) + " " + text, true);
        say(m_zh ? "按数值重编码为 " + utf8(to->name) : "re-encoded as " + utf8(to->name));
        return;
    }
    std::uint64_t v = m_value & maskFor(m_width);
    if (m_signed && width > m_width)
        v = static_cast<std::uint64_t>(asSigned(v, m_width));
    m_value = v & maskFor(width);
    m_width = width;
    m_signed = signed_;
    if (!m_expression.trimmed().isEmpty()) {
        const QString rewritten = rewriteTypePrefix(width, signed_);
        if (!rewritten.isNull()) {
            m_expression = rewritten;
            emit expressionReplaced(rewritten);
        }
    }
    winshun::debug("ra9fael.binarylab: setType " + typeNameFor(width, signed_));
    refresh();
    emit stateChanged();
}

void BitsModel::setKind(int kind)
{
    const int current = m_isFloat ? 2 : (m_signed ? 1 : 0);
    if (kind == current)
        return;
    if (kind < 2 && !m_isFloat) {
        setType(m_width, kind == 1);
        return;
    }
    // Integer <-> float keeps the bits and changes only how they are read,
    // the same reading the float bar offers as a ghost. '@' marks a raw
    // pattern, so a signed type accepts a set top bit.
    const std::string hex = hexUpper(m_value & maskFor(m_width), m_width);
    if (kind == 2) {
        const FloatSpec* f = floatByBits(m_width);
        if (f == nullptr) {
            say(noContainerMessage(m_width));
            return;
        }
        replaceExpression(utf8(f->name) + " " + utf8(hex), true);
        say(m_zh ? "位不变 · 按 " + utf8(f->name) + " 解释" : "same bits · read as " + utf8(f->name));
        winshun::debug("ra9fael.binarylab: setKind " + std::string(f->name));
        return;
    }
    const std::string type = typeNameFor(m_width, kind == 1);
    replaceExpression(utf8(type) + (kind == 1 ? " @" : " ") + utf8(hex), true);
    say(m_zh ? "位不变 · 按 " + utf8(type) + " 解释" : "same bits · read as " + utf8(type));
    winshun::debug("ra9fael.binarylab: setKind " + type);
}

void BitsModel::replaceExpression(const QString& text, bool reEval)
{
    m_expression = text;
    m_blank = false;
    m_histPos = -1;
    if (reEval)
        evaluate();
    refresh();
    emit stateChanged();
    emit expressionReplaced(text);
}

void BitsModel::copyValue(const QString& text, const QString& name)
{
    QGuiApplication::clipboard()->setText(text);
    say(m_zh ? "已复制 " + name + " ✓" : "Copied " + name);
}

void BitsModel::clearMessage()
{
    if (m_message.isEmpty())
        return;
    m_message.clear();
    emit messageChanged();
}

void BitsModel::say(const QString& text)
{
    m_message = text;
    emit messageChanged();
}

QString BitsModel::allText() const
{
    QString out = "HEX " + hexText() + "\nOCT " + octText() + "\nBIN " + binText() + "\n";
    out += m_isFloat ? "FLOAT " + m_floatText : "DEC " + decText();
    if (!m_unitNote.isEmpty())
        out += "\nunit " + m_unitNote;
    return out;
}

QVariantMap BitsModel::fieldFor(int segment) const
{
    const FloatSpec* f = container();
    if (!f)
        return {};
    const FloatFields g = ::binlab::floatFields(m_value, *f);
    const std::uint64_t v = segment == 2 ? g.sign : segment == 1 ? g.exp : g.mant;
    return QVariantMap { { "seg", segment },
        { "key", segment == 2 ? "S" : segment == 1 ? "E" : "M" },
        { "hex", utf8(fieldHex(v)) },
        { "bits", segment == 2 ? 1 : segment == 1 ? f->expBits : f->mantBits } };
}

QVariantMap BitsModel::floatInfo() const
{
    const FloatSpec* f = container();
    if (!f)
        return {};
    return QVariantMap { { "name", utf8(f->name) }, { "ieee", utf8(f->ieee) },
        { "bits", f->bits }, { "expBits", f->expBits }, { "mantBits", f->mantBits } };
}

QVariantList BitsModel::floatFields() const
{
    // The pill row's three entries, S|E|M order like the Win32 bar.
    QVariantList list;
    if (!container() || !m_error.isEmpty())
        return list;
    for (const int seg : { 2, 1, 0 })
        list.push_back(fieldFor(seg));
    return list;
}

QVariantList BitsModel::floatSegments() const
{
    QVariantList list;
    list.reserve(std::max(0, m_width));
    const FloatSpec* f = container();
    for (int i = 0; i < m_width; ++i)
        list.push_back(f ? floatSegment(i, *f) : -1);
    return list;
}

void BitsModel::setHoverSeg(int seg)
{
    if (seg == m_hoverSeg)
        return;
    m_hoverSeg = seg;
    emit stateChanged();
}

int BitsModel::floatSegmentAt(int bit) const
{
    const FloatSpec* f = container();
    if (!f || bit < 0 || bit >= m_width)
        return -1;
    return floatSegment(bit, *f);
}

bool BitsModel::floatGhost() const
{
    const FloatSpec* f = container();
    return f != nullptr && !m_isFloat;
}

QString BitsModel::captionFor(int hoverSeg) const
{
    if (!m_error.isEmpty())
        return m_zh ? "求值失败 · 位模式是最近一次有效结果"
                    : "evaluation failed · showing the last good bits";
    const FloatSpec* f = container();
    if (!f) {
        return m_zh ? QString::number(m_width) + " 位无 IEEE-754 容器 · 改用 16/32/64 位或 "
                          + utf8(floatNameList())
                    : QString::number(m_width) + "-bit: no IEEE-754 container · use 16/32/64 or "
                          + utf8(floatNameList());
    }
    const bool ghost = !m_isFloat;
    const std::string layout = "1+" + std::to_string(f->expBits) + "+" + std::to_string(f->mantBits);
    if (hoverSeg >= 0) {
        const FloatFields g = ::binlab::floatFields(m_value, *f);
        const int lo = hoverSeg == 2 ? f->bits - 1 : f->mantBits;
        const int hi = hoverSeg == 0 ? f->mantBits - 1 : f->bits - 2;
        const char* name = hoverSeg == 2 ? "S" : hoverSeg == 1 ? "E" : "M";
        std::string out = std::string(name) + " ";
        if (hoverSeg == 2) {
            out += m_zh ? "符号 1 位 · 位 " + std::to_string(lo) + " = " + std::to_string(g.sign)
                              + (g.sign ? (m_zh ? "（负）" : " (negative)") : (m_zh ? "（正）" : " (positive)"))
                        : "sign 1 bit · bit " + std::to_string(lo) + " = " + std::to_string(g.sign)
                              + (g.sign ? " (negative)" : " (positive)");
        } else if (hoverSeg == 1) {
            const long long unbiased = static_cast<long long>(g.exp) - f->bias;
            out += m_zh ? "指数 " + std::to_string(f->expBits) + " 位 · 位 " + std::to_string(lo) + "-"
                              + std::to_string(hi) + " = " + fieldHex(g.exp)
                              + "（去偏移 " + std::to_string(unbiased) + "，偏移 " + std::to_string(f->bias) + "）"
                        : "exponent " + std::to_string(f->expBits) + " bits · bit " + std::to_string(lo) + "-"
                              + std::to_string(hi) + " = " + fieldHex(g.exp) + " (unbiased "
                              + std::to_string(unbiased) + ", bias " + std::to_string(f->bias) + ")";
        } else {
            out += m_zh ? "尾数 " + std::to_string(f->mantBits) + " 位 · 位 0-" + std::to_string(hi)
                              + " = " + fieldHex(g.mant)
                              + (g.exp == 0 ? " · 隐含前导 0" : " · 隐含前导 1")
                        : "mantissa " + std::to_string(f->mantBits) + " bits · bit 0-" + std::to_string(hi)
                              + " = " + fieldHex(g.mant)
                              + (g.exp == 0 ? " · implicit leading 0" : " · implicit leading 1");
        }
        return utf8(out);
    }
    const std::string special = floatSpecialLabel(m_value, *f);
    std::string out = ghost
        ? typeNameFor(m_width, m_signed) + " · "
              + (m_zh ? "假设按 " : "ghost-read as ") + f->name + (m_zh ? " 解释 · " : " · ")
        : std::string(f->name) + " · IEEE-754 " + f->ieee + " · ";
    out += layout;
    if (!special.empty())
        out += " · " + special;
    // A ghost reading is only a hypothesis about the same bits, and the caption
    // has to say so instead of showing a number that is not the result.
    out += ghost ? (m_zh ? " · 非当前结果" : " · not the current result")
                 : " · " + m_floatText.toStdString();
    return utf8(out);
}

void BitsModel::copyFloatField(int segment)
{
    if (!container() || !m_error.isEmpty())
        return;
    const QVariantMap field = fieldFor(segment);
    copyValue(field.value("hex").toString(), field.value("key").toString());
}

// ---- expression history ------------------------------------------------------

void BitsModel::loadHistory()
{
    m_history.clear();
    m_historyPersistent = !m_historyPath.isEmpty();
    if (!m_historyPersistent)
        return;
    QFile file(m_historyPath);
    if (!file.open(QIODevice::ReadOnly))
        return; // no file yet: the first successful submit creates it
    const QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
    for (const QString& line : lines) {
        if (line.isEmpty() || line.size() > kMaxHistoryLine)
            continue;
        const std::string l = line.toStdString();
        HistoryEntry e { utf8(jsonField(l, "expr")), utf8(jsonField(l, "result")),
            utf8(jsonField(l, "type")) };
        if (!e.expr.isEmpty())
            m_history.push_back(std::move(e));
        if (m_history.size() >= kMaxHistory)
            break;
    }
}

void BitsModel::appendHistoryFile(const HistoryEntry& e)
{
    if (!m_historyPersistent)
        return;
    QFile file(m_historyPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        // An unwritable dataDir (strict sandbox) degrades to session-only.
        m_historyPersistent = false;
        winshun::debug("ra9fael.binarylab: history is session-only (dataDir is not writable)");
        return;
    }
    const std::string line = "{\"expr\":\"" + jsonEscape(e.expr.toStdString())
        + "\",\"result\":\"" + jsonEscape(e.result.toStdString())
        + "\",\"type\":\"" + jsonEscape(e.type.toStdString()) + "\"}\n";
    file.write(line.data(), static_cast<qint64>(line.size()));
}

void BitsModel::commitHistory()
{
    // Enter = submit: the history holds intentional expressions, not every
    // keystroke, and never an invalid one.
    const QString expr = m_expression.trimmed();
    if (expr.isEmpty() || !m_error.isEmpty()) {
        say(m_zh ? "表达式无效，未记入历史" : "invalid expression, not saved");
        return;
    }
    if (m_history.isEmpty() || m_history.back().expr != expr) {
        HistoryEntry e;
        e.expr = expr;
        e.result = m_isFloat ? m_floatText : hexText();
        e.type = m_isFloat ? utf8("f" + std::to_string(m_width)) : utf8(typeNameFor(m_width, m_signed));
        m_history.push_back(e);
        if (m_history.size() > kMaxHistory)
            m_history.erase(m_history.begin());
        appendHistoryFile(e);
        winshun::debug("ra9fael.binarylab: history append expr=" + expr.toStdString());
        emit historyChanged();
    }
    m_histPos = -1;
    m_historyOpen = false;
    say(m_zh ? "已提交 · 记入历史" : "submitted · saved to history");
    emit stateChanged();
}

void BitsModel::historyStep(bool up)
{
    // Up/Down browse submitted history (newest first); returning past the
    // newest entry restores whatever was typed before browsing began.
    if (m_history.isEmpty())
        return;
    if (m_histPos < 0) {
        if (!up)
            return;
        m_histDraft = m_expression;
        m_histPos = 0;
    } else if (up) {
        if (m_histPos + 1 >= m_history.size())
            return;
        ++m_histPos;
    } else if (m_histPos > 0) {
        --m_histPos;
    } else {
        m_histPos = -1;
        replaceExpression(m_histDraft, true);
        return;
    }
    const int vecIndex = static_cast<int>(m_history.size()) - 1 - m_histPos;
    replaceExpression(m_history[vecIndex].expr, true);
}

void BitsModel::pickHistory(int displayIndex)
{
    if (displayIndex < 0 || displayIndex >= m_history.size())
        return;
    m_histPos = -1;
    m_historyOpen = false;
    const int vecIndex = static_cast<int>(m_history.size()) - 1 - displayIndex;
    replaceExpression(m_history[vecIndex].expr, true);
    emit stateChanged();
}

void BitsModel::rewriteHistoryFile()
{
    if (!m_historyPersistent)
        return;
    QFile file(m_historyPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        // An unwritable dataDir (strict sandbox) degrades to session-only.
        m_historyPersistent = false;
        winshun::debug("ra9fael.binarylab: history is session-only (dataDir is not writable)");
        return;
    }
    for (const HistoryEntry& e : m_history) {
        const std::string line = "{\"expr\":\"" + jsonEscape(e.expr.toStdString())
            + "\",\"result\":\"" + jsonEscape(e.result.toStdString())
            + "\",\"type\":\"" + jsonEscape(e.type.toStdString()) + "\"}\n";
        file.write(line.data(), static_cast<qint64>(line.size()));
    }
}

void BitsModel::removeHistory(int displayIndex)
{
    if (displayIndex < 0 || displayIndex >= m_history.size())
        return;
    const int vecIndex = static_cast<int>(m_history.size()) - 1 - displayIndex;
    m_history.erase(m_history.begin() + vecIndex);
    m_histPos = -1; // the browsed row may be gone; leave browse mode
    rewriteHistoryFile();
    winshun::debug("ra9fael.binarylab: history remove entry " + std::to_string(displayIndex));
    emit historyChanged();
    emit stateChanged();
}

void BitsModel::clearHistory()
{
    m_history.clear();
    m_histPos = -1;
    m_historyOpen = false;
    rewriteHistoryFile();
    winshun::debug("ra9fael.binarylab: history cleared");
    emit historyChanged();
    emit stateChanged();
}

QVariantList BitsModel::historyEntries() const
{
    QVariantList list;
    list.reserve(m_history.size());
    for (auto it = m_history.crbegin(); it != m_history.crend(); ++it) {
        list.push_back(QVariantMap { { "expr", it->expr }, { "result", it->result },
            { "type", it->type } });
    }
    return list;
}

void BitsModel::setHistoryOpen(bool open)
{
    if (open == m_historyOpen)
        return;
    m_historyOpen = open;
    emit stateChanged();
}

void BitsModel::setFloatBarOn(bool on)
{
    if (on == m_floatBarOn)
        return;
    m_floatBarOn = on;
    winshun::debug(std::string("ra9fael.binarylab: float bar ") + (on ? "on" : "off"));
    emit stateChanged();
}

} // namespace binlab
