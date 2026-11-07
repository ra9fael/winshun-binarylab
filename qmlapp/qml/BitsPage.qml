pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The bits page, the Win32 workbench's bits tab: the expression line with its
// JSONL history, the kind/width toolbar with the S|E|M toggle, the bit grid
// with keyboard focus and field bands, the composition bar, the base chips
// and the two-row status area. All state lives in the BitsModel; the page
// holds only view state (hover, keyboard focus cell, the kill ring).
Item {
    id: page

    required property var lab // the BitsModel
    property var workbench    // the shell Window, for the Esc chain's last step

    readonly property real dpr: Screen.devicePixelRatio
    readonly property int rows: Math.ceil(lab.width / 8)
    // View state the Win32 workbench kept in EditorState: the hovered parity
    // chip (the status line swaps in its definition) and the keyboard-focused
    // cell (arrows move it, Space flips it, a click claims it).
    property int hoverParity: -1
    property int focusBit: -1
    readonly property int hoverSeg: floatBar.hoverSeg

    // The Win32 workbench's floatSegColorByField(); FloatBar's pills use the
    // same three colours.
    readonly property color colorS: "#BE82F0"
    readonly property color colorE: "#58A6FF"
    readonly property color colorM: "#7FD1A8"

    implicitHeight: col.implicitHeight

    // old Esc chain: close the dropdown, then clear the line, then the window
    Keys.onEscapePressed: (event) => {
        if (page.lab.historyOpen) {
            page.lab.historyOpen = false
            event.accepted = true
            return
        }
        if (expr.inputFocused && expr.text.length > 0) {
            expr.clear()
            page.lab.expression = ""
            event.accepted = true
            return
        }
        if (page.workbench)
            page.workbench.close()
        event.accepted = true
    }

    function statusRight() {
        const lab = page.lab
        if (lab.message.length > 0)
            return lab.message
        if (lab.unitNote.length > 0)
            return lab.unitNoteCompact
        if (page.focusBit >= 0)
            return (lab.zh ? "焦点 位 " : "focus bit ") + page.focusBit
                + " = " + lab.bitAt(page.focusBit)
        return ""
    }

    function contextText() {
        const lab = page.lab
        if (page.hoverParity >= 0 && page.hoverParity < lab.parity.length)
            return lab.parity[page.hoverParity].definition
        if (page.hoverSeg >= 0)
            return lab.floatCaption(page.hoverSeg)
        if (lab.isFloat) {
            const info = lab.floatInfo()
            return info.bits > 0 ? info.name + " · IEEE-754 " + info.ieee + " · " + lab.floatText
                                 : lab.width + " bit float · " + lab.floatText
        }
        return (lab.cMode ? "C · " : "") + lab.typeName + " · " + lab.width + " bit · "
            + (lab.signedType ? (lab.zh ? "有符号" : "signed") : (lab.zh ? "无符号" : "unsigned"))
    }

    Column {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 12

        // ---- the expression line, re-evaluated on every change; Enter
        // commits into the JSONL history (the Win32 workbench's submit) ----
        Item {
            width: parent.width
            height: expr.height

            InputBox {
                id: expr
                anchors.left: parent.left
                anchors.right: historyBtn.left
                anchors.rightMargin: 8
                placeholder: page.lab.zh ? "表达式，如 u8 0xA5 & 0x0F" : "expression, e.g. u8 0xA5 & 0x0F"
                text: page.lab.expression
                error: page.lab.errorText.length > 0
                onTextChanged: page.lab.expression = text
                onAccepted: page.lab.commitHistory()
                Component.onCompleted: expr.takeFocus()
            }
            FlatButton {
                id: historyBtn
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: height
                text: page.lab.historyOpen ? "▴" : "▾"
                highlighted: page.lab.historyOpen
                onClicked: page.lab.historyOpen = !page.lab.historyOpen
            }
        }

        // The model replaced the line (toolbar rewrite, history browse); the
        // caret goes to the end, where the Win32 workbench left it.
        Connections {
            target: page.lab
            function onExpressionReplaced(text) {
                expr.text = text
                expr.cursorPosition = text.length
            }
            function onStateChanged() {
                if (page.focusBit >= page.lab.width)
                    page.focusBit = page.lab.width - 1
            }
        }

        // ---- the history dropdown: newest first, a row per submitted
        // expression, the browsed row highlighted ----------------------------
        Rectangle {
            visible: page.lab.historyOpen
            width: parent.width
            height: body.height + 16
            radius: Theme.cardRadius
            color: Theme.card
            border.width: Theme.hair(page.dpr)
            border.color: Theme.cardBorder

            Column {
                id: body
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6

                ListView {
                    id: histList
                    width: parent.width
                    height: Math.min(12, page.lab.historyEntries.length) * 28
                    visible: page.lab.historyEntries.length > 0
                    clip: true
                    model: page.lab.historyEntries
                    currentIndex: page.lab.historyPos

                    delegate: Item {
                        width: histList.width
                        height: 28

                        required property int index
                        required property var modelData

                        Rectangle {
                            anchors.fill: parent
                            color: index === page.lab.historyPos ? Theme.textSelection : "transparent"
                        }
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.right: readout.left
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.expr
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideRight
                        }
                        Text {
                            id: readout
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.result + " · " + modelData.type
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            elide: Text.ElideLeft
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: page.lab.pickHistory(index)
                        }
                    }
                }
                Text {
                    visible: page.lab.historyEntries.length === 0
                    text: page.lab.zh ? "暂无历史 · 在表达式行按 Enter 提交"
                                      : "No history yet · press Enter in the expression line to save"
                    color: Theme.faint
                    font.pixelSize: Theme.fontCaption
                }
                Item {
                    width: parent.width
                    height: clearBtn.height
                    FlatButton {
                        id: clearBtn
                        anchors.right: parent.right
                        text: page.lab.zh ? "清空历史" : "Clear history"
                        onClicked: page.lab.clearHistory()
                    }
                }
            }
        }

        Text {
            visible: page.lab.errorText.length > 0
            width: parent.width
            text: visible ? page.lab.errorText + " · " + page.lab.hint : ""
            color: Theme.danger
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideRight
        }

        // ---- kind / width toolbar; the expression's prefix overrides, and a
        // click rewrites it instead of reverting (the Win32 chain rewrite) --
        Row {
            spacing: 8
            Repeater {
                model: page.lab.zh
                    ? [ { k: 0, t: "无符号 u" }, { k: 1, t: "有符号 i" }, { k: 2, t: "浮点 f" } ]
                    : [ { k: 0, t: "unsigned u" }, { k: 1, t: "signed i" }, { k: 2, t: "float f" } ]
                delegate: FlatButton {
                    required property var modelData
                    text: modelData.t
                    highlighted: page.lab.kind === modelData.k
                    enabled: modelData.k !== 2 || page.lab.floatHasContainer
                    onClicked: page.lab.setKind(modelData.k)
                }
            }
            Item { width: 12; height: 1 }
            Repeater {
                model: [8, 16, 32, 64]
                delegate: FlatButton {
                    required property int modelData
                    text: String(modelData)
                    highlighted: page.lab.width === modelData
                    // 8 bits has no IEEE-754 container; the other widths
                    // re-encode the value between containers.
                    enabled: !(page.lab.isFloat && modelData === 8)
                    onClicked: page.lab.setType(modelData, page.lab.signedType)
                }
            }
            Item { width: 12; height: 1 }
            FlatButton {
                text: "S|E|M"
                highlighted: page.lab.floatBarOn
                onClicked: page.lab.floatBarOn = !page.lab.floatBarOn
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: page.lab.typeName
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
        }

        // ---- the bit grid: MSB left, click to flip, drag to paint; Tab and
        // the arrow keys move the focus cell, Space flips it -----------------
        Rectangle {
            id: gridCard
            width: parent.width
            height: grid.rows * grid.cellHeight
            radius: Theme.cardRadius
            color: Theme.card
            border.width: Theme.hair(page.dpr)
            border.color: Theme.cardBorder
            activeFocusOnTab: true

            readonly property bool keyed: activeFocus && page.focusBit >= 0

            Keys.onPressed: (event) => {
                const w = page.lab.width
                const b = page.focusBit
                switch (event.key) {
                case Qt.Key_Left:
                    page.focusBit = b < 0 ? w - 1 : Math.min(w - 1, b + 1)
                    event.accepted = true
                    break
                case Qt.Key_Right:
                    page.focusBit = b < 0 ? 0 : Math.max(0, b - 1)
                    event.accepted = true
                    break
                case Qt.Key_Up:
                    page.focusBit = b < 0 ? w - 1 : Math.min(w - 1, b + 8)
                    event.accepted = true
                    break
                case Qt.Key_Down:
                    page.focusBit = b < 0 ? 0 : Math.max(0, b - 8)
                    event.accepted = true
                    break
                case Qt.Key_Home:
                    page.focusBit = w - 1
                    event.accepted = true
                    break
                case Qt.Key_End:
                    page.focusBit = 0
                    event.accepted = true
                    break
                case Qt.Key_Space:
                    if (b >= 0) {
                        page.lab.toggleBit(b)
                        event.accepted = true
                    }
                    break
                }
            }

            Grid {
                id: grid
                anchors.fill: parent
                columns: 8
                rows: page.rows

                readonly property real cellWidth: width / columns
                readonly property real cellHeight: 50
                property int paintValue: 0 // what a drag paints: the flip of the first cell

                function cellAt(x, y) {
                    const col = Math.min(columns - 1, Math.max(0, Math.floor(x / cellWidth)))
                    const row = Math.min(rows - 1, Math.max(0, Math.floor(y / cellHeight)))
                    return row * columns + col
                }

                Repeater {
                    model: page.lab.width

                    Rectangle {
                        id: cell

                        required property int index
                        // MSB left: cell 0 holds the width-1th bit.
                        readonly property int bit: page.lab.width - 1 - index
                        // The list property, not bitAt(): a binding over an
                        // invokable never re-evaluates when the value changes.
                        readonly property bool on: page.lab.bitValues[bit] === 1
                        // Which field the bit belongs to (the composition bar's
                        // colours), only drawn while the bar is on.
                        readonly property int seg: page.lab.floatSegmentAt(bit)

                        width: grid.cellWidth
                        height: grid.cellHeight
                        color: on ? Theme.accentFill : "transparent"

                        Rectangle { // the field band, the Win32 grid's top strip
                            anchors.top: parent.top
                            width: parent.width
                            height: 2
                            visible: page.lab.floatBarOn && parent.seg >= 0
                            color: parent.seg === 2 ? page.colorS
                                 : parent.seg === 1 ? page.colorE : page.colorM
                            opacity: page.lab.floatGhost ? 0.45 : 1
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top
                            anchors.topMargin: 4
                            text: String(cell.bit)
                            color: cell.on ? Theme.accentText : Theme.faint
                            font.pixelSize: Theme.fontCaption - 2
                        }
                        Text {
                            anchors.centerIn: parent
                            anchors.verticalCenterOffset: 6
                            text: cell.on ? "1" : "0"
                            color: cell.on ? Theme.accentText : Theme.text
                            font.family: "Consolas"
                            font.pixelSize: Theme.fontTitle
                            font.weight: Theme.fontStrong
                        }
                        Rectangle { // the keyboard-focus ring
                            anchors.fill: parent
                            color: "transparent"
                            border.width: gridCard.keyed && page.focusBit === cell.bit ? 2 : 0
                            border.color: Theme.accentText
                        }
                        Rectangle { // the cell's edge, a device pixel wide
                            anchors.fill: parent
                            color: "transparent"
                            border.width: Theme.hair(page.dpr)
                            border.color: Theme.divider
                        }
                    }
                }
            }

            MouseArea {
                id: gridMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onPressed: (mouse) => {
                    const i = grid.cellAt(mouse.x, mouse.y)
                    const bit = page.lab.width - 1 - i
                    page.focusBit = bit
                    gridCard.forceActiveFocus()
                    grid.paintValue = page.lab.bitAt(bit) === 1 ? 0 : 1
                    page.lab.dragPaint(bit, grid.paintValue)
                }
                onPositionChanged: (mouse) => {
                    if (!pressed)
                        return
                    const i = grid.cellAt(mouse.x, mouse.y)
                    page.lab.dragPaint(page.lab.width - 1 - i, grid.paintValue)
                }
            }
        }

        // ---- the composition bar (S|E|M), shown while the toggle is on ----
        FloatBar {
            id: floatBar
            visible: page.lab.floatBarOn
            width: parent.width
            lab: page.lab
            dpr: page.dpr
        }

        // ---- HEX / OCT / BIN / DEC (FLOAT) / 全部, a chip per base: click to
        // copy (a Flow, so a narrow window wraps the BIN chip to the next line
        // — the Win32 workbench's chip wrapping, 93a2b26) --------------------
        Flow {
            width: parent.width
            spacing: 8
            Repeater {
                model: page.lab.readouts // the model's list property, built in C++
                delegate: FlatButton {
                    required property var modelData
                    text: modelData.all ? modelData.label : modelData.label + "  " + modelData.value
                    onClicked: page.lab.copyValue(modelData.value, modelData.name)
                }
            }
        }

        // ---- the status area, the Win32 workbench's two rows: the context
        // line (hovering a parity chip or a bar field swaps in its definition)
        // with the note slot on the right, then the error or the parity row --
        Column {
            width: parent.width
            spacing: 8

            Item {
                width: parent.width
                height: Math.max(ctx.implicitHeight, note.implicitHeight)

                Text {
                    id: ctx
                    anchors.left: parent.left
                    anchors.right: note.visible ? note.left : parent.right
                    anchors.rightMargin: 12
                    text: page.contextText()
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                }
                Text {
                    id: note
                    anchors.right: parent.right
                    visible: text.length > 0
                    width: Math.min(implicitWidth, page.width / 2)
                    text: page.statusRight()
                    color: page.lab.message.length > 0 ? Theme.success : Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideLeft
                }
            }

            Text {
                visible: page.lab.errorText.length > 0
                width: parent.width
                text: visible ? page.lab.errorText + " · " + page.lab.hint : ""
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }

            Row {
                visible: page.lab.errorText.length === 0
                spacing: 8
                Repeater {
                    model: page.lab.parity // built in C++, like the chips above
                    delegate: Rectangle {
                        id: pchip

                        required property var modelData
                        required property int index

                        width: prow.implicitWidth + 20
                        height: 30
                        radius: Theme.smallControlRadius
                        color: Theme.control
                        border.width: Theme.hair(page.dpr)
                        border.color: Theme.controlBorder

                        Row {
                            id: prow
                            anchors.centerIn: parent
                            spacing: 6
                            Text {
                                text: pchip.modelData.label
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption - 2
                            }
                            Text {
                                text: pchip.modelData.value
                                color: Theme.text
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onContainsMouseChanged: page.hoverParity = containsMouse
                                ? index : (page.hoverParity === index ? -1 : page.hoverParity)
                            onClicked: page.lab.copyValue(pchip.modelData.value, pchip.modelData.name)
                        }
                    }
                }
            }
        }
    }

    // ---- bash-style expression-line editing, the Win32 workbench's key
    // table: word moves and kills share one kill ring, Ctrl+Y yanks ---------
    property var killRing: []

    function isWordChar(c) {
        if (c === "_" || c === ".")
            return true
        if (c >= "0" && c <= "9")
            return true
        return c.toLowerCase() !== c.toUpperCase()
    }
    function wordStartBackward(pos) {
        let i = pos
        while (i > 0 && !page.isWordChar(expr.text[i - 1]))
            --i
        while (i > 0 && page.isWordChar(expr.text[i - 1]))
            --i
        return i
    }
    function wordEndForward(pos) {
        const n = expr.text.length
        let i = pos
        while (i < n && !page.isWordChar(expr.text[i]))
            ++i
        while (i < n && page.isWordChar(expr.text[i]))
            ++i
        return i
    }
    function kill(from, to) {
        if (to <= from)
            return
        page.killRing.push(expr.text.substring(from, to))
        if (page.killRing.length > 10)
            page.killRing.shift()
        const t = expr.text
        expr.text = t.substring(0, from) + t.substring(to)
        expr.cursorPosition = from
    }
    function yank() {
        if (page.killRing.length === 0)
            return
        const chunk = page.killRing[page.killRing.length - 1]
        const p = expr.cursorPosition
        const t = expr.text
        expr.text = t.substring(0, p) + chunk + t.substring(p)
        expr.cursorPosition = p + chunk.length
    }

    Shortcut {
        sequence: "Ctrl+A"
        enabled: expr.inputFocused
        onActivated: expr.cursorPosition = 0
    }
    Shortcut {
        sequence: "Ctrl+E"
        enabled: expr.inputFocused
        onActivated: expr.cursorPosition = expr.text.length
    }
    Shortcut {
        sequence: "Ctrl+U"
        enabled: expr.inputFocused
        onActivated: page.kill(0, expr.cursorPosition)
    }
    Shortcut {
        sequence: "Ctrl+K"
        enabled: expr.inputFocused
        onActivated: page.kill(expr.cursorPosition, expr.text.length)
    }
    Shortcut {
        sequence: "Ctrl+W"
        enabled: expr.inputFocused
        onActivated: page.kill(page.wordStartBackward(expr.cursorPosition), expr.cursorPosition)
    }
    Shortcut {
        sequence: "Ctrl+Backspace"
        enabled: expr.inputFocused
        onActivated: page.kill(page.wordStartBackward(expr.cursorPosition), expr.cursorPosition)
    }
    Shortcut {
        sequence: "Alt+Backspace"
        enabled: expr.inputFocused
        onActivated: page.kill(page.wordStartBackward(expr.cursorPosition), expr.cursorPosition)
    }
    Shortcut {
        sequence: "Ctrl+Delete"
        enabled: expr.inputFocused
        onActivated: page.kill(expr.cursorPosition, page.wordEndForward(expr.cursorPosition))
    }
    Shortcut {
        sequence: "Ctrl+Y"
        enabled: expr.inputFocused
        onActivated: page.yank()
    }
    Shortcut {
        sequence: "Ctrl+L"
        enabled: expr.inputFocused
        onActivated: page.kill(0, expr.text.length)
    }
    Shortcut {
        sequence: "Alt+Up"
        enabled: expr.inputFocused
        onActivated: page.lab.historyStep(true)
    }
    Shortcut {
        sequence: "Alt+Down"
        enabled: expr.inputFocused
        onActivated: page.lab.historyStep(false)
    }

    Timer { // the copied note fades (the Win32 workbench's kMsgFadeMs)
        id: msgFade
        interval: 2000
        running: page.lab.message.length > 0
        onTriggered: page.lab.clearMessage()
    }
    Connections {
        target: page.lab
        function onMessageChanged() {
            if (page.lab.message.length > 0)
                msgFade.restart()
        }
    }
}
