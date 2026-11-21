import os
import subprocess
import sys

VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat"


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    os.makedirs(os.path.join(root, "recomp", "build"), exist_ok=True)
    # vcvars32.bat changes the working directory unless VSCMD_START_DIR is set
    os.environ["VSCMD_START_DIR"] = root
    os.environ["VSCMD_SKIP_SENDTELEMETRY"] = "1"

    pre = f'call "{VCVARS}" >nul && cd /d "{root}" && '

    dll_cmd = pre + (
        'cl.exe /nologo /std:c++17 /EHsc /O2 /LD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS '
        '/I"recomp/include" '
        'recomp/src/win_main.cpp recomp/src/xboxkrnl_win32.cpp recomp/src/slate_engine.cpp recomp/src/xbe_loader.cpp recomp/src/d3d9_bridge.cpp '
        '/Fo"recomp/build/" /Fe"recomp/build/BuffyRecompRuntime.dll" '
        'user32.lib gdi32.lib winmm.lib d3d9.lib /link /BASE:0x10000000 /DYNAMICBASE:NO'
    )
    exe_cmd = pre + (
        'cl.exe /nologo /std:c++17 /O2 /GS- recomp/src/launcher_main.cpp '
        '/Fo"recomp/build/" /Fe"recomp/build/BuffyWin32Recomp.exe" kernel32.lib '
        '/link /NODEFAULTLIB /ENTRY:BuffyLauncherEntry /BASE:0x00010000 /FIXED /DYNAMICBASE:NO /SUBSYSTEM:CONSOLE /STACK:0x00800000,0x00080000'
    )

    print("[1/2] Building BuffyRecompRuntime.dll (x86 @ 0x10000000)...", flush=True)
    subprocess.check_call(dll_cmd, shell=True, cwd=root)
    print("[2/2] Building BuffyWin32Recomp.exe (fixed base 0x00010000)...", flush=True)
    subprocess.check_call(exe_cmd, shell=True, cwd=root)
    print("Build succeeded: recomp/build/BuffyWin32Recomp.exe + recomp/build/BuffyRecompRuntime.dll")


if __name__ == "__main__":
    main()
