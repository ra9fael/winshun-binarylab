pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The QML workbench prototype: the bits page. The same layout as the Win32
// workbench's bits mode — expression line, width/signedness toolbar, the bit
// grid, the base chips and the parity row — drawn from WinShun's own controls
// (Theme, TitleBar, InputBox, FlatButton), so the 界面风格 follows the host's.
Window {
    id: window

    required property var lab
    required property WindowFrame frame

    readonly property int rows: Math.ceil(lab.width / 8)
    readonly property real dpr: Screen.devicePixelRatio

    width: 720
    height: 244 + rows * 52
    minimumWidth: 640
    minimumHeight: 320
    color: Theme.backdrop && SystemTheme.materials && window.active ? "transparent" : Theme.page
    title: "Binary Lab · bits"
    flags: Qt.Window | Qt.FramelessWindowHint // the title bar is ours (see WindowFrame)

    Item {
        id: focusSink
        focus: true
        Keys.onEscapePressed: window.close()
    }

    TitleBar {
        id: titleBar
        z: 1
        width: parent.width
        frame: window.frame
        title: window.title
    }

    Column {
        id: page
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.margins: 16
        spacing: 12

        // ---- the expression line, re-evaluated on every change ------------
        InputBox {
            id: expr
            width: parent.width
            placeholder: window.lab.zh ? "表达式，如 u8 0xA5 & 0x0F" : "expression, e.g. u8 0xA5 & 0x0F"
            text: window.lab.expression
            error: window.lab.errorText.length > 0
            onTextChanged: window.lab.expression = text
            Component.onCompleted: expr.takeFocus()
        }

        Text {
            visible: window.lab.errorText.length > 0
            width: parent.width
            text: visible ? window.lab.errorText + " · " + window.lab.hint : ""
            color: Theme.danger
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideRight
        }

        // ---- width / signedness toolbar (the expression's prefix overrides)
        Row {
            spacing: 8
            Repeater {
                model: [8, 16, 32, 64]
                delegate: FlatButton {
                    required property int modelData
                    text: String(modelData)
                    highlighted: window.lab.width === modelData
                    onClicked: window.lab.setWidth(modelData)
                }
            }
            Item { width: 12; height: 1 }
            FlatButton {
                text: (window.lab.zh ? "无符号" : "unsigned") + " u"
                highlighted: !window.lab.signedType
                onClicked: window.lab.setSigned(false)
            }
            FlatButton {
                text: (window.lab.zh ? "有符号" : "signed") + " i"
                highlighted: window.lab.signedType
                onClicked: window.lab.setSigned(true)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: window.lab.typeName
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
        }

        // ---- the bit grid: MSB left, click to flip, drag to paint ---------
        Rectangle {
            id: gridCard
            width: parent.width
            height: grid.rows * grid.cellHeight
            radius: Theme.cardRadius
            color: Theme.card
            border.width: Theme.hair(window.dpr)
            border.color: Theme.cardBorder

            Grid {
                id: grid
                anchors.fill: parent
                columns: 8
                rows: window.rows

                readonly property real cellWidth: width / columns
                readonly property real cellHeight: 50
                property int paintValue: 0 // what a drag paints: the flip of the first cell

                function cellAt(x, y) {
                    const col = Math.min(columns - 1, Math.max(0, Math.floor(x / cellWidth)))
                    const row = Math.min(rows - 1, Math.max(0, Math.floor(y / cellHeight)))
                    return row * columns + col
                }

                Repeater {
                    model: window.lab.width

                    Rectangle {
                        id: cell

                        required property int index
                        // MSB left: cell 0 holds the width-1th bit.
                        readonly property int bit: window.lab.width - 1 - index
                        readonly property bool on: window.lab.bitAt(bit) === 1

                        width: grid.cellWidth
                        height: grid.cellHeight
                        color: on ? Theme.accentFill : "transparent"

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
                        Rectangle { // the cell's edge, a device pixel wide
                            anchors.fill: parent
                            color: "transparent"
                            border.width: Theme.hair(window.dpr)
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
                    const bit = window.lab.width - 1 - i
                    grid.paintValue = window.lab.bitAt(bit) === 1 ? 0 : 1
                    window.lab.dragPaint(bit, grid.paintValue)
                }
                onPositionChanged: (mouse) => {
                    if (!pressed)
                        return
                    const i = grid.cellAt(mouse.x, mouse.y)
                    window.lab.dragPaint(window.lab.width - 1 - i, grid.paintValue)
                }
            }
        }

        // ---- HEX / OCT / BIN / DEC (FLOAT), a chip per base: click to copy
        Row {
            spacing: 8
            Repeater {
                model: {
                    const chips = [
                        { label: "HEX", value: window.lab.hexText },
                        { label: "OCT", value: window.lab.octText },
                        { label: "BIN", value: window.lab.binText },
                        { label: window.lab.isFloat ? "FLOAT" : "DEC", value: window.lab.decText },
                    ]
                    if (window.lab.isFloat)
                        chips.push({ label: "DEC", value: window.lab.unsignedText })
                    return chips
                }
                delegate: FlatButton {
                    required property var modelData
                    text: modelData.label + "  " + modelData.value
                    onClicked: window.lab.copy(modelData.value)
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: window.lab.message
                color: Theme.success
                font.pixelSize: Theme.fontCaption
                Behavior on opacity { NumberAnimation { duration: 150 } }
            }
        }

        // ---- the parity row, as the status bar's four labelled chips ------
        Row {
            spacing: 8
            Repeater {
                model: [
                    { label: "PC", value: window.lab.onesCount % 2 === 0
                        ? (window.lab.zh ? "偶" : "even") : (window.lab.zh ? "奇" : "odd") },
                    { label: "0", value: window.lab.allZero
                        ? (window.lab.zh ? "全零" : "zero") : (window.lab.zh ? "非零" : "nonzero") },
                    { label: "TZ", value: String(window.lab.trailingZeros) },
                    { label: "LZ", value: String(window.lab.leadingZeros) },
                ]
                delegate: Rectangle {
                    id: chip

                    required property var modelData
                    width: chipRow.implicitWidth + 20
                    height: 30
                    radius: Theme.smallControlRadius
                    color: Theme.control
                    border.width: Theme.hair(window.dpr)
                    border.color: Theme.controlBorder

                    Row {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: 6
                        Text {
                            text: chip.modelData.label
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption - 2
                        }
                        Text {
                            text: chip.modelData.value
                            color: Theme.text
                            font.pixelSize: Theme.fontCaption
                        }
                    }
                }
            }
        }
    }

    Timer { // the copied note fades (the Win32 workbench's kMsgFadeMs)
        interval: 2000
        running: window.lab.message.length > 0
        onTriggered: window.lab.clearMessage()
        onRunningChanged: if (!running) window.lab.clearMessage()
    }
}
