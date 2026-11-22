@echo off
cd /d "%~dp0.."
set "ROOT=%CD%"
taskkill /F /IM BuffyWin32Recomp.exe >nul 2>&1
if not exist "%ROOT%\recomp\build" mkdir "%ROOT%\recomp\build"

set "VSCMD_START_DIR=%ROOT%"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%ROOT%"

echo [1/3] Compiling BuffyRecompRuntime.dll (32-bit x86 @ 0x10000000)...
cl.exe /nologo /std:c++17 /EHsc /O2 /LD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"recomp/include" recomp/src/win_main.cpp recomp/src/xboxkrnl_win32.cpp recomp/src/slate_engine.cpp recomp/src/xbe_loader.cpp recomp/src/d3d9_bridge.cpp recomp/src/dsound_bridge.cpp recomp/src/config_ui.cpp /Fo"recomp/build/" /Fe"recomp/build/BuffyRecompRuntime.dll" user32.lib gdi32.lib winmm.lib d3d9.lib gdiplus.lib ole32.lib strmiids.lib shell32.lib dwmapi.lib advapi32.lib /link /BASE:0x10000000 /DYNAMICBASE:NO
if errorlevel 1 (
    echo [ERROR] Runtime DLL compilation failed!
    pause
    exit /b 1
)

if exist "recomp\build\BuffyWin32Recomp.exe" goto skip_launcher
echo [2/3] Compiling launcher BuffyWin32Recomp.exe...
cl.exe /nologo /std:c++17 /O2 /GS- recomp/src/launcher_main.cpp /Fo"recomp/build/" /Fe"recomp/build/BuffyWin32Recomp.exe" kernel32.lib /link /NODEFAULTLIB /ENTRY:BuffyLauncherEntry /BASE:0x00010000 /FIXED /DYNAMICBASE:NO /SUBSYSTEM:CONSOLE /STACK:0x00800000,0x00080000
if errorlevel 1 (
    echo [ERROR] Launcher compilation failed!
    pause
    exit /b 1
)
goto launch_game

:skip_launcher
echo [2/3] Using existing launcher recomp\build\BuffyWin32Recomp.exe

:launch_game

echo [3/3] Launching BuffyWin32Recomp.exe...
"recomp\build\BuffyWin32Recomp.exe" %*
echo.
echo Exit code: %errorlevel%
echo Logs: "%ROOT%\buffy_recomp.log"
pause
