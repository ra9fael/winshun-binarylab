pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The help page, a placeholder until slice D: the Win32 workbench's two-column
// help (syntax, keys, chip definitions) arrives with the port.
Item {
    id: page

    required property var lab // the WorkbenchModel

    implicitHeight: note.implicitHeight

    Text {
        id: note
        width: parent.width
        text: page.lab.zh
            ? "帮助页移植中：语法、按键与各部件说明将在后续切片进入这里。"
            : "The help page is being ported: syntax, keys and the chip definitions arrive in a later slice."
        color: Theme.faint
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.WordWrap
    }
}
