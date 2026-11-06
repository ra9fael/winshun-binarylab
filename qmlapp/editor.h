// editor.h (qmlapp) - the QML workbench behind the same API the Win32
// workbench (../editor.h) gives main.cpp: openEditorWindow / editorVisible /
// closeEditorAndWait, over the same EditorSpec.
//
// Threading (the mirror of the Win32 workbench's): the Qt event loop and the
// window live on the GUI thread (winshunBinaryLabMain's), serve() runs on a
// worker thread, and these calls marshal over with
// Qt::BlockingQueuedConnection. GUI-side state needs no lock; the workbench
// prototype is the bits page only — a bytes/struct spec answers with an
// error instead of a window.
//
// This file must not write to stdout; report problems through `error` and
// winshun::debug (stderr).

#pragma once

#include "formats.h"

#include <cstdint>
#include <memory>
#include <string>

namespace binlab {

struct EditorSpec {
    enum class Mode { Bits, Bytes, Struct, Settings, Help };
    Mode mode = Mode::Bits;
    std::uint64_t value = 0;       // initial value; the expression overrides it
    int width = 32;                // initial width unless the expression sets one
    bool isSigned = false;         // signedness for the DEC readout
    std::string expression;        // initial expression text (query args)
    bool blankExpression = false;  // `bits ` with no words: start with no expression
    std::string endian = "le";     // bytes mode (not in the prototype yet)
    int pack = 0;                  // struct mode (not in the prototype yet)
    bool cMode = false;            // evalMode=c: C integer promotions in the expression line
    bool zh = true;                // window language

    // The host settings snapshot (shown read-only once the settings page
    // exists; the QML prototype ignores it).
    int hostWidth = 32;
    std::string hostEndian = "le";
    int hostPack = 0;
    std::string hostBitsEnter = "copy";
    std::string hostEvalMode = "fixed";
    std::string hostFloatBar = "off";
    std::string dataDir;           // host dataDir; expression history lives there
    std::shared_ptr<const FormatSet> formats;
};

bool openEditorWindow(const EditorSpec& spec, std::string& error);
bool editorVisible();
void closeEditorAndWait(unsigned timeoutMs);

// The plugin protocol loop (main.cpp's BinaryLab::serve()): what the DLL's
// worker thread runs. The DLL's build defines BINLAB_QML_WORKBENCH, which
// makes main.cpp compile BinaryLab into BinaryLabApp.dll and define this.
int runServeLoop();

} // namespace binlab
