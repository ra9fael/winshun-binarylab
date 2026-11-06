// EditorQml.cpp - the QML workbench's window lifecycle (see editor.h here).
//
// Everything here runs on the GUI thread (winshunBinaryLabMain's); the public
// openEditorWindow/editorVisible/closeEditorAndWait marshal over from the
// serve() thread. One QQmlEngine lives for the process (the shared WinShun
// controls and QtQuick come from Win顺's qml\ folder); each open makes a
// window from BitsWindow.qml with a fresh BitsModel.

#include "editor.h"

#include "BitsModel.h"
#include "../winshun.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QThread>

#include "WindowFrame.h"

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
    return g_engine;
}

QString tr(bool zh, const char* chinese, const char* english)
{
    return zh ? QString::fromUtf8(chinese) : QString::fromUtf8(english);
}

// The GUI-thread half of openEditorWindow.
bool guiOpen(const EditorSpec& spec, std::string& error)
{
    error.clear();
    if (spec.mode != EditorSpec::Mode::Bits) {
        error = tr(spec.zh, "QML 工作台原型只实现了 bits 页", "The QML workbench prototype only has the bits page")
                    .toStdString();
        return false;
    }
    QQmlEngine* engine = ensureEngine();
    auto* model = new BitsModel(spec, engine);
    winshun::debug("ra9fael.binarylab: import paths: " + engine->importPathList().join(';').toStdString());
    QQmlComponent component(engine, u"WinShun"_s, u"BitsWindow"_s);
    if (component.isError()) { // the embedded module's qmldir by URL instead
        winshun::debug("ra9fael.binarylab: by-type load failed (" + component.errorString().toStdString()
            + "), trying the qrc URL");
        component.loadUrl(QUrl(u"qrc:/qt/qml/WinShun/BitsWindow.qml"_s), QQmlComponent::PreferSynchronous);
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
    g_frame->setWindow(window);
    g_window = window;
    QObject::connect(window, &QQuickWindow::destroyed, [] { g_visible = false; });
    g_visible = true;
    window->show();
    window->raise();
    window->requestActivate();
    winshun::debug("ra9fael.binarylab: QML workbench opened, window "
        + std::to_string(window->width()) + "x" + std::to_string(window->height())
        + " visible=" + std::to_string(window->isVisible() ? 1 : 0));
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
