pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The bytes page, as far as slice A of the Win32 port carries it: the page's
// own expression line (its text lives in the WorkbenchModel, so it survives
// tab switches, the Win32 workbench's editOwner semantics) and a dim note
// where the hexdump / records readings arrive with the bytes model.
Item {
    id: page

    required property var lab // the WorkbenchModel
    readonly property real dpr: Screen.devicePixelRatio

    implicitHeight: col.implicitHeight

    Column {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 12

        InputBox {
            id: expr
            width: parent.width
            // The Win32 bytes page's cue (editor.h), shared by both languages.
            placeholder: "hex u16le 34 12 78 56 · fmt blm_rawdata_256 <hex...>"
            text: page.lab.bytesExpression
            Component.onCompleted: expr.takeFocus()
        }

        Text {
            width: parent.width
            text: page.lab.zh
                ? "字节流页移植中：标量解码与记录模式将在后续切片进入这里。"
                : "The bytes page is being ported: the scalar and records readings arrive in a later slice."
            color: Theme.faint
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WordWrap
        }
    }
}
