cmake_minimum_required(VERSION 3.20)
project(xsvViewer VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: the DLL must not depend on a VC++ redistributable being present.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_options(/utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# Pure logic. Nothing in here may touch Win32 or Notepad++, so it stays testable.
add_library(xsvcore STATIC
    src/core/Dialect.cpp
    src/core/Export.cpp
    src/core/Filter.cpp
    src/core/Ini.cpp
    src/core/Options.cpp
    src/core/Table.cpp
    src/core/Text.cpp
    src/core/View.cpp
)
target_include_directories(xsvcore PUBLIC src/core)

add_executable(xsvtests tests/test_core.cpp)
target_link_libraries(xsvtests PRIVATE xsvcore)

if(EXISTS ${CMAKE_SOURCE_DIR}/src/plugin/PluginMain.cpp)
    add_library(xsvViewer SHARED
        src/plugin/Columns.cpp
        src/plugin/Document.cpp
        src/plugin/Grid.cpp
        src/plugin/OptionsDialog.cpp
        src/plugin/PluginMain.cpp
        src/plugin/TableHost.cpp
        src/plugin/xsvViewer.rc
    )
    target_include_directories(xsvViewer PRIVATE src/plugin src/npp)
    target_link_libraries(xsvViewer PRIVATE xsvcore comctl32 comdlg32 uxtheme shell32)
    target_link_options(xsvViewer PRIVATE
        "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")

    # Only ever copied into the portable Notepad++ the GUI tests run against.
    add_library(xsvTestShim SHARED tests/gui/TestShim.cpp)
    target_include_directories(xsvTestShim PRIVATE src/npp)
endif()
