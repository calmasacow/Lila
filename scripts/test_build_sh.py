"""Run with python3 scripts/test_build_sh.py; no build dependencies required."""

import os
from pathlib import Path
import subprocess
import tempfile


root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as scratch:
    scratch = Path(scratch)
    cmake = scratch / "cmake"
    cmake.write_text('#!/bin/sh\nprintf "%s\\n" "$PWD" "$@" >> "$CALLS"\n')
    cmake.chmod(0o755)
    calls = scratch / "calls"
    env = os.environ | {
        "PATH": f"{scratch}:{os.environ['PATH']}",
        "CALLS": str(calls),
        "VCPKG_ROOT": str(scratch),
        "LILA_PYTHON_DIR": str(scratch),
    }
    for argument in ("build-test-path", str(scratch / "absolute build")):
        directory = Path(argument)
        if not directory.is_absolute():
            directory = root / directory
        subprocess.run(["bash", str(root / "build.sh"), argument, "Release"],
                       cwd=scratch, env=env, check=True)
        assert calls.read_text().splitlines() == [
            str(root), "--preset", "linux", "-S", str(root), "-B", str(directory),
            "-DLILA_USD=OFF", str(root), "--build", str(directory), "--config", "Release",
        ]
        calls.unlink()
    directory = scratch / "absolute build"
    directory.mkdir()
    (directory / "build.ninja").touch()
    subprocess.run(["bash", str(root / "build.sh"), str(directory)],
                   cwd=scratch, env=env, check=True)
    assert calls.read_text().splitlines() == [
        str(root), "--build", str(directory), "--config", "RelWithDebInfo",
    ]
print("build.sh path checks passed")
