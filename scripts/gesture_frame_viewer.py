#!/usr/bin/env python3
#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
"""Live viewer for frames forwarded by the gesture_access feature.

Companion host-side tool for CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
(see subsys/gesture_access/src/frame_forwarding.{h,cpp}): every frame the
board captures and runs inference on is pushed over the SoC's native USB port
as a CDC ACM device, and shown here with the detection result drawn on top.

The frame resolution is carried in each frame header, so this works
unmodified across capture sizes. Frames are single-channel grayscale.

Requires Pillow. Everything else (serial port handling, the window) comes
from the standard library, so no pyserial is needed.

Usage:
    python gesture_frame_viewer.py                 # auto-detect the port
    python gesture_frame_viewer.py --port /dev/ttyACM1 --scale 6
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import sys
import termios
import threading
import time
import tkinter as tk
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from PIL import Image, ImageDraw, ImageTk

MAGIC = b"GAFF"
HEADER_FORMAT = "<4sBBHHHI"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
PROTOCOL_VERSION = 1
PIXFMT_GREY8 = 1
CRC_SIZE = 2
CRC_SEED = 0xFFFF

# Guard rails so a desynchronised stream cannot make us allocate wildly.
MAX_DIMENSION = 1024
MAX_META_LEN = 4096

# Matches CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING_USB_{VID,PID}
# defaults (see subsys/gesture_access/Kconfig). Override with --vid/--pid if
# a board build customized them.
DEFAULT_VID = "2fe3"
DEFAULT_PID = "0101"


def crc16_ccitt(seed: int, data: bytes) -> int:
    """Reflected CRC-16 with polynomial 0x1021, matching Zephyr's crc16_ccitt()."""
    for byte in data:
        e = (seed ^ byte) & 0xFF
        f = (e ^ (e << 4)) & 0xFF
        seed = ((seed >> 8) ^ (f << 8) ^ (f << 3) ^ (f >> 4)) & 0xFFFF
    return seed


def usb_ids(tty_name: str) -> tuple[str | None, str | None]:
    """Return the (idVendor, idProduct) of the USB device behind a tty."""
    node = os.path.realpath(f"/sys/class/tty/{tty_name}/device")
    for _ in range(6):
        vendor = Path(node, "idVendor")
        product = Path(node, "idProduct")
        if vendor.exists() and product.exists():
            return vendor.read_text().strip(), product.read_text().strip()
        parent = os.path.dirname(node)
        if parent == node:
            break
        node = parent
    return None, None


def find_port(vid: str, pid: str) -> str:
    """Locate the board's frame channel, ignoring the debugger's own COM port."""
    candidates = sorted(glob.glob("/dev/ttyACM*"))
    if not candidates:
        raise SystemExit("No /dev/ttyACM* devices found. Is the board plugged in?")

    for path in candidates:
        if usb_ids(os.path.basename(path)) == (vid.lower(), pid.lower()):
            return path

    raise SystemExit(
        f"No /dev/ttyACM* device with USB ID {vid}:{pid} found.\n"
        f"Candidates: {', '.join(candidates)}\n"
        "Connect a cable to the SoC USB connector (not only the debugger port), "
        "or pass --port explicitly."
    )


class RawSerial:
    """A CDC ACM port in raw mode. Baud rate is irrelevant for USB CDC."""

    def __init__(self, path: str):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        self._saved = termios.tcgetattr(self.fd)

        iflag, oflag, cflag, lflag, ispeed, ospeed, cc = termios.tcgetattr(self.fd)
        iflag = 0
        oflag = 0
        lflag = 0
        cflag &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE | termios.CRTSCTS)
        cflag |= termios.CS8 | termios.CLOCAL | termios.CREAD
        cc = list(cc)
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 1
        termios.tcsetattr(
            self.fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, ispeed, ospeed, cc]
        )
        termios.tcflush(self.fd, termios.TCIFLUSH)

    def read(self, size: int) -> bytes:
        return os.read(self.fd, size)

    def close(self) -> None:
        try:
            termios.tcsetattr(self.fd, termios.TCSANOW, self._saved)
        finally:
            os.close(self.fd)


class FrameReader:
    """Turns the byte stream into frames, resynchronising after any corruption."""

    def __init__(self, port: RawSerial):
        self.port = port
        self.buf = bytearray()
        self.dropped = 0

    def _fill(self, minimum: int) -> None:
        while len(self.buf) < minimum:
            chunk = self.port.read(max(4096, minimum - len(self.buf)))
            if chunk:
                self.buf.extend(chunk)

    def _resync(self) -> None:
        """Discard bytes up to the next plausible frame start."""
        self.dropped += 1
        start = self.buf.find(MAGIC, 1)
        while start < 0:
            # Keep the tail: the magic may straddle the read boundary.
            del self.buf[: max(0, len(self.buf) - (len(MAGIC) - 1))]
            self._fill(len(self.buf) + 1)
            start = self.buf.find(MAGIC)
        del self.buf[:start]

    def next_frame(self) -> tuple[int, int, bytes, dict]:
        while True:
            self._fill(HEADER_SIZE)

            if not self.buf.startswith(MAGIC):
                self._resync()
                continue

            _, version, pixfmt, width, height, meta_len, data_len = struct.unpack_from(
                HEADER_FORMAT, self.buf
            )

            if (
                version != PROTOCOL_VERSION
                or pixfmt != PIXFMT_GREY8
                or not 0 < width <= MAX_DIMENSION
                or not 0 < height <= MAX_DIMENSION
                or meta_len > MAX_META_LEN
                or data_len != width * height
            ):
                self._resync()
                continue

            total = HEADER_SIZE + meta_len + data_len + CRC_SIZE
            self._fill(total)

            body = bytes(self.buf[HEADER_SIZE : HEADER_SIZE + meta_len + data_len])
            (crc,) = struct.unpack_from("<H", self.buf, HEADER_SIZE + meta_len + data_len)

            if crc16_ccitt(CRC_SEED, body) != crc:
                # The frame was cut short (the board timed out mid-transfer) or
                # damaged. Resynchronise rather than consuming `total` bytes,
                # because the next intact frame is probably already inside the
                # range this header claimed.
                self._resync()
                continue

            del self.buf[:total]

            try:
                meta = json.loads(body[:meta_len]) if meta_len else {}
            except json.JSONDecodeError:
                meta = {}

            return width, height, body[meta_len:], meta


@dataclass
class Frame:
    width: int
    height: int
    pixels: bytes
    meta: dict = field(default_factory=dict)


class Viewer:
    def __init__(self, args: argparse.Namespace, port_path: str):
        self.args = args
        self.port_path = port_path
        self.latest: Frame | None = None
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.error: str | None = None
        self.count = 0
        self.dropped = 0
        self.last_shown = time.monotonic()
        self.fps = 0.0
        self.photo: ImageTk.PhotoImage | None = None

        self.root = tk.Tk()
        self.root.title(f"Gesture access frames ({port_path})")
        self.root.configure(background="black")
        self.label = tk.Label(self.root, background="black", text="Waiting for frames...",
                              foreground="white", font=("TkDefaultFont", 14))
        self.label.pack(padx=0, pady=0)
        self.root.protocol("WM_DELETE_WINDOW", self.quit)
        self.root.bind("<Escape>", lambda _event: self.quit())

        if args.save_dir:
            Path(args.save_dir).mkdir(parents=True, exist_ok=True)

        self.thread = threading.Thread(target=self.reader_loop, daemon=True)

    def reader_loop(self) -> None:
        port = None
        try:
            port = RawSerial(self.port_path)
            reader = FrameReader(port)
            while not self.stop.is_set():
                width, height, pixels, meta = reader.next_frame()
                with self.lock:
                    self.latest = Frame(width, height, pixels, meta)
                    self.dropped = reader.dropped
        except Exception as exc:  # noqa: BLE001 - surfaced in the UI and on exit
            if not self.stop.is_set():
                self.error = f"{type(exc).__name__}: {exc}"
        finally:
            if port is not None:
                port.close()

    @staticmethod
    def describe(meta: dict) -> str:
        boxes = meta.get("boxes") or []
        if not boxes:
            return "no gesture"
        best = max(boxes, key=lambda box: box.get("value", 0.0))
        return f"{best.get('label', '?')} {best.get('value', 0.0):.2f}"

    def render(self, frame: Frame) -> Image.Image:
        image = Image.frombytes("L", (frame.width, frame.height), frame.pixels).convert("RGB")
        scale = self.args.scale
        if scale != 1:
            image = image.resize(
                (frame.width * scale, frame.height * scale), Image.Resampling.NEAREST
            )

        if not self.args.no_boxes:
            draw = ImageDraw.Draw(image)
            for box in frame.meta.get("boxes") or []:
                x0 = box.get("x", 0) * scale
                y0 = box.get("y", 0) * scale
                x1 = x0 + box.get("w", 0) * scale
                y1 = y0 + box.get("h", 0) * scale
                draw.rectangle((x0, y0, x1, y1), outline=(0, 255, 0), width=2)
                caption = f"{box.get('label', '?')} {box.get('value', 0.0):.2f}"
                draw.text((x0 + 2, max(0, y0 - 12)), caption, fill=(0, 255, 0))

        return image

    def save(self, image: Image.Image, frame: Frame) -> None:
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        label = self.describe(frame.meta).split()[0]
        image.save(Path(self.args.save_dir, f"{stamp}_{label}.png"))

    def tick(self) -> None:
        if self.error is not None:
            self.label.configure(image="", text=f"Stream error:\n{self.error}")
            self.root.title("Gesture access frames - disconnected")
            self.root.after(500, self.tick)
            return

        with self.lock:
            frame, self.latest = self.latest, None
            dropped = self.dropped

        if frame is not None:
            now = time.monotonic()
            elapsed = now - self.last_shown
            if elapsed > 0:
                # Light smoothing so the readout does not jitter every frame.
                self.fps = 0.7 * self.fps + 0.3 / elapsed if self.fps else 1.0 / elapsed
            self.last_shown = now
            self.count += 1

            image = self.render(frame)
            self.photo = ImageTk.PhotoImage(image)
            self.label.configure(image=self.photo, text="")

            detection = self.describe(frame.meta)
            self.root.title(
                f"Gesture access {frame.width}x{frame.height} | "
                f"{self.fps:4.1f} fps | {detection}"
                + (f" | dropped {dropped}" if dropped else "")
            )

            if self.args.save_dir:
                self.save(image, frame)

            if not self.args.quiet:
                infer = frame.meta.get("infer_ms")
                seq = frame.meta.get("seq", self.count)
                suffix = f" ({infer} ms)" if infer is not None else ""
                print(f"#{seq}: {detection}{suffix}", flush=True)

        self.root.after(15, self.tick)

    def quit(self) -> None:
        self.stop.set()
        self.root.quit()

    def run(self) -> None:
        self.thread.start()
        self.root.after(15, self.tick)
        try:
            self.root.mainloop()
        finally:
            self.stop.set()
        print(f"Displayed {self.count} frames, dropped {self.dropped}.", file=sys.stderr)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--port",
        help="Serial device of the board's frame channel (default: auto-detect by USB ID)",
    )
    parser.add_argument("--vid", default=DEFAULT_VID, help="USB vendor ID used for auto-detection")
    parser.add_argument("--pid", default=DEFAULT_PID, help="USB product ID used for auto-detection")
    parser.add_argument(
        "--scale", type=int, default=4, help="Integer upscaling factor for the window"
    )
    parser.add_argument("--no-boxes", action="store_true", help="Do not draw detection boxes")
    parser.add_argument("--save-dir", help="Also write every displayed frame here as a PNG")
    parser.add_argument("--quiet", action="store_true", help="Do not print a line per frame")
    args = parser.parse_args()

    if args.scale < 1:
        parser.error("--scale must be at least 1")

    return args


def main() -> None:
    args = parse_args()
    port_path = args.port or find_port(args.vid, args.pid)
    print(f"Reading frames from {port_path}", file=sys.stderr)
    Viewer(args, port_path).run()


if __name__ == "__main__":
    main()
