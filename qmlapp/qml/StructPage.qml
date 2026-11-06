pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The struct page, a placeholder until slice D: the Win32 workbench's struct
// tab has no expression line (only bits and bytes do), so this is a dim note
// only.
Item {
    id: page

    required property var lab // the WorkbenchModel

    implicitHeight: note.implicitHeight

    Text {
        id: note
        width: parent.width
        text: page.lab.zh
            ? "结构体页移植中：布局、偏移与字节图将在后续切片进入这里。"
            : "The struct page is being ported: layout, offsets and the byte map arrive in a later slice."
        color: Theme.faint
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.WordWrap
    }
}
