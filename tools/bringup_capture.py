# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""
Reset our app into the bootloader, flash, and catch its record stream - no plug.

A running image is reached over its own CDC port: GCFFlasher sends the same
"reset into the bootloader" frame our app's `onUsbDataOut` recognises
(src/app/main.c), so the app resets itself, the vendor window opens, the upload
lands, and the app re-enumerates. The reader then polls for the tty and opens it
the moment it appears, so the bring-up records are already armed on the bulk IN
pipe when the host issues its first IN token - reading the port *after* the app
has been up for a minute is what loses them.

    .venv/bin/python tools/bringup_capture.py build/fw/phoscon-rcp.bin.GCF <cb2-port>
    .venv/bin/python tools/dbg.py --timeline --file /tmp/hwrec/records.bin

The second line is the point: DBG_COMP_TIME records decode into a bring-up
timeline in CPU cycles (dbg.h). Writes the capture to /tmp/hwrec/records.bin.
Run tools/gcf_check.py first, as every flash path must."""
import cb2flasher
import glob, os, subprocess, sys, time, serial

if len(sys.argv) < 3 or sys.argv[1] in ("-h", "--help"):
    sys.exit(__doc__)
GCF   = sys.argv[1]
PORT  = sys.argv[2]
REPO  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT   = "/tmp/hwrec/records.bin"

def ports():
    return sorted(glob.glob("/dev/cu.usbmodem*"))

def app_port(serial):
    """The node the *same device* reappears as.

    The old code took the first /dev/cu.usbmodem* it found once the given port went away,
    which with two ConBee IIs attached opened the *other* one - it flashed the right device
    (GCFFlasher always got the explicit -d) and then monitored the wrong stick, so a capture
    could come from a device that was not just flashed. Match on the serial instead, and
    refuse to guess when that is ambiguous: an unattended flash path that picks a device is
    worse than one that stops.
    """
    want = os.path.basename(serial.rstrip("/"))
    # macOS names the CDC node <serial><interface>, so the serial is the node name without
    # its last character; compare that way rather than by prefix (two sticks share "DE").
    stem = want[:-1] if want and want[-1].isdigit() else want
    hits = [p for p in ports() if os.path.basename(p).startswith(stem)]
    return stem, hits

print(f"[{time.strftime('%H:%M:%S')}] arming GCFFlasher (it retries the connect itself)")
flash = subprocess.Popen([cb2flasher.resolve(), "-d", PORT, "-t", "90",
                          "-f", GCF], cwd=REPO, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)

t0 = time.time()
while time.time() - t0 < 95:
    out = flash.stdout.readline()
    if not out:
        break
    if "successful written" in out:
        print(f"[{time.strftime('%H:%M:%S')}] flashed: {out.strip()[-40:]}")
        break
else:
    print("no flash confirmation"); sys.exit(1)

flashed_at = time.time()
print(f"[{time.strftime('%H:%M:%S')}] polling for the app's tty (open the instant it appears)")
stem, _ = app_port(PORT)
buf = b""
s = None
while time.time() - flashed_at < 60:
    if s is None:
        # Only the device we just flashed: same base name (the bootloader's port and the
        # app's share it), and stop rather than guess if that is somehow ambiguous.
        stem, cand = app_port(PORT)
        if len(cand) > 1:
            print(f"  {len(cand)} candidate ports for serial {stem}: {cand}")
            print("  refusing to choose - pass the port explicitly and power-cycle the "
                  "device so only it is in its bootloader window")
            sys.exit(2)
        for p in cand:
            try:
                s = serial.Serial(p, 115200, timeout=0.2)
                s.dtr = True
                print(f"[{time.strftime('%H:%M:%S')}] opened {p} (+{time.time()-flashed_at:.1f}s)")
                break
            except Exception:
                s = None
        if s is None:
            time.sleep(0.10)
            continue
    try:
        chunk = s.read(512)
    except Exception as e:
        print(f"[{time.strftime('%H:%M:%S')}] {s.name} gone ({type(e).__name__}), "
              f"polling for the app's port (+{time.time()-flashed_at:.1f}s)")
        try:
            s.close()
        except Exception:
            pass
        s = None
        continue
    if chunk:
        buf += chunk
    time.sleep(0.05)
try:
    if s: s.close()
except Exception:
    pass
open(OUT, "wb").write(buf)
print(f"[{time.strftime('%H:%M:%S')}] captured {len(buf)} B -> {OUT}")
