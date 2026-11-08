pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The float composition bar, the Win32 workbench's paintFloatBar(): one fixed
// caption line above a proportional S|E|M pill row. The pill widths follow the
// container table with the same floors as the Win32 bar (a 1-bit sign and a
// 5-bit exponent stay clickable and labelable), an integer result shows its
// hypothetical float reading dimmed (the ghost), and a failed evaluation or a
// width without a container leaves the grey band under the caption alone.
Rectangle {
    id: bar

    required property var lab // the BitsModel
    property real dpr: 0

    // The Win32 workbench's floatSegColorByField(); BitsPage's cell bands use
    // the same three colours.
    readonly property color colorS: "#BE82F0"
    readonly property color colorE: "#58A6FF"
    readonly property color colorM: "#7FD1A8"
    readonly property color segText: "#121418"

    // Model properties, not invokables: a binding over an invokable records
    // no dependency and froze the pills at their first value.
    readonly property var info: bar.lab.floatInfo
    readonly property bool ready: bar.lab.errorText.length === 0 && info.bits > 0
    readonly property real pillFont: Theme.fontCaption - 1

    // Proportional to the field widths, with the Win32 bar's floors.
    readonly property var widths: {
        const W = band.width
        if (!(bar.info.bits > 0) || W <= 0)
            return [0, 0, 0]
        let wS = Math.max(20, W / bar.info.bits)
        let wE = Math.max(46, W * bar.info.expBits / bar.info.bits)
        if (wS + wE > W - 46)
            wE = Math.max(46, W - 46 - wS)
        return [wS, wE, W - wS - wE]
    }

    implicitHeight: caption.implicitHeight + 4 + band.height + 8
    color: "transparent"

    Text {
        id: caption
        anchors.top: parent.top
        width: parent.width
        text: bar.lab.floatCaption
        color: Theme.subtext
        font.pixelSize: Theme.fontCaption
        elide: Text.ElideRight
    }

    Rectangle {
        id: band
        anchors.top: caption.bottom
        anchors.topMargin: 4
        width: parent.width
        height: 24
        radius: Theme.smallControlRadius
        color: Theme.control
        border.width: Theme.hair(bar.dpr)
        border.color: Theme.controlBorder

        Repeater {
            // A STATIC model: the pill row is always S | E | M. A Repeater over
            // the model's dynamic floatFields list rebuilt — and destroyed —
            // the pills on every state change, including the hover change
            // fired from inside a pill's own HoverHandler (delete-during-emit:
            // the delayed 0xC0000409 the f32 session hit). Per-field values
            // arrive as bindings instead.
            model: [
                { seg: 2, key: "S" },
                { seg: 1, key: "E" },
                { seg: 0, key: "M" },
            ]
            delegate: Rectangle {
                id: pill

                required property var modelData
                required property int index

                // This pill's live entry (hex, bit count) from the model.
                readonly property var field: bar.lab.floatFields[index] ?? null

                x: modelData.seg === 2 ? 0
                     : modelData.seg === 1 ? bar.widths[0] : bar.widths[0] + bar.widths[1]
                width: bar.widths[modelData.seg === 2 ? 0 : modelData.seg === 1 ? 1 : 2]
                height: parent.height
                radius: Theme.smallControlRadius
                visible: bar.ready
                clip: true
                // The ghost is a hypothesis about the same bits, so it is drawn
                // dimmed; hovering a field dims the others the same way the
                // Win32 bar's hover glow did.
                opacity: (bar.lab.floatGhost ? 0.45 : 1)
                    * (bar.lab.hoverSeg >= 0 && bar.lab.hoverSeg !== modelData.seg ? 0.65 : 1)
                color: modelData.seg === 2 ? bar.colorS
                     : modelData.seg === 1 ? bar.colorE : bar.colorM

                // The Win32 bar's text degradation, estimated widths: the
                // full "E5 0x1C" when there is room, else "E 0x1C", else the
                // bare letter.
                readonly property int tier: {
                    const w = pill.width - 10
                    const cw = bar.pillFont * 0.62, mw = bar.pillFont * 0.55
                    const hex = pill.field ? pill.field.hex : ""
                    const bits = pill.field ? String(pill.field.bits) : ""
                    const keyW = pill.modelData.key.length * cw
                    const hexW = hex.length * mw
                    if (keyW + bits.length * cw + 5 + hexW <= w)
                        return 2
                    return keyW + 5 + hexW <= w ? 1 : 0
                }

                Row {
                    anchors.centerIn: parent
                    spacing: 5
                    Text {
                        text: pill.tier >= 2 && pill.field
                            ? pill.modelData.key + pill.field.bits : pill.modelData.key
                        color: bar.segText
                        font.pixelSize: bar.pillFont
                        font.weight: Theme.fontStrong
                    }
                    Text {
                        visible: pill.tier >= 1
                        text: pill.field ? pill.field.hex : ""
                        color: bar.segText
                        font.family: "Consolas"
                        font.pixelSize: bar.pillFont
                    }
                }

                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                    onHoveredChanged: bar.lab.hoverSeg = hovered ? pill.modelData.seg : -1
                }
                TapHandler {
                    onTapped: bar.lab.copyFloatField(pill.modelData.seg)
                }
            }
        }
    }
}
