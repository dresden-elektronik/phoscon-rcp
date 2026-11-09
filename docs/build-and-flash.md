# Build and flash

How to build the firmware for each board, package it, check the container, flash it, get a board back and cut a release. Read it before the first build and before every flash.

| § | section |
|---|---|
| 1 | [Toolchain](#1-toolchain) |
| 2 | [Configure and build](#2-configure-and-build) |
| 3 | [Artifacts and sizes](#3-artifacts-and-sizes) |
| 4 | [Packaging](#4-packaging) |
| 5 | [Flashing, with 5.1 the RaspBees on a Raspberry Pi](#5-flashing) |
| 6 | [Pre-flash guard](#6-pre-flash-guard) |
| 7 | [Device ports](#7-device-ports) |
| 8 | [Recovery and the vendor firmware](#8-recovery-and-the-vendor-firmware) |
| 9 | [The ConBee III container](#9-the-conbee-iii-container) |
| 10 | [The ConBee III radio back-end](#10-the-conbee-iii-radio-back-end) |
| 11 | [The ConBee I and RaspBee I](#11-the-conbee-i-and-raspbee-i) |
| 12 | [Versions and releases](#12-versions-and-releases) |

## 1. Toolchain

| tool | version | used for |
|---|---|---|
| `arm-none-eabi-gcc` | developed with Arm GNU Toolchain 15.3.Rel2 | `cb2`, `rb2`, `cb3` |
| `avr-gcc`, `avr-binutils` | 14, from the Homebrew tap `osx-cross/avr` | `cbrb1` (§11) |
| CMake, Ninja | CMake 3.16 or newer | everything |
| Python 3 | with `pyserial` in `.venv` (`python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`) | the guard, the wrappers, the rigs, the generators |
| `GCFTool` | with the `cbrb1` profile | packaging (§4); the board keys it needs are handed out by dresden elektronik on request |
| GCFFlasher | 4.12.6 or newer: a build with libgpiod 2 crashes before it on a RaspBee reset, builds before 4.12.2 list no devices and look like a firmware fault, a ConBee I's `rcp` image needs 4.12.4 (§11), and earlier builds can freeze at a fixed percentage when their output is not a terminal | flashing (§5) |
| the RAIL library | Simplicity SDK `v2025.6.3` | the ConBee III `rail` back-end only, fetched at configure time (§10) |

The host suites need only a host C compiler and CMake.

## 2. Configure and build

| board | `-DDEBOARD` | toolchain file |
|---|---|---|
| ConBee II | `cb2` | `cmake/arm-none-eabi.cmake` |
| RaspBee II | `rb2` | `cmake/arm-none-eabi.cmake` |
| ConBee III | `cb3` | `cmake/arm-none-eabi-m33.cmake` |
| ConBee I, RaspBee I | `cbrb1` | `cmake/avr-gcc.cmake` |

| image | `-DDS_MODE` | the host link carries | use |
|---|---|---|---|
| probe, the default | `probe` | 8-byte debug records | bring-up and diagnostics; a host cannot drive it |
| RCP | `rcp` | HDLC-framed Spinel | the radio co-processor a host drives |

```sh
# host suites
cmake -B build/test -S . && cmake --build build/test
ctest --test-dir build/test --output-on-failure

# ConBee II, RaspBee II (-DDEBOARD=rb2): add -DDS_MODE=rcp for the Spinel image
cmake -B build/rcp -S src -G Ninja -DDEBOARD=cb2 -DDS_MODE=rcp \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake
cmake --build build/rcp

# ConBee III: a Cortex-M33, its own toolchain file
cmake -B build/fw-cb3-rcp -S src -G Ninja -DDEBOARD=cb3 -DDS_MODE=rcp \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi-m33.cmake
cmake --build build/fw-cb3-rcp

# ConBee I, RaspBee I: an 8-bit AVR, avr-gcc 14 (§1, §11)
cmake -B build/fw-cbrb1-rcp -S src -G Ninja -DDEBOARD=cbrb1 -DDS_MODE=rcp \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/avr-gcc.cmake
cmake --build build/fw-cbrb1-rcp
```

- **Never pass `-DCMAKE_BUILD_TYPE=Release`.**
- **If the build fails, do not package and do not flash:** a stale container from an earlier build would be flashed silently.

## 3. Artifacts and sizes

A build writes `phoscon-rcp` (ELF), `.bin`, `.map` and, after packaging, `.bin.GCF` into its build directory. The linker and a build-time guard both refuse an image over the board's budget. Figures 1 and 2 are the one record of the measured sizes.

![One row per RCP image, all to one byte scale, each an amber bar for the image on a hatched track for the room its board allows. ConBee II, cb2, 51 468 B of 237 312 B, in build/rcp. RaspBee II, rb2, 46 672 B of 237 312 B, in build/rb2-rcp. ConBee III, cb3, 61 024 B of 491 504 B, in build/fw-cb3-rcp. ConBee III with the unselected own back-end, 24 076 B of 491 504 B, in build/fw-cb3-own, its bar white. ConBee I and RaspBee I, cbrb1, 43 572 B of 253 952 B, in build/fw-cbrb1-rcp.](images/image-sizes-rcp.png)

*Figure 1 — every RCP image against its board's budget, with its build directory by convention. The budgets are read from `tools/gcf_check.py`. Source: `tools/diagrams/image_sizes.py`.*

![One row per probe image, to the same scale as Figure 1. ConBee II, cb2, 31 516 B of 237 312 B, in build/fw. RaspBee II, rb2, 26 716 B of 237 312 B, in build/rb2. ConBee III, cb3, 7 264 B of 491 504 B, in build/fw-cb3. ConBee I and RaspBee I, cbrb1, 6 624 B of 253 952 B, in build/fw-cbrb1.](images/image-sizes-probe.png)

*Figure 2 — every probe image, to the scale of Figure 1. Source: `tools/diagrams/image_sizes.py`.*

## 4. Packaging

`GCFTool -b <board> <bin>` writes `<bin>.GCF` beside it; `GCFTool -V <file.GCF>` verifies one. The bootloaders accept only encrypted containers, so packaging needs the board keys.

| board | header type | target |
|---|---|---|
| `cb2` | 1 | `0x5000` |
| `rb2` | 31 | `0x5000` |
| `cb3` | 60, a nested container (§9) | `0x4000` |
| `cbrb1` | 1 | `0` (§11) |

The header is 14 bytes, little-endian: magic `0xCAFEFEED`, type, target, size, a CRC-8 byte. A type-0 (plaintext) container is never built: always package with a board profile.

## 5. Flashing

![Two rows. On the host: cmake --build makes the image, the .bin; GCFTool -b with the board makes the container, the .GCF; gcf_check.py, the pre-flash guard, in amber, either refuses it, and nothing is flashed, or passes it to GCFFlasher -d with the port, which talks to the board. On the board: GCFFlasher's reset frame reaches the running app, this firmware or the vendor's, which resets into the bootloader's start window; GCFFlasher then sends the bootloader the container page by page, and after the CRC check the new image runs.](images/flash-flow.png)

*Figure 3 — from a build to a running image. Amber is the guard (§6), the step a container must pass before anything touches a board. Where the running app cannot hear the reset frame, the reset comes from the board's own line instead: a replug on a ConBee, GPIO17 on a RaspBee (§5.1, §11). Source: `tools/diagrams/flash_flow.py`.*

```sh
python3 tools/gcf_check.py <bin>.GCF <bin> --board <board>
GCFFlasher -t 20 -d <port> -f <bin>.GCF
```

- **Always pass the port.** Several sticks of one kind share a port-name prefix (§7).
- **Start GCFFlasher before the bootloader's window opens**, and watch its output: a stuck flasher is otherwise invisible.
- A running image of this tree answers the flasher's reset frame like the vendor firmware, so a flash needs no replug. Over a ConBee I's `rcp` image, that takes GCFFlasher 4.12.4 or newer (§11).

| board | success line | notes |
|---|---|---|
| ConBee II | `firmware successful written` | the bootloader's window is about 5.6 s |
| ConBee III | `app checksum 0x… (OK)` | §9 |
| ConBee I | `firmware successful written` | `-t 30` (§11) |
| RaspBee II, RaspBee I | `finished`, or `firmware successful written` | on the Pi, with `tools/rb2_flash.py` (§5.1) |

After the bootloader's window has closed, a few seconds, ask the RCP for its version. Add `--baud 115200` on a RaspBee II or ConBee III, and `--baud 57600` on a ConBee I or RaspBee I.

```sh
.venv/bin/python tools/spinel_console.py --port <port> --no-arm --get 0x02 --duration 3
```

On a ConBee II, `tools/killprobe.py` packages, guards and flashes in one step, `tools/waitflash.py` also waits for the board to come back, and `tools/autoflash.py <gcf> [bin]` guards, flashes and watches the boot.

### 5.1 The RaspBees on a Raspberry Pi

![The first twenty pins of the Raspberry Pi's 40-pin header, even pins in the top row and odd pins below, pin 1 at the left. Pins 8 (GPIO14), 10 (GPIO15) and 11 (GPIO17) are amber; the rest are labelled with their usual names (5V, GND, 3V3, GPIO2 and others) and left alone. Beside the header: GPIO14 is TXD, the Pi transmits and the module receives; GPIO15 is RXD, the module transmits and the Pi receives; GPIO17 is the reset, driven low for 120 ms to reset the module. Below that, which UART is behind GPIO14 and GPIO15: on a Raspberry Pi 5 it is /dev/ttyAMA0, which pinctrl names TXD0 and RXD0; on a Pi 3 or a Zero W with Bluetooth on it is /dev/ttyS0, the mini UART, TXD1 and RXD1. A last line says to name the port, because /dev/serial0 can point elsewhere.](images/pi-header.png)

*Figure 4 — the header a RaspBee II or RaspBee I sits on. Amber marks the three pins the link uses; the port to name depends on the Pi. The RaspBee I uses the same three pins (§11). Source: `tools/diagrams/pi_header.py`.*

Run `tools/rb2_flash.py` from a checkout of this repository on the Pi. A released container needs only `--gcf`, without `--bin`. For an image of your own, package and guard on the build host and copy the `.bin` and `.GCF` to the Pi:

```sh
python3 tools/rb2_flash.py --port /dev/ttyAMA0 --gcf phoscon-rcp.bin.GCF \
  --bin phoscon-rcp.bin --flasher <path>/GCFFlasher                # RaspBee II on a Pi 5
python3 tools/rb2_flash.py --board cbrb1 --port /dev/ttyS0 ...      # RaspBee I on a Pi 3
```

`tools/rb2_flash.py` runs the guard again, keeps GCFFlasher's stdin open (it quits when stdin closes), and drives the reset line on GPIO17: it pulses it until the bootloader answers, never while pages go in, and — on the RaspBee II — once after the last page, so the flasher sees the bootloader again and prints `finished`.

| setting | why |
|---|---|
| name the port: `/dev/ttyAMA0` on a Pi 5, `/dev/ttyS0` on a Pi 3 or Zero W | `/dev/serial0` can point at another UART; `pinctrl get 14,15` says which one carries the header |
| `dtparam=uart0=on`, no `console=` on the kernel command line | a console there writes boot text into the module |
| wait out the bootloader's window after a flash | a query sent into it meets the bootloader, not the app |

## 6. Pre-flash guard

Every flash path runs `tools/gcf_check.py --board <board>` before GCFFlasher; nothing is flashed unless every check passes (Figure 5).

![Three bars, one per part, each to the scale of its own flash. cb2 and rb2, the ATSAMR21E18A with 256 KiB: the bootloader and its two flag bytes from 0x0, hatched; the app image from 0x5000, in amber, up to 0x3EF00; the deCONZ NVRAM with the MAC address above it, hatched. cb3, the EFR32MG21 with 1 MiB: the bootloader below 0x4000, hatched; the app image, the AppSection, in amber from 0x4000 to 0x7BFF0; the 16-byte AppInfo the bootloader writes; the bootloader's staging area; and NVM3 from 0xF8000, all hatched. cbrb1, the ATmega256RFR2 with 256 KiB: the app image from 0x0, where the reset vector is, in amber up to 0x3E000; the bootloader in the boot section above it, hatched.](images/flash-maps.png)

*Figure 5 — where an image may go on each part. Amber is the room the guard allows; a hatched region is left alone by every flash. The boundaries are read from `tools/gcf_check.py` itself. The ConBee I and RaspBee I keep their MAC address in the EEPROM, a separate memory (`docs/hardware_avr.md`). Source: `tools/diagrams/flash_maps.py`.*

| check | refused when |
|---|---|
| magic and size | the magic is not `0xCAFEFEED`, or the size field does not match the file |
| type | the type is not the board's (`1..10` `cb2` and `cbrb1`, `31..39` `rb2`, `60` `cb3`), or it is plaintext `0`. A ConBee II container offered to a RaspBee II is refused here |
| target | `0x5000` (`cb2`, `rb2`), `0x4000` (`cb3`), `0` (`cbrb1`) is not what the header says |
| size | the image reaches a protected region (Figure 5); for `cbrb1` also an odd size, a whole number of 256-byte pages, or a `.bin` that does not start with a jump (`docs/hardware_avr.md`) |
| CRCs | `cb3`: the outer CRC-8 or the inner CRC-32 does not verify |
| inner shape | `cb3`: the product magic, the inner type or the inner target is wrong |
| the `.bin`'s | the container was packed from another image: its size, or its plain-image checksum (the header CRC-8 on `cb2`, `rb2` and `cbrb1`, the inner CRC-32 on `cb3`), does not match the `.bin`; or the `.GCF` is older than the `.bin` |

A released container comes without its `.bin`: `--released` runs every check that needs only the container (§12).

## 7. Device ports

| board | macOS | Linux |
|---|---|---|
| ConBee II (USB CDC) | `/dev/cu.usbmodem<serial>` | `/dev/ttyACM<n>` |
| ConBee III, ConBee I (a USB-serial bridge) | `/dev/cu.usbserial-<serial>` | `/dev/ttyUSB<n>` |
| RaspBee II, RaspBee I | — | `/dev/ttyAMA0` on a Pi 5, `/dev/ttyS0` on a Pi 3 |

A bridged board is a serial device: the tool sets the line rate, and the ConBee II's CDC helpers do not apply to it. A ConBee II is VID `0x1CF1`, PID `0x0030`; `bcdDevice` `0x0101` means this firmware runs.

## 8. Recovery and the vendor firmware

| a ConBee II that | gets out by |
|---|---|
| runs an app | the flasher's reset frame (`tools/zm.py <port> reset`) |
| hangs after a flash | the bootloader's watchdog resets it within about 16 s |
| sits in the bootloader for ever (a failed upload erased the app's first page) | flashing again; a power cycle does not help |
| is silent | a power cycle |

The vendor firmware goes back through the same GCFFlasher path. The images are published at `https://deconz.dresden-elektronik.de/deconz-firmware/`: `deCONZ_ConBeeII_0x28010700.bin.GCF` for the ConBee II, `deCONZ_Rpi_0x26400500.bin.GCF` for the ConBee I and RaspBee I, and the current release for the RaspBee II and ConBee III.

## 9. The ConBee III container

The ConBee III's GCF payload is itself a container (Figure 6). `GCFTool -b cb3 <bin>` builds it, and rebuilding the vendor's released image gives a byte-identical `.GCF`, which is the test behind the guard.

![Two strips. The outer GCF: a 14-byte header with type 60, target 0x4000 and a CRC-8, then the inner container as its payload, which the CRC-8 covers. The inner container, magnified, stored in plain except for the app: the product magic 0xDEC0DE03 at +0, in amber because it must equal the stick's DeviceID; a u32 size at +4; a 20-byte image header at +8 with the image size, type 61, the target and the plain size; the app from +28, encrypted with AES-256-CBC and key 1; and a CRC-32 over everything before it in the last four bytes.](images/cb3-container.png)

*Figure 6 — the ConBee III container. Amber is the product magic, which the bootloader compares with the stick's own DeviceID. Source: `tools/diagrams/cb3_container.py`.*

- **The size limit** is the 480 KiB AppSection minus the 16-byte AppInfo trailer the bootloader writes after the app: 491 504 B. An app that fills the whole AppSection passes the bootloader's size check and then fails its verification. The linker script and the image guard both stop at the trailer.
- **Flashing** works through the running image (`docs/hardware_mg21.md`, Figure 2). GCFFlasher has no reset line for a ConBee III; if no image answers, it still polls for the bootloader for about 10 s.
- **A stick that answers nothing:** replug it while a sender repeats the bootloader ID request (`c0 81 02 7d ff c0`) every 250 ms and GCFFlasher retries on the same port; each request holds the bootloader's window open.

## 10. The ConBee III radio back-end

`-DDS_MG21_RADIO` chooses the radio back-end of a ConBee III `rcp` image at configure time (`docs/hardware_mg21.md`). The `probe` image drives no radio and links neither.

| value | back-end | use |
|---|---|---|
| `rail` | Silicon Labs' RAIL library behind this tree's adapter | the default |
| `own` | this tree's own sequencer program | only when asked for; it does not yet acknowledge |

`rail` on any other board is a configure error. A build directory keeps the value it was configured with.

### 10.1 Where the library comes from

RAIL is closed source and never enters the repository. Configure runs `tools/fetch_rail.py`, which downloads nine files (2.4 MB: the xG21 archive, its headers and the licence) from one pinned Simplicity SDK release, checks each against a SHA-256 recorded in the tool, and keeps them in `~/.cache/phoscon-rcp/rail/<tag>/` (or `-DDS_RAIL_CACHE=<dir>`). A cached file is not fetched again; with no network and an empty cache, configure stops and names the file to supply by hand. `python3 tools/fetch_rail.py --check` verifies the cache. `src/lib/PROVENANCE.md` records the release and the licence.

### 10.2 The `rail` build

The adapter sources replace the `own` ones; every translation unit is built for the part's hard-float ABI, because the archive is; and the ten radio interrupt slots route to RAIL's handlers. The image is about 61 KB.

## 11. The ConBee I and RaspBee I

```sh
cmake -B build/fw-cbrb1-rcp -S src -G Ninja -DDEBOARD=cbrb1 -DDS_MODE=rcp \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/avr-gcc.cmake
cmake --build build/fw-cbrb1-rcp
GCFTool -b cbrb1 build/fw-cbrb1-rcp/phoscon-rcp.bin
python3 tools/gcf_check.py --board cbrb1 build/fw-cbrb1-rcp/phoscon-rcp.bin.GCF build/fw-cbrb1-rcp/phoscon-rcp.bin
GCFFlasher -t 30 -d <port> -f build/fw-cbrb1-rcp/phoscon-rcp.bin.GCF      # ConBee I
```

- **The `rcp` image runs at 57 600**, where a reset frame at 38 400 goes unheard. GCFFlasher 4.12.4 or newer handles that by itself; with an older one, flash over it with `tools/cbrb1_flash.py` (`docs/hardware_avr.md`).
- **The toolchain:** `brew trust osx-cross/avr`, then `brew install osx-cross/avr/avr-gcc@14 osx-cross/avr/avr-binutils`; `cmake/avr-gcc.cmake` finds the keg-only formula.
- **The image guard** pads the `.bin` with `0xFF` to an even size that is not a multiple of 256, as the bootloader needs (`docs/hardware_avr.md`).
- **The guard needs both paths** with `--board cbrb1`: its defaults are the ConBee II's build directory.
- **A RaspBee I** is flashed on its Pi with `tools/rb2_flash.py --board cbrb1` (§5.1). The vendor app `0x260B0500` found on one board ignores the flasher's reset frame, so the wrapper pulses GPIO17 until the bootloader answers; once it has, the line is left alone, because a pulse during the upload aborts it.
- **Back to the vendor firmware** on a RaspBee I: GCFFlasher by hand (`-b 38400 -d <header-uart>`, stdin open) with a timeout of at least 60 s: the 127 KB image takes more than 35 s at 38 400. The boot section is lock-protected, so no image can take the way back away.

## 12. Versions and releases

`VERSION` at the repository root holds the version, in semantic versioning, and an `rcp` image reports it in `NCP_VERSION` (`docs/protocol.md`). `cmake/version.cmake` reads it on every build:

| the tree | the image reports |
|---|---|
| at the tag `v<VERSION>`, clean, or without git | `0.1.0` |
| at any other commit | `0.1.0+g1a2b3c4d` |
| with uncommitted changes to tracked files | `0.1.0+g1a2b3c4d.dirty` |

To release, in this order:

1. Set `VERSION`, add its entry to `CHANGELOG.md`, update the sizes in `tools/diagrams/image_sizes.py` from a build of an export, and commit.
2. Run `python3 tools/release_images.py --snapshot` with `GCFTOOL` naming the keyed tool, and flash each container on its board.
3. Tag the commit `v<VERSION>`, run `python3 tools/release_images.py`, and attach `dist/<version>/` to the GitHub release: one container per board and `SHA256SUMS`. A tagged build differs from a snapshot in the version string, so flash the tagged containers again.
4. Add the containers to the updater's manifest at <https://phoscon.de/updater>.
