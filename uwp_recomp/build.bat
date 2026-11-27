@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0\.."

if not exist "extracted\Default.xbe" (
    echo [ERROR] extracted\Default.xbe not found!
    echo Please extract your PAL Buffy the Vampire Slayer Europe disc into extracted\ first.
    exit /b 1
)

:: 1. Locate Visual Studio x64 Build Tools via vswhere
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if not defined VSDIR (
    echo [ERROR] Visual Studio C++ x64 Build Tools not found!
    echo Install Visual Studio 2022/2026 Build Tools with "Desktop development with C++".
    exit /b 1
)
set "VSCMD_START_DIR=%CD%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0\.."

:: 2. Lift Default.xbe -> uwp_recomp\src\generated\ if not yet generated
if not exist "uwp_recomp\src\generated\recomp_register.cpp" (
    echo [1/3] Lifting extracted\Default.xbe to portable x64 C++...
    python uwp_recomp\tools\lift_xbe_to_cpp.py --all
    if errorlevel 1 exit /b 1
)

if not exist "uwp_recomp\build\gen_obj" mkdir "uwp_recomp\build\gen_obj"

:: 3. Compile lifted chunks with /MP /O2 (only when gen_obj is empty or --rebuild-all is passed)
set "NEED_CHUNKS=0"
if not exist "uwp_recomp\build\gen_obj\recomp_register.obj" set "NEED_CHUNKS=1"
if /i "%~1"=="--rebuild-all" set "NEED_CHUNKS=1"
if "!NEED_CHUNKS!"=="1" (
    echo [2/3] Compiling lifted game chunks with /MP /O2 - first build takes 1-2 min...
    cl.exe /nologo /c /MP /std:c++17 /EHsc /O2 /Ob2 /Oi /Ot /bigobj /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"uwp_recomp/include" /I"uwp_recomp/src" "uwp_recomp\src\generated\*.cpp" /Fo"uwp_recomp/build/gen_obj/"
    if errorlevel 1 exit /b 1
) else (
    for %%f in (uwp_recomp\src\generated\*.cpp) do (
        if not exist "uwp_recomp\build\gen_obj\%%~nf.obj" (
            echo [2/3] Recompiling updated chunk %%~nxf...
            cl.exe /nologo /c /std:c++17 /EHsc /O2 /Ob2 /Oi /Ot /bigobj /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"uwp_recomp/include" /I"uwp_recomp/src" "%%f" /Fo"uwp_recomp/build/gen_obj/"
            if errorlevel 1 exit /b 1
        )
    )
)

:: 4. Compile x64 D3D11 / XAudio2 runtime and link BtVSRecompUWP.exe
echo [3/3] Compiling x64 D3D11 runtime and linking BtVSRecompUWP.exe...
cl.exe /nologo /MP /std:c++17 /EHsc /O2 /Ob2 /Oi /Ot /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"uwp_recomp/include" /I"uwp_recomp/src" uwp_recomp/src/uwp_main.cpp uwp_recomp/src/x86_cpu.cpp uwp_recomp/src/d3d11_bridge.cpp uwp_recomp/src/host_bridges.cpp uwp_recomp/src/xaudio2_bridge.cpp uwp_recomp/src/uwp_config_ui.cpp "uwp_recomp\build\gen_obj\*.obj" /Fo"uwp_recomp/build/" /Fe"uwp_recomp/build/BtVSRecompUWP.exe" d3d11.lib dxgi.lib d3dcompiler.lib xaudio2.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib user32.lib /link /STACK:8388608
if errorlevel 1 exit /b 1

echo.
echo [OK] Built uwp_recomp\build\BtVSRecompUWP.exe!
echo Run with: uwp_recomp\build\BtVSRecompUWP.exe --boot-main
