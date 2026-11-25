@echo off
cd /d "%~dp0.."
set "ROOT=%CD%"
taskkill /F /IM BtVSRecompUWP.exe >nul 2>&1
if not exist "%ROOT%\uwp_recomp\build" mkdir "%ROOT%\uwp_recomp\build"
if not exist "%ROOT%\uwp_recomp\build\gen_obj" mkdir "%ROOT%\uwp_recomp\build\gen_obj"

set "VSCMD_START_DIR=%ROOT%"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%ROOT%"

set "DO_FULL=0"
if "%~1"=="--full" set "DO_FULL=1"

if "%DO_FULL%"=="1" (
    echo [1/4] Lifting ALL SlateEngine x86 functions to portable x64 C++...
    del /Q "%ROOT%\uwp_recomp\src\generated\recomp_chunk_*.cpp" >nul 2>&1
    del /Q "%ROOT%\uwp_recomp\build\gen_obj\*.obj" >nul 2>&1
    python uwp_recomp/tools/lift_xbe_to_cpp.py --all --chunk-size 1000
    if errorlevel 1 (
        echo [ERROR] Full x86->C++ lift failed!
        pause
        exit /b 1
    )
) else if not exist "%ROOT%\uwp_recomp\src\generated\recomp_decls.h" (
    echo [1/4] Lifting initial 200 SlateEngine x86 functions...
    python uwp_recomp/tools/lift_xbe_to_cpp.py --limit 200
    if errorlevel 1 (
        echo [ERROR] x86->C++ lifter failed!
        pause
        exit /b 1
    )
)

:: Compile lifted chunks ONLY when gen_obj cache is empty (saves ~4 minutes on incremental host edits!)
if not exist "%ROOT%\uwp_recomp\build\gen_obj\recomp_register.obj" (
    echo [2/4] Compiling lifted C++ chunks into cached .obj files [/MP /bigobj]...
    cl.exe /nologo /c /MP /bigobj /std:c++17 /EHsc /O2 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
        /I"uwp_recomp/include" /I"uwp_recomp/src" ^
        uwp_recomp/src/generated/recomp_register.cpp uwp_recomp/src/generated/recomp_chunk_*.cpp ^
        /Fo"uwp_recomp/build/gen_obj/"
    if errorlevel 1 (
        echo [ERROR] Lifted chunk compilation failed!
        pause
        exit /b 1
    )
) else (
    echo [2/4] Reusing cached lifted .obj files in uwp_recomp\build\gen_obj\ [pass --full to rebuild chunks]
)

echo [3/4] Compiling host runtime and linking BtVSRecompUWP.exe (64-bit x64)...
cl.exe /nologo /MP /std:c++17 /EHsc /O2 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /I"uwp_recomp/include" /I"uwp_recomp/src" ^
    uwp_recomp/src/uwp_main.cpp uwp_recomp/src/x86_cpu.cpp uwp_recomp/src/d3d11_bridge.cpp ^
    uwp_recomp/src/xaudio2_bridge.cpp uwp_recomp/src/host_bridges.cpp ^
    "uwp_recomp\build\gen_obj\*.obj" ^
    /Fo"uwp_recomp/build/" /Fe"uwp_recomp/build/BtVSRecompUWP.exe" ^
    d3d11.lib dxgi.lib d3dcompiler.lib xaudio2.lib ole32.lib user32.lib
if errorlevel 1 (
    echo [ERROR] Host runtime build/link failed!
    pause
    exit /b 1
)

echo [4/4] Running BtVSRecompUWP.exe (logging to uwp_recomp\uwp_recomp.log)...
"%ROOT%\uwp_recomp\build\BtVSRecompUWP.exe" %* > "%ROOT%\uwp_recomp\uwp_recomp.log" 2>&1
pause
