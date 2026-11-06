pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The QML workbench shell, the same five tabs as the Win32 workbench's top
// bar: bits / bytes / struct as content modes, settings / help as tool tabs.
// The tab labels, the per-page expression texts and the placeholder wording
// follow the Win32 workbench (editor.h). Each content page is a Component
// loaded on demand; a page's expression text survives switching because it
// lives in the models, not in the page (the Win32 workbench's editOwner).
Window {
    id: window

    required property var lab // the WorkbenchModel
    required property WindowFrame frame

    readonly property real dpr: Screen.devicePixelRatio
    readonly property var tabLabels: lab.zh
        ? ["位运算", "字节流", "结构体", "设置", "帮助"]
        : ["Bits", "Bytes", "Struct", "Settings", "Help"]

    width: 720
    height: 620
    minimumWidth: 640
    minimumHeight: 320
    color: Theme.backdrop && SystemTheme.materials && window.active ? "transparent" : Theme.page
    title: "Binary Lab · " + tabLabels[lab.tab]
    flags: Qt.Window | Qt.FramelessWindowHint // the title bar is ours (see WindowFrame)

    // The Win32 workbench re-fits the window per mode; keep the loaded page's
    // height (the user can still resize freely afterwards). The page Item's
    // height is the Loader's (not constrained), so measure its implicitHeight.
    function fitHeight() {
        const content = loader.item
        if (!content || content.implicitHeight <= 0)
            return
        const wanted = titleBar.height + page.anchors.topMargin * 2 + tabs.height
                     + page.spacing + content.implicitHeight
        window.height = Math.max(window.minimumHeight, Math.ceil(wanted))
    }

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

        PageTabs {
            id: tabs
            width: parent.width
            labels: window.tabLabels
            current: window.lab.tab
            onActivated: (index) => {
                window.lab.tab = index
                window.fitHeight()
            }
        }

        Loader {
            id: loader
            width: parent.width
            // A page's Column reports its height after the first layout pass,
            // so measure once the loop has polished (Component.onCompleted
            // still sees 0).
            onLoaded: Qt.callLater(window.fitHeight)
            sourceComponent: [bitsPage, bytesPage, structPage, settingsPage, helpPage][window.lab.tab]
        }
    }

    Component {
        id: bitsPage
        BitsPage {
            lab: window.lab.bits
        }
    }
    Component {
        id: bytesPage
        BytesPage {
            lab: window.lab
        }
    }
    Component {
        id: structPage
        StructPage {
            lab: window.lab
        }
    }
    Component {
        id: settingsPage
        SettingsPage {
            lab: window.lab
        }
    }
    Component {
        id: helpPage
        HelpPage {
            lab: window.lab
        }
    }

    Component.onCompleted: window.fitHeight()
}
