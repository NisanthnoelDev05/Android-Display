@echo off
setlocal

echo Setting up Visual Studio 2026 Developer Command Prompt...
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" > nul

if exist AndroidDisplay.exe (
    del /f /q AndroidDisplay_old.exe 2>nul
    move /y AndroidDisplay.exe AndroidDisplay_old.exe 2>nul
)

echo Compiling resource file...
rc /nologo /fo resource.res resource.rc
if %ERRORLEVEL% NEQ 0 (
    echo Error compiling resource file!
    exit /b %ERRORLEVEL%
)

echo Compiling and linking Android Display...
cl /nologo /W4 /O2 /EHsc /std:c++20 /DUNICODE /D_UNICODE /DNOMINMAX ^
   src\Theme.cpp ^
   src\AdbClient.cpp ^
   src\MediaPipeline.cpp ^
   src\QrCode.cpp ^
   src\Dialogs.cpp ^
   src\main.cpp ^
   resource.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:AndroidDisplay.exe ^
   user32.lib gdi32.lib gdiplus.lib comctl32.lib dwmapi.lib uxtheme.lib advapi32.lib ^
   d2d1.lib d3d11.lib ws2_32.lib mfplat.lib mfreadwrite.lib mfuuid.lib mf.lib wmcodecdspuuid.lib winmm.lib ole32.lib shell32.lib

if %ERRORLEVEL% NEQ 0 (
    echo Compilation failed!
    exit /b %ERRORLEVEL%
)

del /f /q *.obj resource.res AndroidDisplay_old.exe 2>nul

echo Build succeeded! Output: AndroidDisplay.exe
