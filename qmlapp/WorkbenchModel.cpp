#include "WorkbenchModel.h"

#include "BitsModel.h"

#include <algorithm>

namespace binlab {

WorkbenchModel::WorkbenchModel(const EditorSpec& spec, QObject* parent)
    : QObject(parent)
    , m_bits(new BitsModel(spec, this))
    , m_tab(std::clamp(static_cast<int>(spec.mode), 0, 4))
    , m_zh(spec.zh)
{
}

QObject* WorkbenchModel::bits() const
{
    return m_bits;
}

void WorkbenchModel::setTab(int tab)
{
    tab = std::clamp(tab, 0, 4);
    if (tab == m_tab)
        return;
    m_tab = tab;
    emit tabChanged();
}

void WorkbenchModel::setBytesExpression(const QString& text)
{
    if (m_bytesExpression == text)
        return;
    m_bytesExpression = text;
    emit bytesExpressionChanged();
}

} // namespace binlab
