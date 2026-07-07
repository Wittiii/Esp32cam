import argparse
import socket
import struct
import time

import cv2
import numpy as np


HEADER_JPEG = b"JPEG"
HEADER_STATUS = b"STAT"
HEADER_SIZE = 8
MAX_PACKET_SIZE = 2_000_000
WINDOW_NAME = "ESP32-CAM TCP Viewer"
STATUS_LINES = 8


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("socket closed")
        data.extend(chunk)
    return bytes(data)


def append_status(status_lines: list[str], message: str) -> None:
    status_lines.append(message)
    del status_lines[:-STATUS_LINES]


def render_frame(frame: np.ndarray | None, fps: float, status_lines: list[str]) -> np.ndarray:
    if frame is None:
        output = np.zeros((480, 640, 3), dtype=np.uint8)
        cv2.putText(
            output,
            "Waiting for JPEG frames...",
            (20, 45),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.8,
            (0, 255, 255),
            2,
            cv2.LINE_AA,
        )
    else:
        output = frame.copy()

    height, width = output.shape[:2]
    overlay_top = max(height - 175, 0)

    cv2.rectangle(output, (0, overlay_top), (width, height), (0, 0, 0), -1)
    cv2.putText(
        output,
        f"FPS: {fps:4.1f}",
        (10, 30),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.8,
        (0, 255, 0),
        2,
        cv2.LINE_AA,
    )

    y = overlay_top + 22
    for line in status_lines:
        cv2.putText(
            output,
            line[:95],
            (10, y),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.5,
            (255, 255, 255),
            1,
            cv2.LINE_AA,
        )
        y += 20

    return output


def receive_packet(sock: socket.socket) -> tuple[bytes, bytes]:
    header = recv_exact(sock, HEADER_SIZE)
    packet_type = header[:4]
    packet_size = struct.unpack("!I", header[4:])[0]

    if packet_size <= 0 or packet_size > MAX_PACKET_SIZE:
        raise ValueError(f"invalid packet size: {packet_size}")

    payload = recv_exact(sock, packet_size)
    return packet_type, payload


def decode_frame(payload: bytes) -> np.ndarray:
    frame = cv2.imdecode(np.frombuffer(payload, dtype=np.uint8), cv2.IMREAD_COLOR)
    if frame is None:
        raise ValueError("JPEG decode failed")
    return frame


def handle_client(client: socket.socket, address: tuple[str, int]) -> bool:
    print(f"[TCP] client connected: {address[0]}:{address[1]}")

    frames = 0
    last_fps_update = time.monotonic()
    fps = 0.0
    status_lines = ["TCP connected"]
    last_raw_frame: np.ndarray | None = None
    cv2.imshow(WINDOW_NAME, render_frame(last_raw_frame, fps, status_lines))

    while True:
        packet_type, payload = receive_packet(client)

        if packet_type == HEADER_JPEG:
            last_raw_frame = decode_frame(payload)
            frames += 1

            now = time.monotonic()
            elapsed = now - last_fps_update
            if elapsed >= 1.0:
                fps = frames / elapsed
                frames = 0
                last_fps_update = now
        elif packet_type == HEADER_STATUS:
            status_text = payload.decode("utf-8", errors="replace")
            print(f"[STAT] {status_text}")
            append_status(status_lines, status_text)
        else:
            raise ValueError(f"unexpected packet header: {packet_type!r}")

        cv2.imshow(WINDOW_NAME, render_frame(last_raw_frame, fps, status_lines))

        key = cv2.waitKey(1) & 0xFF
        if key in (27, ord("q")):
            return False


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Receive and display ESP32-CAM JPEG frames and status over TCP."
    )
    parser.add_argument("--host", default="0.0.0.0", help="Local IP to bind to")
    parser.add_argument("--port", type=int, default=5005, help="Local TCP port")
    args = parser.parse_args()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.host, args.port))
    server.listen(1)

    print(f"[TCP] listening on {args.host}:{args.port}")
    print("[TCP] press Q or ESC in the window to quit")

    try:
        while True:
            client, address = server.accept()
            with client:
                client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                try:
                    keep_running = handle_client(client, address)
                except (ConnectionError, OSError, ValueError) as exc:
                    print(f"[TCP] connection ended: {exc}")
                    keep_running = True

                if not keep_running:
                    break
    finally:
        server.close()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
