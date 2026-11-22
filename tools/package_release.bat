@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0.."
set "ROOT=%CD%"

:: Optional version tag argument (e.g. tools\package_release.bat v1.0.0)
set "VER=%~1"
if "%VER%"=="" (
    for /f %%I in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd"') do set "VER=%%I"
)

set "DIST_ROOT=%ROOT%\dist"
set "STAGE=%DIST_ROOT%\BtVS_recomp_%VER%"
set "ZIP_PATH=%DIST_ROOT%\BtVS_recomp_%VER%_win32.zip"

echo ====================================================================
echo  BtVS Windows Recomp - GitHub Release Packager (%VER%)
echo ====================================================================
echo.

taskkill /F /IM BuffyWin32Recomp.exe >nul 2>&1
if not exist "%ROOT%\recomp\build" mkdir "%ROOT%\recomp\build"

:: 1. Build fresh Release x86 Runtime DLL & Fixed-Base Launcher
set "VSCMD_START_DIR=%ROOT%"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%ROOT%"

echo [1/4] Building BuffyRecompRuntime.dll (32-bit x86 @ 0x10000000)...
cl.exe /nologo /std:c++17 /EHsc /O2 /LD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"recomp/include" ^
    recomp/src/win_main.cpp recomp/src/xboxkrnl_win32.cpp recomp/src/slate_engine.cpp ^
    recomp/src/xbe_loader.cpp recomp/src/d3d9_bridge.cpp recomp/src/dsound_bridge.cpp ^
    recomp/src/config_ui.cpp ^
    /Fo"recomp/build/" /Fe"recomp/build/BuffyRecompRuntime.dll" ^
    user32.lib gdi32.lib winmm.lib d3d9.lib gdiplus.lib ole32.lib strmiids.lib shell32.lib dwmapi.lib advapi32.lib ^
    /link /BASE:0x10000000 /DYNAMICBASE:NO
if errorlevel 1 (
    echo [ERROR] Runtime DLL build failed!
    exit /b 1
)

echo [2/4] Building fixed-base launcher BuffyWin32Recomp.exe...
cl.exe /nologo /std:c++17 /O2 /GS- recomp/src/launcher_main.cpp ^
    /Fo"recomp/build/" /Fe"recomp/build/BuffyWin32Recomp.exe" kernel32.lib ^
    /link /NODEFAULTLIB /ENTRY:BuffyLauncherEntry /BASE:0x00010000 /FIXED /DYNAMICBASE:NO /SUBSYSTEM:CONSOLE /STACK:0x00800000,0x00800000
if errorlevel 1 (
    echo [ERROR] Launcher build failed!
    exit /b 1
)

:: 2. Stage clean release directory (zero copyrighted game files!)
echo [3/4] Staging clean release package...
if exist "%STAGE%" rmdir /S /Q "%STAGE%"
if exist "%ZIP_PATH%" del /F /Q "%ZIP_PATH%"

mkdir "%STAGE%\recomp\build"
mkdir "%STAGE%\tools"
mkdir "%STAGE%\recomp\hd_textures\costume_red_leather"
mkdir "%STAGE%\recomp\hd_textures\costume_silver_club"
mkdir "%STAGE%\recomp\hd_textures\costume_black_dress"
mkdir "%STAGE%\recomp\hd_textures\costume_lilac_wrap"

copy /Y "%ROOT%\recomp\build\BuffyWin32Recomp.exe"   "%STAGE%\recomp\build\" >nul
copy /Y "%ROOT%\recomp\build\BuffyRecompRuntime.dll" "%STAGE%\recomp\build\" >nul
copy /Y "%ROOT%\tools\extract_xiso.py"               "%STAGE%\tools\" >nul
copy /Y "%ROOT%\tools\upscale_textures.py"           "%STAGE%\tools\" >nul
copy /Y "%ROOT%\README.md"                           "%STAGE%\" >nul

:: Copy ONLY the 8 custom HD costume PNG textures (never preview.jpg or dumped Xbox textures)
for %%D in (costume_red_leather costume_silver_club costume_black_dress costume_lilac_wrap) do (
    if exist "%ROOT%\recomp\hd_textures\%%D\*.png" (
        copy /Y "%ROOT%\recomp\hd_textures\%%D\*.png" "%STAGE%\recomp\hd_textures\%%D\" >nul
    )
)

:: Generate convenient root launcher Play_Buffy.bat for end-users
(
    echo @echo off
    echo cd /d "%%~dp0"
    echo if not exist "extracted\Default.xbe" ^(
    echo     echo ====================================================================
    echo     echo  Missing extracted\Default.xbe!
    echo     echo  Place your legal Buffy the Vampire Slayer ^(Europe^) Xbox XISO here
    echo     echo  and extract it first:
    echo     echo.
    echo     echo    python tools\extract_xiso.py "Buffy the Vampire Slayer (Europe).xiso.iso" extracted
    echo     echo ====================================================================
    echo     pause
    echo     exit /b 1
    echo ^)
    echo "recomp\build\BuffyWin32Recomp.exe" %%*
) > "%STAGE%\Play_Buffy.bat"

:: 3. Compress to GitHub Release ZIP & audit contents
echo [4/4] Creating ZIP archive "%ZIP_PATH%"...
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP_PATH%' -Force"
if errorlevel 1 (
    echo [ERROR] Failed to create ZIP archive!
    exit /b 1
)

echo.
echo [OK] Release archive created successfully:
echo      %ZIP_PATH%
echo.
echo Included files:
powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; $z=[IO.Compression.ZipFile]::OpenRead('%ZIP_PATH%'); $z.Entries | ForEach-Object { '{0,10:N0}  {1}' -f $_.Length, $_.FullName }; $z.Dispose()"
echo.
echo Ready to upload to GitHub Releases!
pause
