@echo off
cd /d "%~dp0.."
set "ROOT=%CD%"
set "VSCMD_START_DIR=%ROOT%"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%ROOT%"
set "TEMP=%ROOT%\recomp\build"
set "TMP=%ROOT%\recomp\build"
cl.exe /nologo /std:c++17 /EHsc /O2 /LD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"recomp/include" recomp/src/win_main.cpp recomp/src/xboxkrnl_win32.cpp recomp/src/slate_engine.cpp recomp/src/xbe_loader.cpp recomp/src/d3d9_bridge.cpp recomp/src/dsound_bridge.cpp recomp/src/config_ui.cpp /Fo"recomp/build/" /Fe"recomp/build/BuffyRecompRuntime.dll" user32.lib gdi32.lib winmm.lib d3d9.lib gdiplus.lib ole32.lib strmiids.lib shell32.lib dwmapi.lib advapi32.lib /link /BASE:0x10000000 /DYNAMICBASE:NO
exit /b %ERRORLEVEL%
