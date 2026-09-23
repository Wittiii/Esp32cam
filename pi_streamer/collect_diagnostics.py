"""Export local camera logs and bounded system diagnostics without reading config."""

import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import subprocess
import tempfile


TAIL_BYTES = 256 * 1024
COMMANDS = (
    ("uptime",),
    ("free", "-h"),
    ("df", "-h"),
    ("ip", "-brief", "address"),
    ("iw", "dev", "wlan0", "link"),
    ("vcgencmd", "get_throttled"),
    ("vcgencmd", "measure_temp"),
    ("systemctl", "status", "pi-cam-controller", "--no-pager", "-l"),
    ("systemctl", "show", "pi-cam-controller", "-p", "NRestarts", "-p", "Result",
     "-p", "ExecMainStatus"),
    ("journalctl", "--list-boots", "--no-pager"),
) + tuple(
    ("journalctl", "-b", boot, "--no-pager", "-n", "400", "-o", "short-iso") + scope
    for boot in ("0", "-1")
    for scope in (("-u", "pi-cam-controller"), ("-k",), ("-p", "warning"))
)


def read_tail(handle) -> str:
    handle.seek(0, os.SEEK_END)
    size = handle.tell()
    handle.seek(max(0, size - TAIL_BYTES))
    return ("[earlier output omitted]\n" if size > TAIL_BYTES else "") + handle.read().decode("utf-8", errors="replace")


def command_output(command) -> str:
    # File-backed capture avoids holding arbitrarily long journal lines in RAM.
    with tempfile.TemporaryFile() as capture:
        try:
            result = subprocess.run(command, stdout=capture, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, timeout=10, check=False,
                                    env={**os.environ, "LC_ALL": "C", "SYSTEMD_COLORS": "0"})
            status = f"Exit code: {result.returncode}\n"
        except (OSError, subprocess.TimeoutExpired) as exc:
            status = f"Unavailable or timed out: {exc}\n"
        return status + read_tail(capture)


def collect(log_dir: Path, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    # Exclusive creation prevents accidental overwrites and follows no existing link.
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as report:
        report.write(f"Pi camera diagnostics: {datetime.now(timezone.utc).isoformat()}\n")
        report.write("No config files are included. Logs may contain network addresses and device names.\n")
        for name in [f"controller.log.{i}" for i in range(4, 0, -1)] + ["controller.log"]:
            path = log_dir / name
            report.write(f"\n===== {name} (last {TAIL_BYTES} bytes) =====\n")
            try:
                if path.is_symlink():
                    report.write("Skipped symbolic link\n")
                    continue
                with path.open("rb") as source:
                    report.write(read_tail(source))
            except OSError as exc:
                report.write(f"Unavailable: {exc}\n")
        for command in COMMANDS:
            report.write(f"\n===== {' '.join(command)} =====\n")
            report.write(command_output(command))


def main() -> None:
    base = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log-dir", type=Path, default=base / "logs")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output = args.output or base / "diagnostics" / f"pi-diagnostics-{stamp}.txt"
    collect(args.log_dir, output)
    print(f"Diagnostic report: {output.resolve()}")


if __name__ == "__main__":
    main()
