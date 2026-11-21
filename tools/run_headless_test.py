"""Run BuffyWin32Recomp.exe --headless with a timeout, then print exit code, stdout and the log tail."""
import os
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
exe = os.path.join(root, "recomp", "build", "BuffyWin32Recomp.exe")
timeout = int(sys.argv[1]) if len(sys.argv) > 1 else 20

try:
    r = subprocess.run([exe, "--headless"], cwd=root, capture_output=True, timeout=timeout)
    print(f"Exit code: 0x{r.returncode & 0xFFFFFFFF:08X}")
    out = r.stdout
except subprocess.TimeoutExpired as e:
    print(f"Still running after {timeout}s (killed) - game loop is alive.")
    out = e.stdout or b""
except OSError as e:
    print(f"CreateProcess failed: {e}")
    out = b""

print("---- stdout (tail) ----")
print(out.decode("utf-8", errors="replace")[-6000:])

log = os.path.join(root, "buffy_recomp.log")
if os.path.exists(log):
    print("---- buffy_recomp.log (tail) ----")
    with open(log, "rb") as f:
        print(f.read().decode("utf-8", errors="replace")[-6000:])
