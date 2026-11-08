# Hardware — ATSAMR21E18A with the AT86RF233 (`cb2`, `rb2`)

The silicon facts the ConBee II and RaspBee II firmware depends on, and the pitfalls that cost a driver its correctness. Read it before changing clock, flash, watchdog, USB, UART or radio code for these boards. Datasheet numbers refer to Microchip's SAM R21 and AT86RF233 datasheets and the SAM R21 errata.

| § | section |
|---|---|
| 1 | [SoC and board](#1-soc-and-board) |
| 2 | [Clocks](#2-clocks) |
| 3 | [Flash, NVM and NVRAM](#3-flash-nvm-and-nvram) |
| 4 | [Watchdog](#4-watchdog) |
| 5 | [USB device](#5-usb-device) |
| 6 | [Radio (AT86RF233)](#6-radio-at86rf233) |
| 7 | [Bootloader and boot contract](#7-bootloader-and-boot-contract) |
| 8 | [Errata that shape the code](#8-errata-that-shape-the-code) |
| 9 | [The RaspBee II host link](#9-the-raspbee-ii-host-link-rb2) |

## 1. SoC and board

| item | value |
|---|---|
| MCU | ATSAMR21E18A, Cortex-M0+ at 48 MHz, 256 KB flash, 32 KB SRAM |
| radio | AT86RF233 on SERCOM4 SPI; powered from VBUS, so it survives an MCU reset |
| clock reference | no crystal for the SoC: the radio's CLKM output is the only accurate reference (§2) |
| flash map | bootloader below `0x5000`, the app from `0x5000`, the deCONZ NVRAM from `0x3EF00` (`docs/build-and-flash.md`, Figure 5); the probe image's post-mortem row at `0x3E000` |

| signal | pin | use |
|---|---|---|
| SLP_TR | PA20 | GPIO out; low keeps the radio awake |
| /RST | PB15 | GPIO out |
| /SEL | PB31 | GPIO out; low for a whole access |
| MOSI, SCLK, MISO | PB30, PC18, PC19 | SERCOM4 PAD2, PAD3, PAD0 |
| CLKM | PC16 | GCLK_IO1, the DFLL's reference |
| radio IRQ | PB00 | EIC EXTINT0, level-high with `WAKEUP` (§6) |
| front-end RX/TX | PA08, PA09 | RFCTRL `FECTRL[0]`, `FECTRL[1]` (§6) |
| front-end enable | PA16 + PA17 (`cb2`), PA16 + PA19 (`rb2`) | GPIO out, high (§6) |
| USB | PA24, PA25 | the same pads are the RaspBee II's UART (§9) |

## 2. Clocks

![The clock tree. The AT86RF233's CLKM output, 8 MHz, enters the SoC on PC16 as GCLK_IO1; generator 1 divides it by 250 to 32 kHz, which the DFLL48M, in amber, multiplies by 1500 in closed loop to 48 MHz for generator 0: the CPU, the SERCOMs and USB. A dashed alternative feeds the DFLL from the USB start-of-frame instead: the bootloader's regime, USBCRM. Below, the watchdog's own chain: OSCULP32K, always on, 32 kHz, divided by 16 in generator 4 to 2 kHz for the WDT on channel 3, 8 s; the bootloader sets ALWAYSON, so the app can feed it but not stop it.](images/samr21-clocks.png)

*Figure 1 — the clock tree. Amber is the loop that makes the 48 MHz. Source: `tools/diagrams/samr21_clocks.py`.*

The rules that cost a bring-up its correctness:

- **The DFLL protocol.** Clear `INTFLAG` (`0xD0`) first; `PCLKSR.DFLLRDY` is a write synchronisation, not a lock, and a control write while it is 0 is ignored; clear `ONDEMAND` before configuring (errata 9905). In closed loop wait for `LCKC`, then `LCKF`. In USBCRM there is no lock to wait for (errata 11938).
- **GCLK is a byte-packed indirect block**, not the SAMD21's 32-bit map: one register selects the entry by the ID in the written value.
- **Assign `CLKCTRL.GEN`, never OR it.** The readback echoes the selected entry, and OR-ing into it picks another generator.
- **Configure a generator in one store** of `ID | SRC | GENEN`: a window write re-specifies the whole entry.
- **Never write a generator whose source has no edges.** The write never completes, `SYNCBUSY` still reads 0, and the next access to the block stalls the CPU until a reset (errata 10416). Every wait is bounded and feeds the watchdog.
- **Flash wait states** go to `RWS = 3` before the AHB reaches 48 MHz.
- **The bootloader hands over at 8 MHz** with the DFLL off and the watchdog still running (§7).

Measured on a ConBee II: USB full speed works, both DFLL lock bits are set, and the radio, used as a timebase, gives 48.4 cycles per microsecond.

**The `rcp` images take a second tree**, because a host schedules frames against the radio clock and the DFLL is not exact: its lock settled some hundreds of ppm off the crystal, differently on each boot, with a 32 kHz reference and with a 1 kHz one. CLKM reaches generator 1 and no other, so the crystal is counted there. The `probe` images keep Figure 1.

| clock | path | use |
|---|---|---|
| radio clock | CLKM 8 MHz, generator 1 /8 = 1 MHz, TC4 and TC5 as one 32-bit counter | timestamps and timed frames, read with continuous synchronisation |
| CPU | generator 1, generator 3 /32 = 31 250 Hz, DFLL × 1536 = 48 MHz | everything else, as in Figure 1 |

Timed frames sent on this clock keep their interval to −68 ppm against another stick's crystal, steady across runs, within ±1 µs.

## 3. Flash, NVM and NVRAM

- **NVMCTRL:** 64-byte pages, 256-byte rows, a row is the erase unit. `ADDR` is a word address. A command written while `READY` is low is ignored, and there is no read-while-write: issue one command, wait, then the next. A command stalls the bus for up to 6 ms (erase), and with it every flash fetch, vectors included, so an `rcp` image programs flash only at boot, to clear the bootloader's update flag when it is set.
- **The user row** (`0x804000`, 64 bytes) holds the silicon configuration in its first 8 bytes and production data after it. The WDT window default is `0xB` on silicon, not the datasheet's `0x5` (errata 13951).
- **The calibration area** (`0x806020`, read-only) holds the DFLL coarse and fine values and the USB pad calibration.

What the firmware reads from them:

| field | where | note |
|---|---|---|
| factory EUI-64 | user row `+0x0A`, or `+0x08` in display order | two production layouts |
| USB serial | user row `+0x12` | 9 ASCII bytes |
| device type | user row `0x80402F` | `0x01` is the UART module (RaspBee II) |
| the deCONZ NVRAM basic set | `0x3FFE0`, the last 32 bytes of flash | the host-set MAC: magic `0xDE90`, id 1 or 2, `mac[8]` least significant first, a CRC; parsed by `samr21_basic.c` under its suite. A MAC beginning `00:50:C2:FF:FF` was stored the other way round by older modules |
| the update flag | `0x4FFF` | the app clears it at every boot (§7) |

## 4. Watchdog

- The WDT is clocked by a GCLK **channel** (channel 3 → gen4, Figure 1), not by its enable bit; a dead channel stops it whatever `CTRL` says.
- The bootloader sets `ALWAYSON` on every boot, at 8 s: the app cannot change the period or stop the timer, and the bootloader's clock teardown leaves the watchdog's channel alone.
- **`Wdt_Feed` skips a feed while `STATUS.SYNCBUSY` is set.** A feed issued while the previous one synchronises stalls the bus for about 21 000 cycles, and on a Cortex-M0+ a stalled bus blocks every interrupt. A feed in flight restarts the period when it lands, so skipping is safe.
- **`PM.RCAUSE` names the last reset** and stays latched until the next one; the image emits it at boot. The bootloader jumps to the app without a reset, so a watchdog reset of the image still reads `WDT` (bit 5). So does the first boot after an update: the bootloader ends an update with a watchdog reset of its own.

## 5. USB device

Full-speed device; `GCLK_USB` must be 48 MHz ±0.25 %.

| endpoint | type | size |
|---|---|---|
| EP0 | control | 64 B |
| EP1 IN | bulk | 64 B |
| EP2 OUT | bulk | 64 B |
| EP3 IN | interrupt (CDC notify) | 64 B |

- The endpoint descriptor table must sit in SRAM, 4-byte aligned. An IN bank is ready when `BK1RDY` is set; an OUT bank is armed when `BK0RDY` is cleared.
- Route `GCLK_USB` before the module's `SWRST`; take `PADCAL` from the calibration area; hold `DETACH` until EP0 is ready.
- Interrupt flags are cleared by writing them back; nothing is acknowledged automatically.
- A host completes a bulk read on a short packet, so a reply that ends on a full packet stays pending until the next one arrives: 500 ms was measured. The firmware sends the last octet of such a reply on its own.

## 6. Radio (AT86RF233)

**SPI.** The data path runs at 4 MHz: 6 MHz corrupts Frame Buffer reads on this board. A queued read moves one octet every 2 µs, 49 µs for a 13-octet frame. `PART_NUM` (register `0x1C`) reads `0x0B`.

**States.** A mode change goes through a *forced* `TRX_OFF`: the plain `TRX_OFF` waits for an active state to finish and leaves a mid-frame change stuck. `CLKM_CTRL` is shadowed across the radio's own reset, so the 8 MHz rate is written, never trusted.

**Transmit.**

- The PHR counts the FCS: the radio computes the last two octets over the rest and sends them *in place of* the last two in the buffer. A driver that writes PHR = payload length sends two octets less than it means, with a valid checksum over the truncated frame.
- A frame that asks for an Ack goes through `TX_ARET_ON` (CSMA-CA, retries, Ack wait); the result is `TRAC_STATUS`: 0 success, 1 success with data pending, 3 channel busy, 5 no Ack.

**CCA threshold.** The driver writes the host's `PHY_CCA_THRESHOLD` into the radio (2 dB steps, −94 to −64 dBm), because the reset threshold, −80 dBm, sits at or below the floor these boards read and blocks a ConBee II's CSMA-CA entirely. **Where the stick sits sets the floor:** a ConBee II straight in a USB 3 port reads 8 dB more noise than on an extension cable, and on a USB hub CSMA-CA may not pass at all.

| ConBee II, channel 24 | quiet channel reads | CSMA-CA fails at and below |
|---|---|---|
| in the host's port | −75 to −77 dBm | −80 dBm; passes at the host's −71 |
| on an extension cable | −83 to −84 dBm | −90 dBm; passes from −86 |
| on a USB hub | −59 to −66 dBm, flat | −64 dBm, the highest threshold the register holds: never passes |

**Give each stick its own USB extension cable, away from the host and from hubs.**

**Receive.**

- `IRQ_STATUS` has one reader, the radio vector: a read clears it. The mask covers every cause the driver consumes.
- A frame is read in one access: PHY status, PHR, PSDU, LQI, ED, RX status (bit 7 the CRC). The frame's RSSI is `−94 + ED` dBm.
- `TRX_CTRL_2` bit 7 (`RX_SAFE_MODE`) keeps a received frame from being overwritten until its read ends. Bit 5 is the scrambler, set at reset: writing `0x20` for protection changes nothing.
- Every receive mode runs in basic `RX_ON`, where the radio neither filters nor acknowledges; the vector filters in software and composes the Ack (`docs/ack-pending-frame.md`).

**Energy measurement.** Request, confirm, then read: a write access to `PHY_ED_LEVEL` starts a measurement (legal only while receiving), `IRQ_4` reports completion, and the level is `−94 + ED` dBm. A request outside a receive state completes with no measurement, and the value survives a retune: reading without a confirmed request on this channel reads the last channel. A manual CCA reports `CCA_STATUS` 1 for **idle**.

**The IRQ line** is level-sensitive (edge detection would need a GCLK route that does not commit on this board) with `WAKEUP` set: without it the request is not taken while the core sleeps, and the vector waits for the next SysTick.

**The firmware-composed Ack.** The vector stamps its entry, reads the frame, decides, writes the four Ack octets and issues `TX_START` 167 µs after the stamp, so the preamble starts 192 µs after the frame. The worst of 1 500 exchanges started at 200 µs.

**AES.** On `cb2` the block cipher is the radio's engine, at 130 µs a block over the SPI against 325 µs for `aes_table.c`: about 2 ms in the main loop for a 121-octet frame. A block holds the bus, and the vector shares the engine, so the key is compared before each block. It is usable while receiving and while transmitting.

**The Enhanced Ack.** Its MIC takes up to 560 µs, which does not fit ahead of the Ack's slot, so the Ack starts on schedule without it. The vector writes the header and zeros, issues `TX_START`, computes the MIC and writes it into the Frame Buffer in SRAM mode while the header is on air. The transceiver reads the buffer as it transmits (datasheet 10.2), and the MIC's first octet leaves 176 + 32 × (1 + header) µs after `TX_START`: 688 µs for the shortest header that needs four blocks. The vector runs that long, above USB and the tick.

**A frame whose Ack the host must see.** `TX_ARET` keeps the Ack to itself, so a version 2 frame asking for one, once a host has set a key, takes its own path: CSMA-CA in the main loop on `RX_ON`, `TX_START` then the upload with interrupts masked (the PHR has 176 µs), and the vector listens for the Ack. Anything else received in that wait is dropped, as `TX_ARET` drops it. Other frames stay on `TX_ARET`.

**A timed frame** takes the same path. The main loop waits in earnest from 3 ms ahead, the last stretch masked, and a slot that cannot be met completes as `CCA_FAILURE`. Against a RAIL stick in both directions, the sum of this board's transmit and receive errors is 0.5 µs at the median and within −2.5 to +4 µs.

**The front end.** Both boards carry a PA/LNA front end, engaged at radio init: `PA_EXT_EN`, the RFCTRL routing of the radio's RX/TX indicator to PA08/PA09, and the board's two enable pins (§1). Without the enable pins a RaspBee II neither hears a ConBee II on channel 26 nor is heard by it.

**Transmit power** is capped at the stock firmware's levels, channel 26 lower for FCC out-of-band emission:

| board | channels 11–25 | channel 26 |
|---|---|---|
| ConBee II | −3 dBm | −12 dBm |
| RaspBee II | −6 dBm | −17 dBm |

## 7. Bootloader and boot contract

The ConBee II's GCF bootloader (V2.07, below `0x5000`) opens a window of about 5.6 s, then jumps to the app at `0x5000` — blindly, with no content check, and with interrupts disabled: **the app starts with `PRIMASK = 1`, at 8 MHz, DFLL off, watchdog running**, and must re-enable interrupts itself.

**The app must clear the update flag at `0x4FFF` at every boot, before anything can hang** (Figure 2): a boot that finds it still set erases the app's first page, and the board stays in the bootloader.

![Four states of the two flag bytes, 0x4FFF (update) and 0x4FFE (reboot). After a flash the upload has set both, 0xA3 and 0xAD. A reset leads to clean boot 1, which erases nothing and clears the reboot flag: 0xA3 and 0x00. From there, if the app clears 0x4FFF early, both read 0x00 and any later reset is safe. If the app dies first, clean boot 2 sees 0xA3 and 0x00 and erases the page at 0x5000; the app gate then fails and the bootloader's start window never closes. The update flag, wherever it is still 0xA3, is drawn in amber.](images/boot-flags.png)

*Figure 2 — the ConBee II boot flags. Amber is the update flag while it is set: the one byte the app must clear at every boot. Source: `tools/diagrams/boot_flags.py`.*

## 8. Errata that shape the code

| errata | consequence |
|---|---|
| 9905 | clear `DFLLCTRL.ONDEMAND` before configuring the DFLL |
| 10416 | a read of a synchronised register whose clock is stopped freezes the CPU until a reset: every GCLK and USB wait is bounded |
| 11938 | the DFLL status bits are wrong in USBCRM: no lock is polled there |
| 13152 | die revision A routes CLKM to another generator; the ConBee IIs measured are later revisions |
| 13951 | the user row's WDT window default is `0xB` |
| 12368, 12005 | PA24/PA25 pulls are not disabled automatically, and the pins cannot be a sampled GPIO input: clear the pulls before muxing them |

## 9. The RaspBee II host link (`rb2`)

The RaspBee II is the ConBee II's SoC and radio behind a UART instead of USB, mounted on a Raspberry Pi's header (`docs/build-and-flash.md`, Figure 4). Each product has its own image, which differs in the link driver, and its own container type (1 for the ConBee II, 31 for the RaspBee II).

| fact | value |
|---|---|
| the link | SERCOM5 as a USART on PA24 (TX) and PA25 (RX), the pads the ConBee II uses for USB |
| line settings | 8N1, no flow control. The `rcp` image runs at 115 200, as a Thread host needs; the `probe` image at 38 400, the rate of the bootloader and the vendor firmware |
| receiving at 115 200 | the receiver's two octets of buffer last about 174 µs, so the UART vector sits above the radio vector, and a frame upload (up to 500 µs) unmasks interrupts once `TX_START` is issued |
| which product it is | the user row's device-type byte `0x80402F`: `0x01` is the UART module |
| the bootloader | GCF bootloader V3 at 38 400. It takes a type-31 container at `0x5000`, verifies it and starts the image; it cannot be read over the link |
| the port on the Pi | the header UART on GPIO14/GPIO15: `/dev/ttyAMA0` on a Pi 5, `/dev/ttyS0` on a Pi 3. `/dev/serial0` may name another UART |
| the reset line | GPIO17, low for 120 ms. `tools/rb2_flash.py` drives it |
| GCFFlasher | quits when its stdin closes; after the last page the bootloader starts the app without the ID packet the flasher waits for. `tools/rb2_flash.py` handles both |
| measured on a Pi 5 | a Thread 1.4 OpenThread host on the `rcp` image at 115 200 with a ConBee II as peer, either stick leading: forty stress runs with no HDLC error and no request left unanswered; the probe image's witnesses read SERCOM5 configured as designed |
| the RTC | the module's PCF85063 is the host's, on its I2C bus; no firmware here touches it |
