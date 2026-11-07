// BitsModel.h - the bits page of the QML workbench, as one QObject.
//
// The model holds the state the Win32 workbench's EditorState held for the
// bits page: the expression line, the type toolbar (kind u/i/f plus width),
// the value the grid edits, the readouts, the parity chips, the JSONL
// expression history and the float-bar data. The evaluation is the same
// evalBits() the query path uses, so the grid always edits the expression's
// result. Toolbar clicks go through setType()/setKind(), the Win32
// workbench's chain rewrite: the bit pattern is kept, and a leading type
// prefix in the expression is rewritten so the next edit agrees with the
// click instead of reverting it.

#pragma once

#include "../formats.h"
#include "../logic.h"

#include <QString>
#include <QVariantList>
#include <QVector>

#include <string>

namespace binlab {

class BitsModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString expression READ expression WRITE setExpression NOTIFY stateChanged)
    Q_PROPERTY(int width READ width NOTIFY stateChanged)
    // The grid's cells bind to this list, not to a bitAt() call: a QML
    // binding over an invokable records no dependency, so a flipped bit
    // would never repaint (the value changed, the cell did not).
    Q_PROPERTY(QVariantList bitValues READ bitValues NOTIFY stateChanged)
    Q_PROPERTY(bool signedType READ signedType NOTIFY stateChanged)
    Q_PROPERTY(QString typeName READ typeName NOTIFY stateChanged)
    Q_PROPERTY(int kind READ kind NOTIFY stateChanged) // 0 = u, 1 = i, 2 = f
    Q_PROPERTY(bool cMode READ cMode CONSTANT)         // host evalMode = c
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString hint READ hint NOTIFY stateChanged)
    Q_PROPERTY(bool isFloat READ isFloat NOTIFY stateChanged)
    Q_PROPERTY(QString floatText READ floatText NOTIFY stateChanged)
    Q_PROPERTY(QString unitNote READ unitNote NOTIFY stateChanged)
    // The status slot's shortened form: "4Mi = 4 x 2^20 = 4194304" -> "4Mi -> 4194304".
    Q_PROPERTY(QString unitNoteCompact READ unitNoteCompact NOTIFY stateChanged)
    Q_PROPERTY(QString hexText READ hexText NOTIFY stateChanged)
    Q_PROPERTY(QString octText READ octText NOTIFY stateChanged)
    Q_PROPERTY(QString binText READ binText NOTIFY stateChanged)
    Q_PROPERTY(QString binGroupedText READ binGroupedText NOTIFY stateChanged)
    Q_PROPERTY(QString decText READ decText NOTIFY stateChanged)
    Q_PROPERTY(QString unsignedText READ unsignedText NOTIFY stateChanged)
    // The chips bind to these lists, built here in C++ like bitValues(): the
    // page's Repeaters take a stable list from the model instead of rebuilding
    // a JS array on every state change.
    Q_PROPERTY(QVariantList readouts READ readouts NOTIFY stateChanged)
    Q_PROPERTY(QVariantList parity READ parity NOTIFY stateChanged)
    Q_PROPERTY(int onesCount READ onesCount NOTIFY stateChanged)
    Q_PROPERTY(bool allZero READ allZero NOTIFY stateChanged)
    Q_PROPERTY(int trailingZeros READ trailingZeros NOTIFY stateChanged)
    Q_PROPERTY(int leadingZeros READ leadingZeros NOTIFY stateChanged)
    // History, newest first for the dropdown; historyPos is in the same
    // display order (0 = newest) while Alt+Up/Down browses it.
    Q_PROPERTY(QVariantList historyEntries READ historyEntries NOTIFY historyChanged)
    Q_PROPERTY(int historyPos READ historyPos NOTIFY stateChanged)
    Q_PROPERTY(bool historyOpen READ historyOpen WRITE setHistoryOpen NOTIFY stateChanged)
    // The composition bar: on/off is the host setting's session value; the
    // ghost flag marks the hypothetical integer read of the same bits.
    Q_PROPERTY(bool floatBarOn READ floatBarOn WRITE setFloatBarOn NOTIFY stateChanged)
    Q_PROPERTY(bool floatHasContainer READ floatHasContainer NOTIFY stateChanged)
    Q_PROPERTY(bool floatGhost READ floatGhost NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool zh READ zh CONSTANT)

public:
    // Built from the EditorSpec the query row carried (blank expressions stay
    // blank: a click must not be overridden by a stale evaluation).
    explicit BitsModel(const struct EditorSpec& spec, QObject* parent = nullptr);

    QString expression() const { return m_expression; }
    void setExpression(const QString& text);
    int width() const { return m_width; }
    QVariantList bitValues() const;
    bool signedType() const { return m_signed; }
    QString typeName() const { return m_typeName; }
    int kind() const { return m_isFloat ? 2 : (m_signed ? 1 : 0); }
    bool cMode() const { return m_cMode; }
    QString errorText() const { return m_error; }
    QString hint() const { return m_hint; }
    bool isFloat() const { return m_isFloat; }
    QString floatText() const { return m_floatText; }
    QString unitNote() const { return m_unitNote; }
    QString unitNoteCompact() const;
    QString hexText() const;
    QString octText() const;
    QString binText() const;
    QString binGroupedText() const;
    QString decText() const;
    QString unsignedText() const;
    QVariantList readouts() const;   // HEX/OCT/BIN/DEC(FLOAT)/全部 chip entries
    QVariantList parity() const;     // ones/parity/even-bit/odd-bit/TZ/LZ entries
    int onesCount() const { return m_ones; }
    bool allZero() const { return m_value == 0; }
    int trailingZeros() const;
    int leadingZeros() const;
    QVariantList historyEntries() const;
    int historyPos() const { return m_histPos; }
    bool historyOpen() const { return m_historyOpen; }
    void setHistoryOpen(bool open);
    bool floatBarOn() const { return m_floatBarOn; }
    void setFloatBarOn(bool on);
    bool floatHasContainer() const { return floatByBits(m_width) != nullptr; }
    bool floatGhost() const;
    QString message() const { return m_message; }
    bool zh() const { return m_zh; }

    Q_INVOKABLE int bitAt(int index) const;      // LSB 0, inside the width
    Q_INVOKABLE void toggleBit(int index);
    Q_INVOKABLE void dragPaint(int index, int value);
    // The Win32 workbench's chain rewrite: keep the bits, adjust the type,
    // rewrite a leading type prefix. Between float containers the value is
    // re-encoded from its decimal text instead.
    Q_INVOKABLE void setType(int width, bool signed_);
    Q_INVOKABLE void setKind(int kind);          // 0 = u, 1 = i, 2 = f
    // Every clickable readout copies through here so the note names what was
    // taken ("已复制 HEX ✓").
    Q_INVOKABLE void copyValue(const QString& text, const QString& name);
    Q_INVOKABLE void clearMessage();
    Q_INVOKABLE QString allText() const;         // the 全部 chip's multi-line copy
    // The composition bar's data: the container layout, one field's numbers,
    // the caption line, and which field a grid cell's bit belongs to.
    Q_INVOKABLE QVariantMap floatInfo() const;   // {name, ieee, bits, expBits, mantBits}
    Q_INVOKABLE QVariantMap floatField(int segment) const; // 0=M, 1=E, 2=S
    Q_INVOKABLE QString floatCaption(int hoverSeg) const;
    Q_INVOKABLE void copyFloatField(int segment);
    Q_INVOKABLE int floatSegmentAt(int bit) const; // -1 when no container
    // History: Enter commits the line (intentional expressions only, not
    // every keystroke), Alt+Up/Down browses with a draft restore, the
    // dropdown picks and clear empties the file.
    Q_INVOKABLE void commitHistory();
    Q_INVOKABLE void historyStep(bool up);
    Q_INVOKABLE void pickHistory(int displayIndex);
    Q_INVOKABLE void clearHistory();

signals:
    void stateChanged();
    void messageChanged();
    void historyChanged();
    // The line was replaced by the model (chain rewrite, history browse).
    // The page moves the InputBox's caret to the end, where the Win32
    // workbench left it after every rewrite.
    void expressionReplaced(const QString& text);

private:
    struct HistoryEntry {
        QString expr;
        QString result;
        QString type;
    };

    void evaluate();
    void refresh();   // recompute the readouts from m_value
    void say(const QString& text);
    // Set the line from inside the model (the setType/setKind paths); the
    // Win32 workbench's replaceExpression().
    void replaceExpression(const QString& text, bool reEval);
    // The rewritten line, or a null QString when the line has no leading type
    // prefix to rewrite.
    QString rewriteTypePrefix(int width, bool signed_) const;
    void loadHistory();
    void appendHistoryFile(const HistoryEntry& e);
    QString noContainerMessage(int width) const;
    const FloatSpec* container() const { return floatByBits(m_width); }

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
    QString m_unitNote;
    int m_ones = 0;
    QString m_message;
    bool m_floatBarOn = false;

    QVector<HistoryEntry> m_history;  // oldest first; back = newest
    int m_histPos = -1;               // display order: 0 = newest, -1 = not browsing
    QString m_histDraft;
    bool m_historyOpen = false;
    bool m_historyPersistent = false; // false once dataDir proves unwritable
    QString m_historyPath;            // empty: session-only
};

} // namespace binlab
