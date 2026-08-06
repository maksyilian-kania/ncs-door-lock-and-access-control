#!/usr/bin/env python3
#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
"""Live viewer for frames forwarded by the gesture_access feature.

Companion host-side tool for CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
(see subsys/gesture_access/src/frame_forwarding.{h,cpp}): every frame the
board captures and runs inference on can be pushed over a serial connection
and shown here with the detection result drawn on top.

Skeleton only for now. TODO: port over the working wire-protocol framing/CRC
resync/rendering logic once frame_forwarding.cpp implements the on-device side and the wire format is finalized.

Usage:
    python gesture_frame_viewer.py                 # auto-detect the port
    python gesture_frame_viewer.py --port /dev/ttyACM1 --scale 6
"""

from __future__ import annotations

import argparse
import sys

# TODO: wire format constants (magic/version/pixel format, header layout,
# CRC parameters) once frame_forwarding.cpp defines them. Keep this in sync
# with subsys/gesture_access/src/frame_forwarding.h.


def find_port() -> str:
    """Locate the board's frame channel.

    TODO: auto-detect by USB VID/PID (see frame_forwarding.cpp) or serial
    port enumeration, following the approach in sdk-edge-ai's
    gesture_frame_viewer.py.
    """
    raise NotImplementedError


class FrameReader:
    """Turns the byte stream into frames, resynchronising after corruption.

    TODO: port the framing/CRC/resync logic once the wire format is final.
    """

    def __init__(self, port_path: str):
        self.port_path = port_path

    def next_frame(self):
        raise NotImplementedError


class Viewer:
    """Displays frames and the gesture inference result as they arrive.

    TODO: port the Tk/Pillow rendering loop.
    """

    def __init__(self, args: argparse.Namespace, port_path: str):
        self.args = args
        self.port_path = port_path

    def run(self) -> None:
        raise NotImplementedError


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--port",
        help="Serial device of the board's frame channel (default: auto-detect)",
    )
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
    port_path = args.port or find_port()
    print(f"Reading frames from {port_path}", file=sys.stderr)
    Viewer(args, port_path).run()


if __name__ == "__main__":
    main()
