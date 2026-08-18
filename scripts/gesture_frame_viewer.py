#!/usr/bin/env python3
#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
"""Live viewer for frames forwarded by the gesture_access feature.

Companion host-side tool for CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
(see subsys/gesture_access/src/frame_forwarding.{h,cpp}): every frame the
board captures and runs inference on is pushed over the SoC's native USB port
as a CDC ACM device, and shown here with a marker drawn over every detected
grid cell (FOMO can see multiple objects at once, so there may be more than
one) plus the live confidence of the strongest cell - handy for picking
CONFIG_DOOR_LOCK_GESTURE_ACCESS_MODEL's detection threshold, since the
confidence number is reported every frame, not just when it clears the bar.

Every above-threshold grid cell the device reports is drawn as a circle with
its confidence, over a grid outline matching the model's output tensor. The
device only forwards its raw per-frame result, not the debounced detection
state it uses internally to decide when to actually unlock (see
UpdateDebounce() in gesture_access.cpp) - so this viewer reimplements the
same confirm/release consecutive-frame counting here, and shows the result
as an OK/WAIT indicator, to mirror what the device would decide.

Press 'q' or Escape to quit (or close the window).

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


# Number of grid cells FOMO's output tensor has along each axis (see
# kOutputWidth/kOutputHeight in gesture_access_model.cpp). The frame carries
# its own pixel dimensions, so the cell size in pixels is derived per-frame.
GRID_SIZE = 12

MARKER_COLOR = (0, 255, 0)
GRID_COLOR = (60, 60, 60)
OK_COLOR = (0, 200, 0)
WAIT_COLOR = (220, 20, 20)


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

        # Host-side reimplementation of UpdateDebounce() in gesture_access.cpp:
        # the device never forwards its debounced state, only the raw
        # per-frame result, so this mirrors the same consecutive-frame
        # confirm/release counting to approximate what the device decides.
        self.confirm_count = 0
        self.release_count = 0
        self.confirmed = False

        self.root = tk.Tk()
        self.root.title(f"Gesture access frames ({port_path})")
        self.root.configure(background="black")
        self.label = tk.Label(self.root, background="black", text="Waiting for frames...",
                              foreground="white", font=("TkDefaultFont", 14))
        self.label.pack(padx=0, pady=0)
        self.root.protocol("WM_DELETE_WINDOW", self.quit)
        self.root.bind("<Escape>", lambda _event: self.quit())
        self.root.bind("<KeyPress-q>", lambda _event: self.quit())
        self.root.focus_set()

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
        if "conf" not in meta:
            return "no data"
        conf = meta.get("conf", 0) / 1000.0
        if not meta.get("det"):
            return "no gesture"
        return f"hand {conf:.2f}"

    def update_debounce(self, detected: bool) -> None:
        """Mirror gesture_access.cpp's UpdateDebounce() using the raw per-frame
        `det` flag, since the device does not forward its own debounced
        state."""
        if detected:
            self.release_count = 0
            self.confirm_count += 1
        else:
            self.confirm_count = 0
            self.release_count += 1

        if not self.confirmed and self.confirm_count >= self.args.confirm_frames:
            self.confirmed = True
        elif self.confirmed and self.release_count >= self.args.release_frames:
            self.confirmed = False

    def draw_grid(self, draw: ImageDraw.ImageDraw, width: int, height: int) -> None:
        for cell in range(1, GRID_SIZE):
            x = cell * width // GRID_SIZE
            draw.line((x, 0, x, height), fill=GRID_COLOR, width=1)
        for cell in range(1, GRID_SIZE):
            y = cell * height // GRID_SIZE
            draw.line((0, y, width, y), fill=GRID_COLOR, width=1)

    def draw_detections(self, draw: ImageDraw.ImageDraw, pts: list[dict], scale: int) -> None:
        radius = 5 * scale
        for pt in pts:
            cx = pt.get("x", 0) * scale
            cy = pt.get("y", 0) * scale
            draw.ellipse(
                (cx - radius, cy - radius, cx + radius, cy + radius),
                outline=MARKER_COLOR,
                width=2,
            )
            conf = pt.get("conf", 0) / 1000.0
            draw.text((cx + radius + 2, cy - radius), f"{conf:.0%}", fill=MARKER_COLOR)

    def draw_indicator(self, draw: ImageDraw.ImageDraw, width: int) -> None:
        radius = 15
        center = (width - radius - 12, radius + 12)
        color = OK_COLOR if self.confirmed else WAIT_COLOR
        draw.ellipse(
            (center[0] - radius, center[1] - radius, center[0] + radius, center[1] + radius),
            fill=color,
            outline=(255, 255, 255),
            width=2,
        )
        text = "OK" if self.confirmed else "WAIT"
        draw.text((center[0] - radius, center[1] + radius + 4), text, fill=(255, 255, 255))

    def draw_stats(self, draw: ImageDraw.ImageDraw, frame: Frame) -> None:
        conf = frame.meta.get("conf", 0) / 1000.0
        streak, required = (
            (self.confirm_count, self.args.confirm_frames)
            if not self.confirmed
            else (self.release_count, self.args.release_frames)
        )
        lines = [
            f"FPS: {self.fps:.1f}",
            f"confidence: {conf:.0%}",
            f"streak: {min(streak, required)}/{required}",
        ]
        for index, text in enumerate(lines):
            draw.text((8, 6 + index * 16), text, fill=(255, 255, 0))

    def render(self, frame: Frame) -> Image.Image:
        image = Image.frombytes("L", (frame.width, frame.height), frame.pixels).convert("RGB")
        scale = self.args.scale
        if scale != 1:
            image = image.resize(
                (frame.width * scale, frame.height * scale), Image.Resampling.NEAREST
            )

        draw = ImageDraw.Draw(image)

        if not self.args.no_marker:
            self.draw_grid(draw, image.width, image.height)
            self.draw_detections(draw, frame.meta.get("pts") or [], scale)

        self.draw_indicator(draw, image.width)
        self.draw_stats(draw, frame)

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
            self.update_debounce(bool(frame.meta.get("det")))

            image = self.render(frame)
            self.photo = ImageTk.PhotoImage(image)
            self.label.configure(image=self.photo, text="")

            detection = self.describe(frame.meta)
            status = "OK" if self.confirmed else "WAIT"
            self.root.title(
                f"Gesture access {frame.width}x{frame.height} | "
                f"{self.fps:4.1f} fps | {detection} | {status}"
                + (f" | dropped {dropped}" if dropped else "")
            )

            if self.args.save_dir:
                self.save(image, frame)

            if not self.args.quiet:
                infer_us = frame.meta.get("us")
                suffix = f" ({infer_us / 1000.0:.1f} ms)" if infer_us is not None else ""
                print(f"#{self.count}: {detection}{suffix}", flush=True)

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
    parser.add_argument(
        "--no-marker", action="store_true", help="Do not draw the grid or detection markers"
    )
    parser.add_argument("--save-dir", help="Also write every displayed frame here as a PNG")
    parser.add_argument("--quiet", action="store_true", help="Do not print a line per frame")
    parser.add_argument(
        "--confirm-frames",
        type=int,
        default=3,
        help="Consecutive detecting frames before the OK/WAIT indicator confirms a gesture "
        "(matches CONFIG_DOOR_LOCK_GESTURE_ACCESS_DEBOUNCE_CONFIRM_FRAMES's default)",
    )
    parser.add_argument(
        "--release-frames",
        type=int,
        default=2,
        help="Consecutive non-detecting frames before the OK/WAIT indicator releases a "
        "confirmed gesture (matches CONFIG_DOOR_LOCK_GESTURE_ACCESS_DEBOUNCE_RELEASE_FRAMES's "
        "default)",
    )
    args = parser.parse_args()

    if args.scale < 1:
        parser.error("--scale must be at least 1")
    if args.confirm_frames < 1 or args.release_frames < 1:
        parser.error("--confirm-frames and --release-frames must be at least 1")

    return args


def main() -> None:
    args = parse_args()
    port_path = args.port or find_port(args.vid, args.pid)
    print(f"Reading frames from {port_path}", file=sys.stderr)
    Viewer(args, port_path).run()


if __name__ == "__main__":
    main()
