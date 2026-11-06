// qmlmain.cpp - the entries of BinaryLabApp.dll, the QML workbench prototype.
//
// winshunBinaryLabMain: the plugin itself, as WinShun starts it. Qt goes up on
// this (the GUI) thread; the plugin protocol (winshun.h serve(), the query and
// action mapping in main.cpp) runs on a worker thread and marshals its window
// opens here (qmlapp/EditorQml.cpp). winshunBinaryLabPreview: `binarylab.exe
// --bits [--app-dir <folder>] [expression]` opens the bits page directly, no
// Win顺 and no protocol — how the prototype is checked.
//
// The runtime setup mirrors WinShun's winshun.capture (src/capture-plugin/
// PluginMain.cpp): Win顺's Qt, Win顺's font, the renderer the host is using.

#include "editor.h"
#include "../winshun.h"

#include <QGuiApplication>
#include <QQuickWindow>

#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>

#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <thread>

#include "SystemTheme.h"
#include "Win32Util.h"
#include "platform/WindowEffects.h"

using namespace Qt::StringLiterals;

namespace {

// Win顺's UI font (PluginMain.cpp, applyUiFont): Alibaba PuHuiTi from its
// fonts\, read as data when the path is outside the ANSI code page.
void applyUiFont(const QString& appDir)
{
    const QString fontDir = appDir + u"/fonts/"_s;
    bool ascii = true;
    for (const QChar c : fontDir)
        ascii = ascii && c.unicode() < 128;
    QString family;
    for (const QString& file : {u"AlibabaPuHuiTi-3-55-Regular.ttf"_s}) {
        int id = -1;
        if (ascii) {
            id = QFontDatabase::addApplicationFont(fontDir + file);
        } else {
            QFile data(fontDir + file);
            if (data.open(QIODevice::ReadOnly))
                id = QFontDatabase::addApplicationFontFromData(data.readAll());
        }
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (family.isEmpty() && !families.isEmpty())
            family = families.first();
    }
    if (family.isEmpty()) {
        qWarning("binarylab: no UI font in %s, using the system font", qUtf8Printable(QDir::toNativeSeparators(fontDir)));
        return;
    }
    QFont font(family);
    font.setHintingPreference(QFont::PreferVerticalHinting);
    QGuiApplication::setFont(font);
}

// The GUI thread's setup, shared by both entries.
void prepareGui(const QString& appDir, bool software)
{
    QCoreApplication::setLibraryPaths({appDir}); // platforms\qwindows.dll: Win顺's
    // The font engine as Win顺 has it: FreeType, GDI for the software renderer.
    qputenv("QT_QPA_PLATFORM", software ? "windows:fontengine=gdi" : "windows:fontengine=freetype");
    QCoreApplication::setOrganizationName(u"WinShun"_s);
    QCoreApplication::setApplicationName(u"WinShun"_s);
}

void startGui(const QString& appDir, bool software)
{
    qunsetenv("QT_QPA_PLATFORM");
    QQuickWindow::setGraphicsApi(software ? QSGRendererInterface::Software : QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    ws::SystemTheme::setBackdropAvailable(!software && ws::win::backdropSupported());
    applyUiFont(appDir);
    if (ws::SystemTheme::backdropAvailable()) {
        QFont font = QGuiApplication::font();
        font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::NoSubpixelAntialias));
        QGuiApplication::setFont(font);
    }
}

} // namespace

// The plugin: Win顺 starts binarylab.exe, the stub loads this and calls here.
extern "C" __declspec(dllexport) int winshunBinaryLabMain(const wchar_t* appDirName)
{
    const QString appDir = QString::fromWCharArray(appDirName);
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    // The prototype follows the host's renderer when the appearance says
    // software (the strict sandbox); otherwise D3D11. BINLAB_RENDERER=software
    // forces it for debugging.
    const bool software = qEnvironmentVariable("BINLAB_RENDERER") == "software";
    prepareGui(appDir, software);
    int argc = 1;
    char name[] = "BinaryLab";
    char* argv[] = {name, nullptr};
    QGuiApplication app(argc, argv);
    startGui(appDir, software);
    QGuiApplication::setQuitOnLastWindowClosed(false); // the host may open again

    // The plugin protocol on a worker thread: serve() blocks on stdin, the GUI
    // event loop runs here. serve() returning (shutdown, stdin closed) is the
    // process's end.
    int rc = 0;
    std::thread serve([&] {
        rc = binlab::runServeLoop();
        QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
    });
    app.exec();
    serve.join();
    return rc;
}

// The standalone check: the bits page, no Win顺. The stub passes Win顺's
// folder as --app-dir; the expression comes from the rest of the command line.
extern "C" __declspec(dllexport) int winshunBinaryLabPreview(const wchar_t* appDirName)
{
    winshun::debug("binarylab: preview entry");
    const QString appDir = QString::fromWCharArray(appDirName);
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    QStringList args;
    int count = 0;
    if (LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i)
            args.push_back(QString::fromWCharArray(wide[i]));
        ::LocalFree(wide);
    }
    const bool software = qEnvironmentVariable("BINLAB_RENDERER") == "software";
    prepareGui(appDir, software);
    int argc = 1;
    char name[] = "BinaryLab";
    char* argv[] = {name, nullptr};
    QGuiApplication app(argc, argv);
    startGui(appDir, software);
    QGuiApplication::setQuitOnLastWindowClosed(true);

    binlab::EditorSpec spec;
    spec.zh = !args.contains(u"--en"_s);
    QString expression;
    for (int i = 0; i < args.size(); ++i) {
        if (args.at(i) == u"--en"_s)
            continue;
        if (args.at(i) == u"--app-dir"_s) {
            ++i; // its value
            continue;
        }
        if (args.at(i) == u"--bits"_s)
            continue;
        expression = args.at(i); // the rest: the expression (last one wins)
    }
    spec.expression = expression.toStdString();
    spec.blankExpression = expression.isEmpty();
    spec.dataDir = appDir.toStdString();
    winshun::debug("binarylab: preview, expression='" + spec.expression + "'");

    std::string error;
    const bool opened = binlab::openEditorWindow(spec, error);
    winshun::debug(std::string("binarylab: preview openEditorWindow -> ") + (opened ? "true" : "false " + error));
    if (!opened) {
        std::fprintf(stderr, "binarylab: %s\n", error.c_str());
        std::fflush(stderr);
        return 2;
    }
    winshun::debug("binarylab: preview entering the event loop");
    return app.exec();
}
