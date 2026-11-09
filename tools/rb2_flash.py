#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Flash a RaspBee II or a RaspBee I over a Raspberry Pi header UART (docs/build-and-flash.md §5.1).

GCFFlasher does the protocol; this wraps the three things it does not do on these boards
(docs/hardware_samr21.md §9, docs/hardware_avr.md §11.5):

  - it quits as soon as its stdin closes, so stdin is held open with a pipe;
  - its own pulse on the module reset line opens no window, so this drives GPIO17 itself while
    no page is going in - which only matters when the running app cannot hear the flasher's
    reset frame (a vendor-less board, an image at another rate, an old vendor app);
  - RaspBee II: after the last page the bootloader starts the app without the ID packet the
    flasher waits for, so one pulse after the pages stop brings the bootloader back and the
    flasher to `finished`. RaspBee I (--board cbrb1): the bootloader checks the CRC and starts
    the app itself, so the pulses stop once the flasher has synced with it - a pulse while pages
    go in aborts the update.

The container is guarded before anything touches the board, as on every flash path in this tree
(docs/build-and-flash.md §6). Run it on the host the module is mounted on:

  tools/rb2_flash.py --port <header-uart> --gcf phoscon-rcp.bin.GCF \
      --bin phoscon-rcp.bin --flasher <path>/GCFFlasher [--board cbrb1]

<header-uart> is the UART on GPIO14/GPIO15: /dev/ttyAMA0 on a Pi 5, /dev/ttyS0 on a Pi 3 or a
Zero W with Bluetooth on (docs/build-and-flash.md §5.1).

Exit is 0 when GCFFlasher reports completion, 1 otherwise.
"""

import argparse
import os
import subprocess
import sys
import threading
import time

# Completion: the V3 bootloader path (a RaspBee II container) closes with `app checksum 0x… (OK)`
# and `finished`; the older path prints `firmware successful written`.
SUCCESS = ("app checksum", "finished", "firmware successful written")
# Pages are in flight while progress lines keep arriving, and a reset inside that window aborts them.
UPLOAD_COOLDOWN_S = 8.0


def strip_controls(text: str) -> str:
    """Drop the progress line's cursor moves so the log reads as one line per event."""
    out = []
    for chunk in text.replace("\r", "\n").split("\n"):
        clean = chunk
        for mark in ("\x1b[",):
            while mark in clean:
                rest = clean.split(mark, 1)[1]
                tail = rest.lstrip("0123456789;")
                clean = clean.replace(mark + rest[: len(rest) - len(tail)], "")
        if "uploading" in clean:
            clean = clean.split("uploading", 1)[0] + "uploading"
        if clean.strip():
            out.append(clean.strip())
    return "\n".join(out)


def guard(gcf: str, image: str | None, board: str) -> None:
    """Run the mandatory pre-flash guard; a failure must stop here, not at the board."""
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gcf_check.py")
    argv = [sys.executable, tool, gcf]
    argv.append(image if image else "--released")
    argv += ["--board", board]
    print("$ " + " ".join(argv))
    res = subprocess.run(argv, capture_output=True, text=True)
    print(res.stdout.strip() or res.stderr.strip())
    if res.returncode != 0:
        sys.exit("guard refused the container; nothing was flashed")


def reset_line(chip_path: str, line: int):
    """Return (pulse_fn, release_fn) for the module reset line, or None when libgpiod is missing."""
    try:
        from gpiod import Chip, LineSettings
        import gpiod.line as G
    except ImportError:
        return None
    chip = Chip(chip_path)
    settings = LineSettings()
    settings.direction = G.Direction.OUTPUT
    settings.output_value = G.Value.ACTIVE  # released: the line asserts reset when it goes inactive
    request = chip.request_lines(config={line: settings}, consumer="phoscon-rcp rb2 flash")

    def pulse(seconds: float) -> None:
        request.set_value(line, G.Value.INACTIVE)
        time.sleep(seconds)
        request.set_value(line, G.Value.ACTIVE)

    def release() -> None:
        request.release()
        chip.close()

    return pulse, release


def main() -> int:
    ap = argparse.ArgumentParser(description="flash a RaspBee II or RaspBee I, driving its reset line")
    ap.add_argument("--port", required=True, help="the header UART the module sits on: /dev/ttyAMA0 (Pi 5) or /dev/ttyS0 (Pi 3)")
    ap.add_argument("--gcf", required=True, help="the packaged container")
    which = ap.add_mutually_exclusive_group(required=True)
    which.add_argument("--bin", help="the .bin the container was made from; the guard checks the container against it")
    which.add_argument("--released", action="store_true",
                       help="a released container, which comes without its .bin (docs/build-and-flash.md §12)")
    ap.add_argument("--flasher", required=True, help="path to GCFFlasher")
    ap.add_argument("--board", choices=("rb2", "cbrb1"), default="rb2",
                    help="the guard's board rules: rb2 for a RaspBee II, cbrb1 for a RaspBee I")
    ap.add_argument("--baud", type=int, default=38400, help="the rate the board's bootloader speaks")
    ap.add_argument("--timeout", type=int, default=45, help="GCFFlasher's -t window")
    ap.add_argument("--gpiochip", default="/dev/gpiochip0")
    ap.add_argument("--reset-line", type=int, default=17, help="the module reset line on the header")
    ap.add_argument("--pulse-start", type=float, default=5.5,
                    help="wait this long before the first pulse: the flasher's own UART reset attempt "
                         "has to time out first, and a pulse before it is spent before the query")
    ap.add_argument("--pulse-interval", type=float, default=1.2)
    ap.add_argument("--pulse-width", type=float, default=0.12)
    ap.add_argument("--no-reset-line", action="store_true", help="rely on the flasher's own reset path")
    args = ap.parse_args()

    guard(args.gcf, args.bin, args.board)

    driver = None
    if not args.no_reset_line:
        driver = reset_line(args.gpiochip, args.reset_line)
        if driver is None:
            print("libgpiod for Python is not available; falling back to the flasher's own reset path")

    argv = [args.flasher, "-t", str(args.timeout), "-b", str(args.baud), "-f", args.gcf, "-d", args.port]
    print("$ " + " ".join(argv))

    # GCFFlasher ends its run when its stdin closes, so the pipe is opened and never written to.
    proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace")

    seen: list[str] = []
    stop = threading.Event()
    last_upload = [0.0]

    def read_output() -> None:
        assert proc.stdout is not None
        for raw in proc.stdout:
            line = raw.strip()
            if line:
                seen.append(line)
                if "uploading" in line or "BTL data request" in line:
                    last_upload[0] = time.time()
                print("   " + strip_controls(line))
        stop.set()

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()

    def drive_reset() -> None:
        if driver is None:
            return
        pulse, release = driver
        time.sleep(args.pulse_start)
        while not stop.is_set():
            text = "\n".join(seen).lower()
            if any(mark.lower() in text for mark in SUCCESS):
                print("   verified: reset line left alone")
                break
            if args.board == "cbrb1" and "bootloader synced" in text:
                print("   synced: reset line left alone, the bootloader starts the app itself")
                break
            # Hold off while pages go in, then pulse again: after the last page the flasher waits for
            # the board to come back and report its app checksum, and only a reset puts the
            # bootloader back on the wire.
            if time.time() - last_upload[0] < UPLOAD_COOLDOWN_S:
                time.sleep(0.3)
                continue
            pulse(args.pulse_width)
            time.sleep(args.pulse_interval)
        release()

    pacer = threading.Thread(target=drive_reset, daemon=True)
    pacer.start()

    code = proc.wait()
    stop.set()
    reader.join(timeout=2)
    pacer.join(timeout=2)

    text = "\n".join(seen)
    text = "\n".join(seen).lower()
    ok = proc.returncode == 0 and any(mark.lower() in text for mark in SUCCESS)
    for mark in ("command reset timeout", "RaspBee reset done", "query bootloader id",
                 "bootloader version", "done, wait", "app checksum", "finished",
                 "firmware successful written"):
        hits = sum(1 for line in seen if mark.lower() in line.lower())
        if hits:
            print(f"   {mark}: {hits}")
    print(f"== flasher exit={code} {'success' if ok else 'no success marker'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
