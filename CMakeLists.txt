cmake_minimum_required(VERSION 3.20)
project(s9x_mss_gui LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /W3 /EHsc)
    add_compile_definitions(UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS MINIZ_NO_TIME)
endif()

# WIN32 here makes it a /SUBSYSTEM:WINDOWS app (no console window).
add_executable(s9x_mss_gui WIN32
    gui.cpp
    convert.cpp
    s9x_format.cpp
    mss_format.cpp
    miniz.c
    s9x_mss.h
    miniz.h
)

# Tell MSVC's CRT not to warn about std::ptrdiff_t / fopen / etc.
if(MSVC)
    target_compile_definitions(s9x_mss_gui PRIVATE _CRT_NONSTDC_NO_DEPRECATE)
endif()

target_link_libraries(s9x_mss_gui PRIVATE
    user32 gdi32 shell32 comdlg32 comctl32 ole32
)

# 64-bit builds get a "-x64" suffix; 32-bit keeps the plain name.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(s9x_mss_gui PROPERTIES OUTPUT_NAME "s9x_mss-x64")
else()
    set_target_properties(s9x_mss_gui PROPERTIES OUTPUT_NAME "s9x_mss")
endif()
