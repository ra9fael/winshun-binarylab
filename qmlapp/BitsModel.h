// BitsModel.h - the bits page of the QML workbench, as one QObject.
//
// The model holds the state the Win32 workbench's EditorState held for the
// bits page: the expression line, the width and signedness toolbar, the value
// the grid edits, and the readouts (HEX/OCT/BIN/DEC, the parity row). The
// evaluation is the same evalBits() the query path uses, so the grid always
// edits the expression's result; a type prefix (u8/...) inside the expression
// overrides the toolbar, and the toolbar then reports back what the prefix
// chose. A bit click flips the value directly; the expression line keeps its
// text until the next edit (the prototype's simplification of the Win32
// workbench's rewrite-on-toggle).

#pragma once

#include "../formats.h"
#include "../logic.h"

#include <QString>
#include <QVariantList>

namespace binlab {

class BitsModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString expression READ expression WRITE setExpression NOTIFY stateChanged)
    Q_PROPERTY(int width READ width NOTIFY stateChanged)
    Q_PROPERTY(bool signedType READ signedType NOTIFY stateChanged)
    Q_PROPERTY(QString typeName READ typeName NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString hint READ hint NOTIFY stateChanged)
    Q_PROPERTY(bool isFloat READ isFloat NOTIFY stateChanged)
    Q_PROPERTY(QString floatText READ floatText NOTIFY stateChanged)
    Q_PROPERTY(QString hexText READ hexText NOTIFY stateChanged)
    Q_PROPERTY(QString octText READ octText NOTIFY stateChanged)
    Q_PROPERTY(QString binText READ binText NOTIFY stateChanged)
    Q_PROPERTY(QString binGroupedText READ binGroupedText NOTIFY stateChanged)
    Q_PROPERTY(QString decText READ decText NOTIFY stateChanged)
    Q_PROPERTY(QString unsignedText READ unsignedText NOTIFY stateChanged)
    Q_PROPERTY(int onesCount READ onesCount NOTIFY stateChanged)
    Q_PROPERTY(bool allZero READ allZero NOTIFY stateChanged)
    Q_PROPERTY(int trailingZeros READ trailingZeros NOTIFY stateChanged)
    Q_PROPERTY(int leadingZeros READ leadingZeros NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool zh READ zh CONSTANT)

public:
    // Built from the EditorSpec the query row carried (blank expressions stay
    // blank: a click must not be overridden by a stale evaluation).
    explicit BitsModel(const struct EditorSpec& spec, QObject* parent = nullptr);

    QString expression() const { return m_expression; }
    void setExpression(const QString& text);
    int width() const { return m_width; }
    bool signedType() const { return m_signed; }
    QString typeName() const { return m_typeName; }
    QString errorText() const { return m_error; }
    QString hint() const { return m_hint; }
    bool isFloat() const { return m_isFloat; }
    QString floatText() const { return m_floatText; }
    QString hexText() const;
    QString octText() const;
    QString binText() const;
    QString binGroupedText() const;
    QString decText() const;
    QString unsignedText() const;
    int onesCount() const { return m_ones; }
    bool allZero() const { return m_value == 0; }
    int trailingZeros() const;
    int leadingZeros() const;
    QString message() const { return m_message; }
    bool zh() const { return m_zh; }

    Q_INVOKABLE int bitAt(int index) const;      // LSB 0, inside the width
    Q_INVOKABLE void toggleBit(int index);
    Q_INVOKABLE void dragPaint(int index, int value);
    Q_INVOKABLE void setWidth(int width);
    Q_INVOKABLE void setSigned(bool signed_);
    Q_INVOKABLE void copy(const QString& text);
    Q_INVOKABLE void clearMessage();

signals:
    void stateChanged();
    void messageChanged();

private:
    void evaluate();
    void refresh();   // recompute the readouts from m_value
    void say(const QString& text);

    std::uint64_t m_value = 0;
    int m_width = 32;
    bool m_signed = false;
    bool m_cMode = false;
    bool m_zh = true;
    bool m_blank = false;          // spec.blankExpression: the line starts empty
    QString m_expression;
    QString m_typeName;
    QString m_error;
    QString m_hint;
    bool m_isFloat = false;
    QString m_floatText;
    int m_ones = 0;
    QString m_message;
};

} // namespace binlab
