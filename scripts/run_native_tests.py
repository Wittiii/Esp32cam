"""Build host-side firmware regression tests without contacting any device."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    compiler = os.environ.get("CXX") or shutil.which("g++")
    if not compiler:
        raise SystemExit("A C++17 compiler is required (set CXX or install g++).")
    with tempfile.TemporaryDirectory(prefix="camera-native-tests-") as temporary:
        extension = ".exe" if os.name == "nt" else ""
        cases = {
            "rtsp": ['-DMICRO_RTSP_PLATFORM_HEADER="test_transport.h"',
                     "-Itest/native_rtsp", "-Ilib/micro_rtsp", "test/native_rtsp/test_rtsp.cpp",
                     "lib/micro_rtsp/CStreamer.cpp", "lib/micro_rtsp/CRtspSession.cpp"],
            "victron": ["-Itest/native/victron_stubs", "-Iinclude", "test/native/test_victron.cpp"],
            "control_payload": ["-Iinclude", "test/native/test_control_payload.cpp"],
            "server_capture": ["-Itest/native/capture_stubs", "-Iinclude", "test/native/test_server_capture.cpp",
                               "src/dfr1154_server_capture.cpp"],
        }
        for name, arguments in cases.items():
            binary = str(Path(temporary) / (name + extension))
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-O1", *arguments, "-o", binary], cwd=ROOT, check=True)
            result = subprocess.run([binary], cwd=ROOT, check=True, capture_output=True, text=True, timeout=15)
            print(name + ": " + result.stdout.strip().splitlines()[-1])

if __name__ == "__main__":
    main()
