import argparse
import socket
import struct
import time

import cv2
import numpy as np


HEADER_JPEG = b"JPEG"
HEADER_STATUS = b"STAT"
HEADER_CONTROL = b"CTRL"
HEADER_SIZE = 8
MAX_PACKET_SIZE = 2_000_000
WINDOW_NAME = "ESP32-CAM TCP Viewer"
CONTROL_WINDOW = "ESP32-CAM Controls"
STATUS_LINES = 8

FRAME_SIZE_OPTIONS = [
    "QVGA",
    "VGA",
    "SVGA",
    "XGA",
    "HD",
    "SXGA",
    "UXGA",
]

CONTROL_DEFAULTS = {
    "framesize": 2,
    "jpeg_quality": 10,
    "brightness": 1,
    "contrast": 0,
    "saturation": -1,
    "sharpness": 0,
    "hmirror": 0,
    "vflip": 0,
    "led": 0,
}


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


def render_disconnected(status_lines: list[str]) -> None:
    frame = render_frame(None, 0.0, status_lines)
    cv2.imshow(WINDOW_NAME, frame)
    cv2.waitKey(1)


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


def parse_cfg_status(message: str) -> dict[str, int] | None:
    if not message.startswith("cfg "):
        return None

    values: dict[str, int] = {}
    for item in message[4:].split():
        if "=" not in item:
            continue
        key, raw_value = item.split("=", 1)
        if key == "framesize":
            number_part = raw_value.split("(", 1)[0]
        else:
            number_part = raw_value
        try:
            values[key] = int(number_part)
        except ValueError:
            continue
    return values


def noop(_: int) -> None:
    return


def create_control_window() -> None:
    cv2.namedWindow(CONTROL_WINDOW, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(CONTROL_WINDOW, 540, 320)
    cv2.createTrackbar("FrameSize", CONTROL_WINDOW, CONTROL_DEFAULTS["framesize"], len(FRAME_SIZE_OPTIONS) - 1, noop)
    cv2.createTrackbar("JPEG Quality", CONTROL_WINDOW, CONTROL_DEFAULTS["jpeg_quality"], 63, noop)
    cv2.createTrackbar("Brightness", CONTROL_WINDOW, CONTROL_DEFAULTS["brightness"] + 2, 4, noop)
    cv2.createTrackbar("Contrast", CONTROL_WINDOW, CONTROL_DEFAULTS["contrast"] + 2, 4, noop)
    cv2.createTrackbar("Saturation", CONTROL_WINDOW, CONTROL_DEFAULTS["saturation"] + 2, 4, noop)
    cv2.createTrackbar("Sharpness", CONTROL_WINDOW, CONTROL_DEFAULTS["sharpness"] + 2, 4, noop)
    cv2.createTrackbar("Mirror", CONTROL_WINDOW, CONTROL_DEFAULTS["hmirror"], 1, noop)
    cv2.createTrackbar("VFlip", CONTROL_WINDOW, CONTROL_DEFAULTS["vflip"], 1, noop)
    cv2.createTrackbar("Flash LED", CONTROL_WINDOW, CONTROL_DEFAULTS["led"], 1, noop)


def set_control_values(values: dict[str, int]) -> None:
    cv2.setTrackbarPos("FrameSize", CONTROL_WINDOW, max(0, min(len(FRAME_SIZE_OPTIONS) - 1, values.get("framesize", 1))))
    cv2.setTrackbarPos("JPEG Quality", CONTROL_WINDOW, max(4, min(63, values.get("jpeg_quality", 10))))
    cv2.setTrackbarPos("Brightness", CONTROL_WINDOW, max(0, min(4, values.get("brightness", 0) + 2)))
    cv2.setTrackbarPos("Contrast", CONTROL_WINDOW, max(0, min(4, values.get("contrast", 0) + 2)))
    cv2.setTrackbarPos("Saturation", CONTROL_WINDOW, max(0, min(4, values.get("saturation", 0) + 2)))
    cv2.setTrackbarPos("Sharpness", CONTROL_WINDOW, max(0, min(4, values.get("sharpness", 0) + 2)))
    cv2.setTrackbarPos("Mirror", CONTROL_WINDOW, max(0, min(1, values.get("hmirror", 0))))
    cv2.setTrackbarPos("VFlip", CONTROL_WINDOW, max(0, min(1, values.get("vflip", 0))))
    cv2.setTrackbarPos("Flash LED", CONTROL_WINDOW, max(0, min(1, values.get("led", 0))))


def get_control_values() -> dict[str, int]:
    return {
        "framesize": cv2.getTrackbarPos("FrameSize", CONTROL_WINDOW),
        "jpeg_quality": max(4, cv2.getTrackbarPos("JPEG Quality", CONTROL_WINDOW)),
        "brightness": cv2.getTrackbarPos("Brightness", CONTROL_WINDOW) - 2,
        "contrast": cv2.getTrackbarPos("Contrast", CONTROL_WINDOW) - 2,
        "saturation": cv2.getTrackbarPos("Saturation", CONTROL_WINDOW) - 2,
        "sharpness": cv2.getTrackbarPos("Sharpness", CONTROL_WINDOW) - 2,
        "hmirror": cv2.getTrackbarPos("Mirror", CONTROL_WINDOW),
        "vflip": cv2.getTrackbarPos("VFlip", CONTROL_WINDOW),
        "led": cv2.getTrackbarPos("Flash LED", CONTROL_WINDOW),
    }


def encode_control_payload(values: dict[str, int]) -> bytes:
    message = ";".join(f"{key}={value}" for key, value in values.items())
    return message.encode("utf-8")


def send_control_packet(sock: socket.socket, values: dict[str, int]) -> None:
    payload = encode_control_payload(values)
    header = HEADER_CONTROL + struct.pack("!I", len(payload))
    sock.sendall(header + payload)


def handle_client(client: socket.socket, address: tuple[str, int]) -> bool:
    print(f"[TCP] client connected: {address[0]}:{address[1]}")

    frames = 0
    last_fps_update = time.monotonic()
    fps = 0.0
    last_control_send = 0.0
    status_lines = ["TCP connected", "Use the Controls window to tweak the camera."]
    last_raw_frame: np.ndarray | None = None
    last_sent_controls = CONTROL_DEFAULTS.copy()
    controls_synced = False

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
            cfg_values = parse_cfg_status(status_text)
            if cfg_values is not None:
                set_control_values(cfg_values)
                last_sent_controls = get_control_values()
                controls_synced = True
        else:
            raise ValueError(f"unexpected packet header: {packet_type!r}")

        current_controls = get_control_values()
        now = time.monotonic()
        if current_controls != last_sent_controls and (now - last_control_send) >= 0.2:
            send_control_packet(client, current_controls)
            append_status(
                status_lines,
                "control sent: "
                + f"{FRAME_SIZE_OPTIONS[current_controls['framesize']]} q={current_controls['jpeg_quality']} led={current_controls['led']}",
            )
            last_sent_controls = current_controls
            last_control_send = now
        elif not controls_synced and (now - last_control_send) >= 1.0:
            send_control_packet(client, current_controls)
            last_sent_controls = current_controls
            last_control_send = now

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

    create_control_window()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.host, args.port))
    server.listen(1)

    print(f"[TCP] listening on {args.host}:{args.port}")
    print("[TCP] press Q or ESC in the window to quit")
    render_disconnected(["Viewer listening", f"{args.host}:{args.port}"])

    try:
        while True:
            client, address = server.accept()
            with client:
                client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                try:
                    keep_running = handle_client(client, address)
                except (ConnectionError, OSError, ValueError) as exc:
                    print(f"[TCP] connection ended: {exc}")
                    render_disconnected(["TCP disconnected", str(exc)])
                    keep_running = True

                if not keep_running:
                    break
    finally:
        server.close()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
