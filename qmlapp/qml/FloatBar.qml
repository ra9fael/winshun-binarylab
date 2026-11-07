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

    readonly property var info: lab.floatInfo()
    readonly property bool ready: lab.errorText.length === 0 && info.bits > 0
    // 0 = M, 1 = E, 2 = S (floatSegment's numbering); the page's status line
    // reads this for its context swap.
    property int hoverSeg: -1

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
        text: bar.lab.floatCaption(bar.hoverSeg)
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
            model: [
                { seg: 2, key: "S", color: bar.colorS },
                { seg: 1, key: "E", color: bar.colorE },
                { seg: 0, key: "M", color: bar.colorM },
            ]
            delegate: Rectangle {
                id: pill

                required property var modelData

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
                    * (bar.hoverSeg >= 0 && bar.hoverSeg !== modelData.seg ? 0.65 : 1)
                color: modelData.color

                Row {
                    anchors.centerIn: parent
                    spacing: 5
                    Text {
                        text: pill.modelData.key
                        color: bar.segText
                        font.pixelSize: Theme.fontCaption - 1
                        font.weight: Theme.fontStrong
                    }
                    Text {
                        text: bar.lab.floatField(pill.modelData.seg).hex
                        color: bar.segText
                        font.family: "Consolas"
                        font.pixelSize: Theme.fontCaption - 1
                    }
                }

                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                    onHoveredChanged: bar.hoverSeg = hovered ? pill.modelData.seg : -1
                }
                TapHandler {
                    onTapped: bar.lab.copyFloatField(pill.modelData.seg)
                }
            }
        }
    }
}
