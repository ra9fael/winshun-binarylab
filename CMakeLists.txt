cmake_minimum_required(VERSION 3.21)
project(binarylab LANGUAGES CXX)

set(BINARYLAB_OUTPUT_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH "Plugin runtime output folder")
file(READ "${CMAKE_SOURCE_DIR}/plugin.json" binarylab_manifest)
string(JSON BINARYLAB_VERSION GET "${binarylab_manifest}" version)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The generated C++ template builds the exe straight into the plugin folder,
# because plugin.json names it as runtime.program. Keep that convention.
add_executable(binarylab main.cpp binarylab.rc)
target_compile_definitions(binarylab PRIVATE BINARYLAB_VERSION="${BINARYLAB_VERSION}")
target_compile_definitions(binarylab PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# The v2 editor window: user32 (windows, DPI) and gdi32 (painting).
target_link_libraries(binarylab PRIVATE user32 gdi32)
# toml++ (record format files) is vendored; SYSTEM keeps its warnings out of /W4 and -Wall.
target_include_directories(binarylab SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party/tomlplusplus")

if(MSVC)
    target_compile_options(binarylab PRIVATE /utf-8 /W4 /permissive-)
    # Static CRT: the exe needs no runtime DLL, which the strict sandbox cannot load.
    set_target_properties(binarylab PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(binarylab PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra -O2)
    target_link_options(binarylab PRIVATE -static -static-libgcc -static-libstdc++)
endif()

set_target_properties(binarylab PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BINARYLAB_OUTPUT_DIR}")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(binarylab PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${BINARYLAB_OUTPUT_DIR}")
endforeach()

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

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME binarylab_package COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_package.py")
