"""Build the standalone Windows executable.

    pip install pyinstaller bleak
    python build_exe.py

Produces `dist/EL15 Load Control.exe` — one file, no Python needed on the
machine that runs it.

Notes for anyone changing this:

  - `--paths ../el15_bench` is required. The app imports the protocol client and
    the sweep engine from there at runtime via sys.path; PyInstaller cannot see
    that by static analysis, so without this the build succeeds and the app dies
    on launch with ModuleNotFoundError.
  - `--windowed` means the frozen app has no console: `sys.stdout` is None and
    `print` becomes a no-op. That is why the self-checks also write
    `el15_check.log` beside the executable — otherwise a frozen build can only
    be tested by looking at it.
  - onefile spawns the real app as a CHILD of the bootloader process. Anything
    that inspects "the process" — a window enumeration, a task manager kill —
    has to account for two PIDs, not one.
"""
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
NAME = "EL15 Load Control"


def main():
    try:
        import PyInstaller  # noqa: F401
    except ImportError:
        print("pyinstaller is not installed:  pip install pyinstaller bleak")
        return 1
    for stale in (HERE / "build", HERE / "dist"):
        shutil.rmtree(stale, ignore_errors=True)
    cmd = [
        sys.executable, "-m", "PyInstaller", "--noconfirm", "--onefile", "--windowed",
        "--name", NAME,
        "--paths", str(HERE.parent / "el15_bench"),
        "--hidden-import", "bleak",
        "--distpath", str(HERE / "dist"),
        "--workpath", str(HERE / "build"),
        "--specpath", str(HERE / "build"),
        str(HERE / "el15_desktop.py"),
    ]
    print(" ".join(cmd))
    r = subprocess.run(cmd)
    if r.returncode:
        return r.returncode
    exe = HERE / "dist" / ("%s.exe" % NAME)
    if not exe.exists():
        print("build reported success but produced no executable")
        return 1
    print("\nbuilt %s (%.1f MB)" % (exe, exe.stat().st_size / 1e6))
    print("check it with:  \"%s\" --check" % exe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
