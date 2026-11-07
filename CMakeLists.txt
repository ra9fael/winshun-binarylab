cmake_minimum_required(VERSION 3.21)
project(binarylab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON) # BinaryLabApp's Q_OBJECT types (the stub keeps none)

# The generated C++ template builds the exe straight into the plugin folder,
# because plugin.json names it as runtime.program. Keep that convention.
# The QML workbench prototype (the qml branch): the same split as WinShun's
# own winshun.capture (src/capture-plugin). binarylab.exe is a starter without
# Qt (qmlapp/stub/main.cpp): it loads BinaryLabApp.dll, built on Win顺's Qt
# (WS_APP_DIR), which runs the plugin protocol on a worker thread and the QML
# bits page on the GUI thread. plugin.json keeps naming binarylab.exe.
set(WINSHUN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../WinShun" CACHE PATH
    "The WinShun checkout the shared QML controls and their C++ come from")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickShapes)
# The QML module's resources under :/qt/qml/<URI>, as WinShun's own app does;
# without it they land at :/<URI>, where the engine never looks.
qt_policy(SET QTP0001 NEW)

add_executable(binarylab qmlapp/stub/main.cpp)
target_compile_definitions(binarylab PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# Beside BinaryLabApp.dll, where plugin.json's runtime.program points.
set_target_properties(binarylab PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(binarylab PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_SOURCE_DIR}")
endforeach()
if(MSVC)
    target_compile_options(binarylab PRIVATE /utf-8 /W4 /permissive-)
    # Static CRT: the exe needs no runtime DLL, which the strict sandbox cannot load.
    set_target_properties(binarylab PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(binarylab PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra -O2)
    target_link_options(binarylab PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# The plugin itself, built on Win顺's Qt. Its sources: the query/action
# mapping (main.cpp, unchanged), the QML workbench (qmlapp), and the shared
# app C++ the WinShun module's QML controls need (SystemTheme and friends), as
# capture's CMakeLists gathers them.
set(app_dir "${WINSHUN_DIR}/src/app")
set(core_dir "${WINSHUN_DIR}/src/core")
qt_add_library(BinaryLabApp SHARED
    main.cpp
    qmlapp/qmlmain.cpp
    qmlapp/EditorQml.cpp
    qmlapp/WorkbenchModel.cpp
    qmlapp/BitsModel.cpp
    "${app_dir}/SystemTheme.cpp" "${app_dir}/SystemTheme.h"
    "${app_dir}/Phosphor.cpp" "${app_dir}/Phosphor.h"
    "${app_dir}/PhosphorGlyph.cpp" "${app_dir}/PhosphorGlyph.h"
    "${app_dir}/WindowFrame.cpp" "${app_dir}/WindowFrame.h"
    "${app_dir}/platform/WindowEffects.cpp" "${app_dir}/platform/WindowEffects.h"
    "${app_dir}/platform/Foreground.cpp" "${app_dir}/platform/Foreground.h"
    "${core_dir}/Win32Util.cpp" "${core_dir}/Win32Util.h" "${core_dir}/OwnInput.h"
)
target_compile_definitions(BinaryLabApp PRIVATE BINLAB_QML_WORKBENCH UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# WinShun's shared app sources are C++20 (std::ranges, std::to_array), as its
# own CMakeLists builds them. The plugin's own files stay C++17: formats_toml.h
# speaks toml++ in its C++17 string world (char8_t off), and logic.h has always
# built that way.
set_target_properties(BinaryLabApp PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
set_source_files_properties(main.cpp qmlapp/qmlmain.cpp qmlapp/EditorQml.cpp qmlapp/WorkbenchModel.cpp qmlapp/BitsModel.cpp
    PROPERTIES COMPILE_OPTIONS "/std:c++17")
target_include_directories(BinaryLabApp PRIVATE
    "${CMAKE_SOURCE_DIR}" "${app_dir}" "${app_dir}/platform" "${core_dir}")
target_include_directories(BinaryLabApp SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party/tomlplusplus")

# The QML module URI WinShun, as capture's: the shared controls from WinShun's
# own module, flattened by QT_RESOURCE_ALIAS, plus this prototype's page.
set(shared_qml
    Theme.qml Hook.qml UiScale.qml WindowEdge.qml HoverTip.qml DropMenu.qml Glyph.qml TitleBar.qml ToggleSwitch.qml FlatButton.qml
    InputBox.qml Caret.qml CenteredNumber.qml KeyCap.qml HotkeyRecorder.qml ListScrollBar.qml ListWheelScroll.qml
    ScrollPixelAlignment.qml SettingsSection.qml SettingRow.qml PageTabs.qml)
set(binarylab_qml
    "${CMAKE_CURRENT_SOURCE_DIR}/qmlapp/qml/Workbench.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qmlapp/qml/BitsPage.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qmlapp/qml/FloatBar.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qmlapp/qml/BytesPage.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qmlapp/qml/StructPage.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qmlapp/qml/SettingsPage.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qmlapp/qml/HelpPage.qml")
foreach(name IN LISTS shared_qml)
    list(APPEND binarylab_qml "${app_dir}/qml/${name}")
endforeach()
set_source_files_properties("${app_dir}/qml/Theme.qml" PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
foreach(file IN LISTS binarylab_qml)
    get_filename_component(name "${file}" NAME)
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${name}")
endforeach()
# The 界面风格 Look files, as WinShun's own module has them.
file(GLOB theme_qml CONFIGURE_DEPENDS "${app_dir}/qml/themes/*/*.qml")
foreach(file IN LISTS theme_qml)
    file(RELATIVE_PATH alias "${app_dir}/qml" "${file}")
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${alias}" QT_QML_SKIP_QMLDIR_ENTRY TRUE)
endforeach()
list(APPEND binarylab_qml ${theme_qml})
file(GLOB theme_images CONFIGURE_DEPENDS "${app_dir}/qml/themes/*/*.png")
foreach(file IN LISTS theme_images)
    file(RELATIVE_PATH alias "${app_dir}/qml" "${file}")
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${alias}")
endforeach()

set(qml_import_scan "")
if(QT6_IS_SHARED_LIBS_BUILD)
    set(qml_import_scan NO_IMPORT_SCAN) # as WinShun's (src/app/CMakeLists.txt)
endif()
qt_add_qml_module(BinaryLabApp
    URI WinShun
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/WinShun"
    VERSION 1.0
    NO_PLUGIN
    ${qml_import_scan}
    NO_GENERATE_EXTRA_QMLDIRS
    QML_FILES ${binarylab_qml}
    RESOURCES ${theme_images}
    SOURCES "${app_dir}/HotkeyRule.h"
)
# Bytecode only, no QML compiled to C++: the plugin runs on whichever Qt 6 its
# Win顺 comes with (see capture's CMakeLists).
set_target_properties(BinaryLabApp PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")

# The icons (Phosphor, see Phosphor.h), as WinShun's and capture's.
qt_add_resources(BinaryLabApp "binarylab_phosphor"
    PREFIX /fonts
    BASE "${WINSHUN_DIR}/third_party/phosphor"
    FILES "${WINSHUN_DIR}/third_party/phosphor/Phosphor.ttf"
          "${WINSHUN_DIR}/third_party/phosphor/Phosphor-Fill.ttf")

target_link_libraries(BinaryLabApp PRIVATE
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickShapes
    user32 gdi32 dwmapi advapi32 ole32 shell32 shlwapi uxtheme comctl32 imm32 shcore)
set_target_properties(BinaryLabApp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(BinaryLabApp PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_SOURCE_DIR}")
endforeach()

if(MSVC)
    target_compile_options(BinaryLabApp PRIVATE /utf-8 /W3)
endif()

# Pure-logic tests: logic.h and formats.h have no OS dependency, so they build and run on any
# C++17 compiler without starting a plugin process.
add_executable(binarylab_tests tests/test_logic.cpp)
target_include_directories(binarylab_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_compile_definitions(binarylab_tests PRIVATE BINLAB_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures")
target_include_directories(binarylab_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party/tomlplusplus")
if(MSVC)
    target_compile_options(binarylab_tests PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(binarylab_tests PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra -O2)
endif()
set_target_properties(binarylab_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(binarylab_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/tests")
endforeach()

enable_testing()
add_test(NAME binarylab_logic COMMAND binarylab_tests)
