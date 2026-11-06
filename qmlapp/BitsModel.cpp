#include "BitsModel.h"

#include "editor.h"

#include <QClipboard>
#include <QGuiApplication>

#include <algorithm>

namespace binlab {

namespace {

QString utf8(const std::string& text)
{
    return QString::fromStdString(text);
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
{
    // The workbench always evaluates in Fixed mode unless the host's evalMode
    // says C and the expression carries no type prefix (evalBits decides that
    // itself: a prefix promises an exact width and keeps Fixed semantics).
    if (!m_blank) {
        m_expression = utf8(spec.expression);
        evaluate();
    } else {
        m_typeName = utf8(typeNameFor(m_width, m_signed));
    }
    refresh();
}

void BitsModel::setExpression(const QString& text)
{
    if (m_expression == text)
        return;
    m_expression = text;
    m_blank = false;
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
    m_typeName = utf8(e.typeName);
    m_isFloat = e.isFloat;
    m_floatText = utf8(e.floatText);
}

void BitsModel::refresh()
{
    m_ones = 0;
    for (int i = 0; i < m_width; ++i)
        m_ones += static_cast<int>((m_value >> i) & 1);
    if (m_typeName.isEmpty())
        m_typeName = utf8(typeNameFor(m_width, m_signed));
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
    chips.push_back(QVariantMap { { "label", "HEX" }, { "value", hexText() } });
    chips.push_back(QVariantMap { { "label", "OCT" }, { "value", octText() } });
    chips.push_back(QVariantMap { { "label", "BIN" }, { "value", binText() } });
    chips.push_back(QVariantMap { { "label", m_isFloat ? "FLOAT" : "DEC" }, { "value", decText() } });
    if (m_isFloat)
        chips.push_back(QVariantMap { { "label", "DEC" }, { "value", unsignedText() } });
    return chips;
}

QVariantList BitsModel::parity() const
{
    QVariantList chips;
    chips.push_back(QVariantMap { { "label", "PC" },
        { "value", m_ones % 2 == 0 ? (m_zh ? "偶" : "even") : (m_zh ? "奇" : "odd") } });
    chips.push_back(QVariantMap { { "label", "0" },
        { "value", m_value == 0 ? (m_zh ? "全零" : "zero") : (m_zh ? "非零" : "nonzero") } });
    chips.push_back(QVariantMap { { "label", "TZ" }, { "value", trailingZeros() } });
    chips.push_back(QVariantMap { { "label", "LZ" }, { "value", leadingZeros() } });
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
    m_isFloat = false;
    m_floatText.clear();
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
    m_isFloat = false;
    m_floatText.clear();
    refresh();
    emit stateChanged();
}

void BitsModel::setWidth(int width)
{
    if (width != 8 && width != 16 && width != 32 && width != 64)
        return;
    if (width == m_width)
        return;
    m_width = width;
    if (m_expression.trimmed().isEmpty()) {
        // A bare value: keep its low bits and re-read it inside the new width.
        if (m_width < 64)
            m_value &= (std::uint64_t(1) << m_width) - 1;
    } else {
        evaluate();
    }
    refresh();
    emit stateChanged();
}

void BitsModel::setSigned(bool signed_)
{
    if (signed_ == m_signed)
        return;
    m_signed = signed_;
    if (!m_expression.trimmed().isEmpty())
        evaluate();
    refresh();
    emit stateChanged();
}

void BitsModel::copy(const QString& text)
{
    QGuiApplication::clipboard()->setText(text);
    say(m_zh ? "已复制" : "Copied");
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

} // namespace binlab
