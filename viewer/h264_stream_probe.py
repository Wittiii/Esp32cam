import argparse
import shutil
import socket
import subprocess
import sys
import time


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise SystemExit(
            f"{name} not found in PATH. Install FFmpeg and make sure {name}.exe is available."
        )
    return path


def wait_for_client(host: str, port: int) -> tuple[socket.socket, tuple[str, int]]:
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((host, port))
    server.listen(1)
    print(f"[TCP] waiting for H.264 sender on {host}:{port}")
    client, address = server.accept()
    server.close()
    print(f"[TCP] connected: {address[0]}:{address[1]}")
    return client, address


def launch_ffplay(ffplay: str, host: str, port: int, probe_size: str) -> subprocess.Popen:
    command = [
        ffplay,
        "-fflags",
        "nobuffer",
        "-flags",
        "low_delay",
        "-framedrop",
        "-strict",
        "experimental",
        "-probesize",
        probe_size,
        "-analyzeduration",
        "0",
        f"tcp://{host}:{port}?listen=1",
    ]
    print("[FFPLAY] " + " ".join(command))
    return subprocess.Popen(command)


def probe_stream(ffprobe: str, host: str, port: int, timeout: float) -> int:
    command = [
        ffprobe,
        "-v",
        "error",
        "-show_streams",
        "-show_format",
        "-of",
        "compact",
        f"tcp://{host}:{port}?listen=1",
    ]
    print("[FFPROBE] waiting for stream metadata...")
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        print("[FFPROBE] timed out while waiting for stream")
        return 1

    if completed.stdout:
        print(completed.stdout.strip())
    if completed.stderr:
        print(completed.stderr.strip(), file=sys.stderr)
    return completed.returncode


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Receive and inspect a raw H.264 TCP stream from a Raspberry Pi."
    )
    parser.add_argument("--host", default="0.0.0.0", help="Local bind address")
    parser.add_argument("--port", type=int, default=5006, help="TCP port for the incoming stream")
    parser.add_argument(
        "--mode",
        choices=("play", "probe"),
        default="play",
        help="play opens ffplay, probe prints stream metadata with ffprobe",
    )
    parser.add_argument(
        "--probe-size",
        default="32",
        help="FFplay probe size in bytes; increase if the decoder fails to lock on",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=15.0,
        help="Timeout in seconds for probe mode",
    )
    args = parser.parse_args()

    if args.mode == "play":
        ffplay = require_tool("ffplay")
        process = launch_ffplay(ffplay, args.host, args.port, args.probe_size)
        try:
            process.wait()
        except KeyboardInterrupt:
            process.terminate()
    else:
        ffprobe = require_tool("ffprobe")
        sys.exit(probe_stream(ffprobe, args.host, args.port, args.timeout))


if __name__ == "__main__":
    main()
