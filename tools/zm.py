#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""ZM (Dresden Zigbee-MCU) framed-probe helper for the CB2 CDC port.

Builds frames exactly like GCFFlasher does (PROT_SendFlagged):
    C0 <payload...> <crc_lo> <crc_hi> C0
where crc = two's-complement (16-bit) of the sum of the payload bytes.

Commands:
    zm.py <port> version          query firmware version (CMD 0x0D)
    zm.py <port> reset            write PARAM_WATCHDOG_TIMEOUT=2s (CMD 0x0B)
                                   -> running app resets into the bootloader
    zm.py <port> raw <hex>        send a raw (already framed) payload

    --baud N                      line rate, default 115200. The ConBee II's CDC link is
                                   115200; a RaspBee II's UART speaks the vendor's 38400.

The response is parsed; 'version' prints the firmware build.
"""
import struct
import sys
import time

import serial

CMD_STATUS = 0x07
CMD_FIRMWARE_VERSION = 0x0D
CMD_WRITE_PARAMETER = 0x0B
PARAM_WATCHDOG_TIMEOUT = 0x26


def zm_frame(payload: bytes) -> bytes:
    crc = sum(payload) & 0xFFFF
    crc = (~crc + 1) & 0xFFFF
    return bytes([0xC0]) + payload + bytes([crc & 0xFF, (crc >> 8) & 0xFF, 0xC0])


def version_payload() -> bytes:
    # cmd, seq, status, frame_len(9), 4 dummy
    return bytes([CMD_FIRMWARE_VERSION, 0x05, 0x00, 0x09, 0x00,
                  0x00, 0x00, 0x00, 0x00])


def reset_payload() -> bytes:
    # cmd, seq, status, frame_len(12), buf_len(5), param_id, u32 timeout (2 s)
    return bytes([CMD_WRITE_PARAMETER, 0x03, 0x00, 0x0C, 0x00,
                  0x05, 0x00, PARAM_WATCHDOG_TIMEOUT,
                  0x02, 0x00, 0x00, 0x00])


def parse(rx: bytes):
    """Strip framing (leading C0 ... trailing C0) and return the payload."""
    if not rx:
        return None
    body = rx
    if body[0] == 0xC0:
        body = body[1:]
    if body.endswith(b"\xc0"):
        body = body[:-1]
    # body is now: payload + crc(2)
    return body


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    port, cmd = sys.argv[1], sys.argv[2]

    baud = 115200
    if "--baud" in sys.argv:
        i = sys.argv.index("--baud")
        baud = int(sys.argv[i + 1])
        del sys.argv[i:i + 2]

    if cmd == "version":
        payload = version_payload()
    elif cmd == "reset":
        payload = reset_payload()
    elif cmd == "raw":
        payload = bytes.fromhex("".join(sys.argv[3].split()))
    else:
        print("unknown command", cmd)
        return 2

    frame = zm_frame(payload)
    s = serial.Serial(port, baud, timeout=0.8)
    time.sleep(0.2)
    s.reset_input_buffer()
    s.write(frame)
    s.flush()
    time.sleep(0.7)
    rx = s.read(256)
    s.close()

    if not rx:
        print(f"{cmd}: <no response>  (sent {' '.join(f'{b:02X}' for b in frame)})")
        return 0

    body = parse(rx)
    print(f"{cmd}: rx {' '.join(f'{b:02X}' for b in rx)}")
    if cmd == "version" and body and len(body) >= 9 and body[0] == CMD_FIRMWARE_VERSION:
        status = body[2]
        fw = struct.unpack_from("<I", body, 5)[0]
        print(f"  firmware version = 0x{fw:08X}  (status={status})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
