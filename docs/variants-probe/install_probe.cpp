#include "Install.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <windows.h>
#include <stdexcept>
#include <cstdio>
using namespace Qt::StringLiterals;

void check(bool ok, const char* label) {
    if (!ok) throw std::runtime_error(label);
    std::printf("PASS %s\n", label);
}
QByteArray read(const QString& path) {
    QFile f(path); check(f.open(QIODevice::ReadOnly), "read installed file"); return f.readAll();
}
void write(const QString& path, const QByteArray& bytes) {
    QFile f(path); check(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(), "write owned fixture");
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        check(argc == 4, "gdi package, qml package, and test directory supplied");
        QDir().mkpath(QString::fromLocal8Bit(argv[3]));
        QTemporaryDir test(QString::fromLocal8Bit(argv[3]) + u"/install-XXXXXX"_s);
        check(test.isValid(), "isolated installation directory");
        const QString plugins = test.path() + u"/plugins"_s;
        const QString data = test.path() + u"/data/ra9fael.binarylab.data"_s;
        QDir().mkpath(data);
        const QByteArray history = "{\"expr\":\"u8 42\",\"result\":\"0x2A\",\"type\":\"u8\"}\n";
        write(data + u"/history.jsonl"_s, history);
        const QByteArray settings = "[Plugin.ra9fael.binarylab]\ndefaultWidth=16\nHotkey=Ctrl+Alt+B\n";
        write(test.path() + u"/settings.ini"_s, settings);
        const QString target = plugins + u"/ra9fael.binarylab"_s;
        const QString gdi = QString::fromLocal8Bit(argv[1]), qml = QString::fromLocal8Bit(argv[2]);
        for (const QString& package : {gdi, qml, qml, gdi}) {
            auto candidate = ws::plugin::install::inspect(package, test.path());
            check(candidate && candidate->manifest, "production archive and manifest inspection");
            const bool isQml = package == qml;
            check(candidate->manifest->id == u"ra9fael.binarylab", "shared installation identity");
            QString error;
            const QString placed = ws::plugin::install::place(*candidate, plugins, &error);
            if (placed.isEmpty()) std::fprintf(stderr, "%s\n", qUtf8Printable(error));
            check(!placed.isEmpty(), "production installer places or replaces edition");
            ws::plugin::install::discard(*candidate);
            check(QDir(plugins).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() == 1, "only one plugin directory remains");
            check(QFile::exists(target + u"/BinaryLabApp.dll"_s) == isQml, "QML DLL present only for QML");
            check(QJsonDocument::fromJson(read(target + u"/release.json"_s)).object().value(u"variant"_s).toString() == (isQml ? u"qml" : u"gdi"), "installed edition matches selected package");
            check(read(data + u"/history.jsonl"_s) == history, "history survives replacement and downgrade");
            check(read(test.path() + u"/settings.ini"_s) == settings, "settings and hotkey file unchanged");
        }
        auto candidate = ws::plugin::install::inspect(qml, test.path());
        const QString exe = QDir::toNativeSeparators(target + u"/binarylab.exe"_s);
        HANDLE locked = CreateFileW(reinterpret_cast<LPCWSTR>(exe.utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        check(locked != INVALID_HANDLE_VALUE, "hold old executable without delete sharing");
        QString error;
        const QString placed = ws::plugin::install::place(*candidate, plugins, &error);
        CloseHandle(locked);
        ws::plugin::install::discard(*candidate);
        check(placed.isEmpty() && !error.isEmpty(), "locked old executable prevents unsafe replacement");
        check(!QFile::exists(target + u"/BinaryLabApp.dll"_s), "failed replacement preserves GDI");
        std::puts("ALL INSTALL CHECKS PASSED");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
