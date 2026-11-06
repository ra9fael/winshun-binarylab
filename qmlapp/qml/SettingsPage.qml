pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The settings page, a placeholder until slice D: the Win32 workbench shows
// the host settings snapshot read-only here, plus the workbench's own
// settings. Both arrive with the port.
Item {
    id: page

    required property var lab // the WorkbenchModel

    implicitHeight: note.implicitHeight

    Text {
        id: note
        width: parent.width
        text: page.lab.zh
            ? "设置页移植中：宿主设置快照（只读）与工作台设置将在后续切片进入这里。"
            : "The settings page is being ported: the host snapshot (read-only) and the workbench settings arrive in a later slice."
        color: Theme.faint
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.WordWrap
    }
}
