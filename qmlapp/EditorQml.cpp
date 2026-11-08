// EditorQml.cpp - the QML workbench's window lifecycle (see editor.h here).
//
// Everything here runs on the GUI thread (winshunBinaryLabMain's); the public
// openEditorWindow/editorVisible/closeEditorAndWait marshal over from the
// serve() thread. One QQmlEngine lives for the process (the shared WinShun
// controls and QtQuick come from Win顺's qml\ folder); each open makes a
// Workbench window with a fresh WorkbenchModel (which owns the pages' models).

#include "editor.h"

#include "WorkbenchModel.h"
#include "../winshun.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QStyleHints>
#include <QSurfaceFormat>
#include <QThread>

#include "WindowFrame.h"
#include "SystemTheme.h"
#include "platform/WindowEffects.h"

#include <algorithm>
#include <atomic>
#include <memory>

using namespace Qt::StringLiterals;

namespace binlab {

namespace {

std::atomic<bool> g_visible { false };
QPointer<QQuickWindow> g_window;
std::unique_ptr<ws::WindowFrame> g_frame;
QQmlEngine* g_engine = nullptr; // the GUI thread's, alive to the process

QString appDir()
{
    return qEnvironmentVariable("WS_APP_DIR"); // set by the stub
}

QQmlEngine* ensureEngine()
{
    if (g_engine)
        return g_engine;
    g_engine = new QQmlEngine;
    g_engine->addImportPath(appDir() + u"/qml"_s); // QtQuick and the rest: Win顺's
    g_frame = std::make_unique<ws::WindowFrame>(true); // a resizable workbench
    // The title bar's icon slot is drawn empty until the frame has one; this
    // is static and set once, before any title bar exists. The png travels
    // inside BinaryLabApp.dll's qrc (CMakeLists.txt).
    ws::WindowFrame::setIcon(u"qrc:/plugin-icons/app-32.png"_s);
    return g_engine;
}

// The GUI-thread half of openEditorWindow.
bool guiOpen(const EditorSpec& spec, std::string& error)
{
    error.clear();
    // One workbench at a time, and the previous window must die synchronously
    // before the next one takes the WindowFrame over: the old window's
    // WM_NCDESTROY clears the frame's HWND while the frame still points at
    // it, and re-fitting a second window over that live HWND crashed the
    // process (0xC0000005 on every same-process reopen).
    if (g_window) {
        g_window->close();
        delete g_window.data(); // destroyed fires: g_visible drops to false
    }
    QQmlEngine* engine = ensureEngine();
    auto* model = new WorkbenchModel(spec, engine);
    winshun::debug("ra9fael.binarylab: import paths: " + engine->importPathList().join(';').toStdString());
    QQmlComponent component(engine, u"WinShun"_s, u"Workbench"_s);
    if (component.isError()) { // the embedded module's qmldir by URL instead
        winshun::debug("ra9fael.binarylab: by-type load failed (" + component.errorString().toStdString()
            + "), trying the qrc URL");
        component.loadUrl(QUrl(u"qrc:/qt/qml/WinShun/Workbench.qml"_s), QQmlComponent::PreferSynchronous);
    }
    QObject* object = component.createWithInitialProperties({
        {u"lab"_s, QVariant::fromValue<QObject*>(model)},
        {u"frame"_s, QVariant::fromValue(static_cast<QObject*>(g_frame.get()))},
    });
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        qWarning().noquote() << "binarylab: no workbench window:" << component.errorString();
        error = component.errorString().trimmed().toStdString();
        delete object;
        return false;
    }
    window->setIcon(QGuiApplication::windowIcon());
    g_frame->setWindow(window);
    // The DWM frame and the Mica backdrop, as capture's prepareWindow gives
    // them to every frameless WinShun QML window: an alpha-buffered swapchain
    // (without an alpha channel the window's transparent colour renders as
    // opaque black — the workbench read as a black window on light tokens),
    // the native frame with Mica behind the content, and the dark title
    // frame. Applied with the window created but not shown, as there.
    const bool backdrop = ws::SystemTheme::backdropAvailable();
    if (backdrop) {
        QSurfaceFormat format = window->requestedFormat();
        if (format.alphaBufferSize() < 8) {
            format.setAlphaBufferSize(8);
            window->setFormat(format);
        }
    }
    window->create();
    ws::win::styleFramelessWindow(window, backdrop);
    ws::win::setDarkFrame(window,
        QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    ws::win::keepDarkFrameOnShow(window);
    g_window = window;
    QObject::connect(window, &QQuickWindow::destroyed, [] { g_visible = false; });
    g_visible = true;
    window->show();
    window->raise();
    window->requestActivate();
    winshun::debug("ra9fael.binarylab: QML workbench opened, window "
        + std::to_string(window->width()) + "x" + std::to_string(window->height())
        + " visible=" + std::to_string(window->isVisible() ? 1 : 0));
    // The shell's fitHeight() runs on Component.onCompleted; log the settled
    // size once the event loop gets there.
    QPointer<QQuickWindow> logged = window;
    QMetaObject::invokeMethod(qGuiApp, [logged] {
        if (logged)
            winshun::debug("ra9fael.binarylab: workbench settled at "
                + std::to_string(logged->width()) + "x" + std::to_string(logged->height()));
    }, Qt::QueuedConnection);
    return true;
}

} // namespace

// ---- the serve-thread API (main.cpp's) ------------------------------------------

bool openEditorWindow(const EditorSpec& spec, std::string& error)
{
    // The preview calls from the GUI thread itself: no marshal (a
    // BlockingQueuedConnection to the current thread would deadlock).
    if (QThread::currentThread() == qGuiApp->thread())
        return guiOpen(spec, error);
    std::string guiError;
    QMetaObject::invokeMethod(qGuiApp, [&] { guiOpen(spec, guiError); }, Qt::BlockingQueuedConnection);
    error = std::move(guiError);
    return error.empty();
}

bool editorVisible()
{
    return g_visible.load();
}

void closeEditorAndWait(unsigned timeoutMs)
{
    if (!g_window)
        return;
    if (QThread::currentThread() == qGuiApp->thread()) {
        g_window->close();
        return;
    }
    QMetaObject::invokeMethod(qGuiApp, [] {
        if (g_window)
            g_window->close();
    }, Qt::BlockingQueuedConnection);
    // close() is synchronous on the GUI thread; the timeout is for the marshal
    // itself, which a healthy event loop answers at once. The deadline stays
    // for the API's shape (the Win32 workbench joins a thread here).
    (void)timeoutMs;
}

} // namespace binlab
