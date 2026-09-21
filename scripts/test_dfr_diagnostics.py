"""Test diagnostic state/JSON on the host without hardware or a broker."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    compiler = os.environ.get("CXX") or shutil.which("g++")
    if not compiler:
        raise SystemExit("A C++11 compiler is required (set CXX or install g++).")
    with tempfile.TemporaryDirectory(prefix="dfr-diagnostics-test-") as temporary:
        binary = Path(temporary) / ("diagnostics.exe" if os.name == "nt" else "diagnostics")
        subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra",
                        "-Itest/native/diagnostics_stubs", "-Iinclude",
                        "test/native/test_dfr_diagnostics.cpp", "-o", str(binary)],
                       cwd=ROOT, check=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
        lines = result.stdout.splitlines()
        for line in lines[:-1]:
            json.loads(line)
        print(lines[-1])
        print(f"JSON valid; test boot report: {len(lines[0])} bytes (server limit 1024)")


if __name__ == "__main__":
    main()
