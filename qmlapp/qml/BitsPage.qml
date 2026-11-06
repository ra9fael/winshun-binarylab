pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The bits page, moved out of the prototype's BitsWindow.qml unchanged: the
// expression line, the width/signedness toolbar, the bit grid, the base chips
// and the parity row, all driven by the BitsModel.
Item {
    id: page

    required property var lab // the BitsModel
    readonly property real dpr: Screen.devicePixelRatio
    readonly property int rows: Math.ceil(lab.width / 8)

    implicitHeight: col.implicitHeight

    Column {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 12

        // ---- the expression line, re-evaluated on every change ------------
        InputBox {
            id: expr
            width: parent.width
            placeholder: page.lab.zh ? "表达式，如 u8 0xA5 & 0x0F" : "expression, e.g. u8 0xA5 & 0x0F"
            text: page.lab.expression
            error: page.lab.errorText.length > 0
            onTextChanged: page.lab.expression = text
            Component.onCompleted: expr.takeFocus()
        }

        Text {
            visible: page.lab.errorText.length > 0
            width: parent.width
            text: visible ? page.lab.errorText + " · " + page.lab.hint : ""
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
                    highlighted: page.lab.width === modelData
                    onClicked: page.lab.setWidth(modelData)
                }
            }
            Item { width: 12; height: 1 }
            FlatButton {
                text: (page.lab.zh ? "无符号" : "unsigned") + " u"
                highlighted: !page.lab.signedType
                onClicked: page.lab.setSigned(false)
            }
            FlatButton {
                text: (page.lab.zh ? "有符号" : "signed") + " i"
                highlighted: page.lab.signedType
                onClicked: page.lab.setSigned(true)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: page.lab.typeName
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
            border.width: Theme.hair(page.dpr)
            border.color: Theme.cardBorder

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

        // ---- HEX / OCT / BIN / DEC (FLOAT), a chip per base: click to copy
        // (a Flow, so a narrow window wraps the BIN chip to the next line —
        // the Win32 workbench's chip wrapping, 93a2b26). The copied note sits
        // in the same Flow, as plain flow content: a child with anchors
        // inside a positioner is illegal and broke the Flow's layout the
        // moment the note appeared (the whole row collapsed to zero height
        // and never came back).
        Flow {
            width: parent.width
            spacing: 8
            Repeater {
                model: page.lab.readouts // the model's list property, built in C++
                delegate: FlatButton {
                    required property var modelData
                    text: modelData.label + "  " + modelData.value
                    onClicked: page.lab.copy(modelData.value)
                }
            }
            Text {
                text: page.lab.message
                color: Theme.success
                font.pixelSize: Theme.fontCaption
            }
        }

        // ---- the parity row, as the status bar's four labelled chips ------
        Row {
            spacing: 8
            Repeater {
                model: page.lab.parity // built in C++, like the chips above
                delegate: Rectangle {
                    id: chip

                    required property var modelData
                    width: chipRow.implicitWidth + 20
                    height: 30
                    radius: Theme.smallControlRadius
                    color: Theme.control
                    border.width: Theme.hair(page.dpr)
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
        running: page.lab.message.length > 0
        onTriggered: page.lab.clearMessage()
        onRunningChanged: if (!running) page.lab.clearMessage()
    }
}
