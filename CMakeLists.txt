cmake_minimum_required(VERSION 3.20)
project(AndroidDisplay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(AndroidDisplay WIN32
    src/Theme.cpp
    src/AdbClient.cpp
    src/MediaPipeline.cpp
    src/QrCode.cpp
    src/Dialogs.cpp
    src/main.cpp
    resource.rc
)

target_compile_definitions(AndroidDisplay PRIVATE UNICODE _UNICODE NOMINMAX)

target_link_libraries(AndroidDisplay PRIVATE
    user32
    gdi32
    gdiplus
    comctl32
    dwmapi
    uxtheme
    advapi32
    d2d1
    d3d11
    ws2_32
    mfplat
    mfreadwrite
    mfuuid
    mf
    wmcodecdspuuid
    winmm
    ole32
    shell32
)
