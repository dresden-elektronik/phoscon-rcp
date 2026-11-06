#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Shared CDC view for the board-facing tools: bytes in, meaning out.

monitor2.py, watch.py and bootprobe.py all watched the CDC port and printed
repr() of whatever came back. That is the right thing for the vendor
bootloader - it answers with text ("help", "ID", "unlock! ", "VALID CRC
0xC9 ") - and the wrong thing for our own app, whose CDC bulk IN carries
8-byte binary debug records (docs/coding-rules.md section 5, src/hal/dbg.h).
Those records were being printed as mojibake, i.e. the one channel a ConBee
II gives us was being watched without being read.

This module keeps the two apart without being told which firmware is running:

  - the record contract is fixed-size (every drain hands over a multiple of 8),
    so bytes are consumed 8 at a time;
  - a frame whose magic field is not 0x0000 is *not* a record: it is shown as
    text, and the view then resynchronises on the next plausible record header
    (a phase change - a reset into the bootloader, or a bootloader that wrote
    text and then handed the port to the app) is survivable instead of
    poisoning the rest of the stream;
  - decoding uses tools/dbg.py, so the string tables are parsed out of src/
    and can never drift away from the firmware.

Usage from a tool:

    import cdcview
    view = cdcview.CdcView("watch")
    ...
    for line in view.feed(chunk):
        print(f"[{t:8.3f}] {line}")
    ...
    for line in view.flush():        # end of session: what never completed
        print(line)
"""

import os
import sys

RECORD_BYTES = 8
MAGIC = 0x0000

# A plausible record header, for resynchronising only.  A CDC stream on this
# board mixes the vendor bootloader's text with our app's records, and a
# half-consumed frame must not make every later frame look broken.
# Only used to tell "this looks like a record header" from "this is the
# bootloader talking": the magic field does the real work (our frames always
# carry 0x0000, the vendor's text never does), the bounds just stop a stray
# NUL pair in a text stream from inventing a record.  A reason code is NOT
# bounded here - an undeclared reason must still show up as UNKNOWN so that
# dbg.py --strict can complain about it.
MAX_COMPONENT = 16
MAX_LINE = 4000


def _load_decoder(root=None):
    """tools/dbg.py is the single source of the tables; import it, do not copy it."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import dbg
    except ImportError:
        return None
    root = root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    components, reasons, _order = dbg.parse_dbg_header(root)
    files = dbg.component_files(root, components)
    return dbg.Decoder(root, components, reasons, files)


def ascii_hint(payload):
    return "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in payload)


def looks_like_record(data, at):
    if at + RECORD_BYTES > len(data):
        return False
    magic, comp, line = (int.from_bytes(data[i:i + 2], "little")
                         for i in (at, at + 2, at + 4))
    return magic == MAGIC and 1 <= comp <= MAX_COMPONENT and 1 <= line <= MAX_LINE


class CdcView:
    """Turn a CDC byte stream into record lines and text lines.

    A run of non-record bytes is COALESCED into one text line per 64 bytes: the
    vendor bootloader answers the flasher with one short line per page, so a
    naive per-frame report buries the dozen records that matter.
    """

    TEXT_FLUSH = 64        # bytes of text to show in one line
    KEEP = RECORD_BYTES - 1  # always keep this much back: a record may straddle chunks

    def __init__(self, label="cdc", root=None, decoder=None, hexdump=False,
                 on_record=None, echo=True):
        self.label = label
        self.on_record = on_record      # dbg.py uses this to build JSON records
        self.echo = echo                # False: the caller prints the record lines
        self.decoder = decoder if decoder is not None else _load_decoder(root)
        self.hexdump = hexdump
        self.buf = bytearray()
        self.text_run = bytearray()
        self.records = 0
        self.texts = 0
        self.resyncs = 0

    def _record_line(self, rec):
        if self.decoder is None:
            comp, line, reason = rec
            return "record  comp=%d line=%d reason=%d (dbg.py not importable)" % (
                comp, line, reason)
        head, extra = self.decoder.text(rec, prefix="record  ")
        if extra:
            return head + "\n            " + "\n            ".join(extra)
        return head

    @staticmethod
    def _looks_printable(blob):
        if len(blob) < 4:
            return False
        good = sum(1 for b in blob if 0x20 <= b < 0x7F or b in (9, 10, 13))
        return good >= 0.8 * len(blob)

    def _text_line(self):
        if not self.text_run:
            return None
        blob = bytes(self.text_run)
        self.text_run.clear()
        self.texts += 1
        line = "%s text  %r  (%d byte(s), not records)" % (
            self.label, ascii_hint(blob), len(blob))
        if self.hexdump:
            line += "  [%s]" % blob.hex(" ")
        return line

    def feed(self, data):
        """Return the lines this chunk produced."""
        out = []
        self.buf += data
        while True:
            if len(self.buf) >= RECORD_BYTES and looks_like_record(self.buf, 0):
                line = self._text_line()
                if line:
                    out.append(line)
                rec = tuple(int.from_bytes(self.buf[i:i + 2], "little")
                            for i in (2, 4, 6))       # component, line, reason
                del self.buf[:RECORD_BYTES]
                self.records += 1
                if self.on_record:
                    self.on_record(rec)
                if self.echo:
                    out.append(self._record_line(rec))
                continue

            # Find the next plausible header.  Everything before it is text.
            at = 1
            while at <= len(self.buf) - RECORD_BYTES and not looks_like_record(self.buf, at):
                at += 1
            if at <= len(self.buf) - RECORD_BYTES:
                self.text_run += self.buf[:at]
                del self.buf[:at]
                self.resyncs += 1
                if len(self.text_run) >= self.TEXT_FLUSH:
                    out.append(self._text_line())
                continue

            # No header in what we hold.  If it reads like prose it IS text - a
            # live session with the vendor bootloader has to appear as it arrives,
            # not after 64 bytes collect.  Our records have NULs in every frame
            # (magic 0x0000), so "mostly printable" separates the two reliably.
            if self._looks_printable(self.buf):
                self.text_run += self.buf
                self.buf.clear()
                out.append(self._text_line())
                break
            # Otherwise keep the tail back: a record may complete with the next chunk.
            if len(self.buf) > self.KEEP:
                self.text_run += self.buf[:len(self.buf) - self.KEEP]
                del self.buf[:len(self.buf) - self.KEEP]
                if len(self.text_run) >= self.TEXT_FLUSH:
                    out.append(self._text_line())
            break
        return out

    def flush(self):
        """End of session: what is left, records or not."""
        out = []
        line = self._text_line()
        if line:
            out.append(line)
        if len(self.buf) >= RECORD_BYTES and looks_like_record(self.buf, 0):
            rec = tuple(int.from_bytes(self.buf[i:i + 2], "little") for i in (2, 4, 6))
            self.records += 1
            if self.on_record:
                self.on_record(rec)
            if self.echo:
                out.append(self._record_line(rec))
            self.buf.clear()
        elif self.buf:
            tail = bytes(self.buf)
            self.buf.clear()
            line = "%s tail  %r  (%d byte(s), no complete record)" % (
                self.label, ascii_hint(tail), len(tail))
            if self.hexdump:
                line += "  [%s]" % tail.hex(" ")
            out.append(line)
        return out

    def stats(self):
        return "%d record(s), %d text line(s), %d resynchronisation(s)" % (
            self.records, self.texts, self.resyncs)


if __name__ == "__main__":
    # Read stdin bytes and decode them - lets a capture be re-read later, and
    # gives the other tools a command-line fallback.
    view = CdcView("stdin", hexdump=("--hex" in sys.argv))
    data = sys.stdin.buffer.read()
    step = 64
    for off in range(0, len(data), step):
        for line in view.feed(data[off:off + step]):
            print(line)
    for line in view.flush():
        print(line)
    print("decoded from %d byte(s): %s" % (len(data), view.stats()))
