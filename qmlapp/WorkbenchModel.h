// WorkbenchModel.h - the QML workbench's shell, as one QObject.
//
// The Win32 workbench's EditorState held the five tabs and, for the shared
// expression line, whose text it currently holds (editOwner). The QML shell
// gives every content page its own input instead, so the "whose text" state
// becomes the pages themselves: the bits page's line lives in BitsModel, the
// bytes page's here (until the bytes model arrives), and struct / settings /
// help have no line, exactly like the Win32 workbench. Tab switching in QML
// swaps Loaders, so nothing else has to be saved or restored.

#pragma once

#include "editor.h"

#include <QObject>
#include <QString>

namespace binlab {

class BitsModel;

class WorkbenchModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* bits READ bits CONSTANT)
    Q_PROPERTY(int tab READ tab WRITE setTab NOTIFY tabChanged)
    Q_PROPERTY(QString bytesExpression READ bytesExpression WRITE setBytesExpression
                   NOTIFY bytesExpressionChanged)
    Q_PROPERTY(bool zh READ zh CONSTANT)

public:
    explicit WorkbenchModel(const EditorSpec& spec, QObject* parent = nullptr);

    QObject* bits() const; // the BitsModel, parented to this
    int tab() const { return m_tab; }
    void setTab(int tab);
    QString bytesExpression() const { return m_bytesExpression; }
    void setBytesExpression(const QString& text);
    bool zh() const { return m_zh; }

signals:
    void tabChanged();
    void bytesExpressionChanged();

private:
    BitsModel* m_bits;
    int m_tab = 0;
    bool m_zh = true;
    QString m_bytesExpression;
};

} // namespace binlab
