@echo off
rem Build the GUI with MSVC. Run from a Developer Command Prompt for VS 2022
rem (or call vcvars32.bat / vcvars64.bat first).
rem
rem Output name follows the target arch:
rem   x64  -> s9x_mss-x64.exe
rem   x86  -> s9x_mss.exe
rem
rem If you prefer to use the IDE, open s9x_mss.sln in Visual Studio 2022.

setlocal
rem VSCMD_ARG_TGT_ARCH is set by the vcvars* scripts: x64 / x86 / arm64 / etc.
if /I "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    set OUT=s9x_mss-x64.exe
) else (
    set OUT=s9x_mss.exe
)
set SRC=gui.cpp convert.cpp s9x_format.cpp mss_format.cpp miniz.c

where cl.exe >nul 2>&1
if errorlevel 1 (
    echo cl.exe not found on PATH. Open a "Developer Command Prompt for VS 2022"
    echo and re-run this script, or call vcvars64.bat manually.
    exit /b 1
)

cl /nologo /EHsc /std:c++17 /W3 /O2 /utf-8 /DUNICODE /D_UNICODE ^
   /DMINIZ_NO_TIME ^
   /Fe:%OUT% %SRC% ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib comdlg32.lib comctl32.lib ole32.lib

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)
echo.
echo Built %OUT%
endlocal
