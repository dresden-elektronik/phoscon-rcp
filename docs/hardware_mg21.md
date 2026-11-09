# Hardware — EFR32MG21 (`cb3`, the ConBee III)

The silicon facts behind `src/hal/mg21/`: the part, where its identity lives, the bring-up rules that differ from the SAMR21, and the two radio back-ends. Read it before changing anything under `src/hal/mg21/`. The board itself, with its bootloader and pins, is §10.7 and §10.8.

Section numbers continue `docs/hardware_samr21.md`, so this document starts at §10.

## 10. EFR32MG21 — the ConBee III silicon and radio

### 10.1 The part

| item | value |
|---|---|
| part | EFR32MG21A010F1024IM32: Cortex-M33, 1 MiB flash in 8 KiB erase pages, 96 KiB RAM |
| clock | the 38.4 MHz HFXO, trimmed per board by the manufacturing token's CTUNE; the image starts on the 20 MHz FSRCO and switches |
| radio | on-die 2.4 GHz; the A010 variant is capped at 10 dBm, −104.3 dBm sensitivity at 250 kbit/s |
| vector table | 77 entries; SysTick is the core's own |
| how the image starts | only from the bootloader, straight after a reset, in the part's reset state (FSRCO, HFXO off, USART0 untouched). Any reset the image does not ask for reopens the bootloader window, which is what makes a `cb3` image recoverable over the link (§10.7) |
| peripheral aliases | the image addresses peripherals through their `_NS` aliases, as the stock application does; the core runs Secure either way, so the alias is an addressing choice, not a security state |
| GPIO input | `gpioModeInput` has no pull on this family; a low-active input needs an external resistor |

The stock coordinator firmware fills all 96 KiB of RAM, 44 KiB of it a heap for its Zigbee stack. An RCP image here has the same RAM under this tree's no-allocation rule, so its budget is static pools.

### 10.2 Identity and flash

The stick's identity lives in the user data page (`0x0FE00000`, 1 KiB), outside the main flash: a firmware reads it and never writes it.

| item | where | what the firmware does |
|---|---|---|
| the factory MAC | the manufacturing token `TOKEN_MFG_CUSTOM_EUI_64`, user data page `+0x002`, least significant byte first | `HWADDR` is the token read most significant byte first (`hal/mg21/mg21_ident.c`); an all-`0xFF`, all-zero or group value answers `INVALID_STATE` |
| the bootloader's DeviceConfig | user data page `+0x300`: product id (`0xDEC0DE03` for the ConBee III), serial, MAC, the link (USART0, PB1/PB0, 115 200) | read only; its product id is what a container's product magic must match |
| NVM3 | `0xF8000`, the last 32 KiB of flash | nothing of this firmware's goes there or into the bootloader's staging area below it (`docs/build-and-flash.md`, Figure 6) |

- Silicon Labs' own DEVINFO EUI-64 is not a fallback: deCONZ does not know the stick by it.
- A development stick may carry a token programmed from a 17-digit string kept to its low 16 digits (`0212EFFFFDEAFFE1`). That is what was written, and `HWADDR` reports it as stored.
- Whether a mass erase also clears the user data page is not settled, so a recovery must not count on the tokens surviving one.
- Both sides of the container format use the reflected CRC-32 (`0xEDB88320`, init `0xFFFFFFFF`, complemented); the bootloader computes it in the GPCRC peripheral.

### 10.3 Bring-up rules

The numbers below are computed by this tree's code and pinned by its suites (`mg21_regs`, `usart_baud`, `tick_plan`). Three of them are the ones a SAMR21 habit gets wrong: the CMU here enables no peripheral clocks, the USART does not route its own pads, and writing an interrupt flag does not clear it.

| item | rule |
|---|---|
| peripheral clocks | there is no clock-enable register: a peripheral is enabled by its own `EN`, and the CMU only selects each group's *source*. `EM01GRPACLKCTRL` resets to an oscillator this board does not run, so it must be written |
| interrupt flags | a write to `IF` does nothing; set and clear go through the `IF_SET` and `IF_CLR` aliases at `+0x1000` and `+0x2000`. A stale `TXC` or an uncleared `RXOF` otherwise wedges the UART. `TXC` is cleared when a chunk is armed, not after its last octet: a vector held past that octet's time on the wire would erase the chunk's only `TXC` |
| pad routing | a USART's pads are chosen in `GPIO->USARTROUTE[n]`, not in the USART: RX on PB0, TX on PB1 |
| locks | three blocks, three keys: `CMU` `0x93F7`, `WDOG0` `0xABE8`, `HFXO0` `0x580E`. A key in the wrong block leaves it locked and its writes silently ignored |
| oscillator | no enable bit: the mode decides, and `STATUS.RDY` (not `ENS`) says it runs |
| USART divisor | `CLKDIV = 256 × (ref / (16 × baud) − 1)`: 115 200 Bd from 38.4 MHz programs 5080 and runs at 115 142 Bd |
| watchdog | the bootloader arms 8 s on LFRCO and never disables it; the image re-arms 2 s. The period is 2^(n+3)+1 cycles of the selected clock |
| the millisecond | SysTick counts core cycles: 38 400 at 38.4 MHz. LFRCO (32.768 kHz) is no millisecond source; its shortest exact whole-millisecond period is 125 ms |
| priorities | four priority bits, one byte per interrupt; the SAMR21's arithmetic does not carry over |

Measured from the probe image's boot record: HFXO ready and selected, SysTick reload 38 399, `WDOG0` at 2 s, the line at 115 142 Bd.

### 10.4 The radio and its sources

The EFR32xG21 reference manual documents every peripheral at register level **except the radio**: its radio chapter is a block diagram. Silicon Labs' RAIL library, a binary, is the published way to drive it, and every published 802.15.4 firmware for Series 2 links it. This tree has two back-ends behind `hal/radio.h` (Figure 1): `rail`, the `rcp` default, and `own`, which drives the radio from registers.

![The ConBee III's two radio back-ends side by side under hal/radio.h. Left, rail: mg21_rail.c, the adapter, in amber; below it the RAIL library, hatched because it is fetched at build time and pinned, never in the tree; rail_shims.c answers the thirteen symbols the library expects from its host; RAIL owns the radio blocks, its own sequencer image, the calibration and the ten radio interrupts. Right, own: mg21_radio.c, which polls; it installs the sequencer program assembled by tools/mg21_seq.py and the register tables generated by tools/mg21_radio_gen.py from the stock bring-up's recorded values. Both end in mg21_radio_off.c, which switches the radio off before every reset.](images/mg21-backends.png)

*Figure 1 — the two radio back-ends. Amber is the code that implements `hal/radio.h`; hatched is what this tree links but does not contain; dashed is generated. File names under `own/` drop their `mg21_` prefix. Source: `tools/diagrams/mg21_backends.py`.*

What this tree takes from where:

| source | used for |
|---|---|
| the EFR32MG21 data sheet and the EFR32xG21 reference manual | the part and every non-radio peripheral |
| the current SDK headers (Simplicity SDK, Zlib) | the peripheral register transcription `mg21_regs` checks against |
| gecko_sdk v4.0.0's device headers and the family's CMSIS SVD | the radio's control registers: the newer SDKs withdrew them. `tools/mg21_radio_gold.py` checks the two against each other (every offset and field agrees) and writes `test/data/mg21_radio_ref.h` |
| the RAIL library and its headers | the `rail` back-end; the PA curves behind its power lookup |
| dresden elektronik's bootloader and stock firmware | the flash layout, the reset record, and the register values of the stock radio bring-up that the `own` back-end's tables carry |
| measurements on a ConBee III | everything about the radio's behaviour, through the probe image's register monitor |

### 10.5 The `own` back-end

`src/hal/mg21/own/` drives the radio with a sequencer program and register sequences of this tree; no RAIL code is in an `own` build. No image selects it; it takes `-DDS_MG21_RADIO=own`. It receives and transmits; it does not acknowledge, assess the channel or set the power.

How it works:

- **The sequencer.** The radio controller runs a small program from SEQRAM and enters a per-state vector on every radio state transition. The instruction set is published nowhere; `tools/mg21_seq.py` records the encodings settled on the bench and assembles this tree's own program from them.
- **The data.** `tools/mg21_radio_gen.py` turns the program into `mg21_seq_image.c` and the stock bring-up's register values — only those reception needs — into `mg21_radio_cfg.c`. Both are generated; do not edit them. `SYNTH.FREQ` and the receive record are computed in `mg21_phy.c`, under its host suite.
- **Install.** The radio clock first (a radio access faults while it is gated), then the program, the vectors, the block enables, the bring-up table, the part's own IR calibration pair from `DEVINFO+0x248`, and the channel tune.
- **Receive.** The CPU polls the receive buffer; a record is the length octet, the PSDU without FCS, RSSI and a status octet. The FCS is recomputed, the address filter runs in software, and an overflow is recovered by the CPU.
- **Transmit** starts from `OFF`: the frame goes into the transmit buffer, the program brings the 10 dBm PA up, and a frame that asks for an Ack keeps the receiver on through the turnaround.
- **Before every reset** the radio is handed back (`Mg21Radio_Release`).

Measured with the `rcp` image on this back-end against a ConBee II: promiscuous receive 20 of 20 on two channels, the address filter right in every case, transmit 20 of 20, and an Ack-requesting frame answered.

The **register monitor** in the `cb3` probe image (`DS_MONITOR`) is the bench instrument this work runs on, because the ConBee III has no debug connection: HDLC-framed read, write and trace commands on the link, answered as debug records, with flash, the user data page and the security blocks read-only. `tools/mg21_mon.py` is the host side.

### 10.6 The `rail` back-end

`src/hal/mg21/rail/` implements `hal/radio.h` on RAIL. RAIL owns the radio blocks, its sequencer image, the calibration and the ten radio interrupts; the adapter owns the contract above it. The library is one pinned SDK release, fetched at configure time and never committed (`docs/build-and-flash.md`, `src/lib/PROVENANCE.md`).

- **Context.** RAIL's callback runs in the radio interrupts. There the adapter only copies a received frame into its ring and decides the Ack's Frame Pending bit; everything else happens in `Radio_Poll`.
- **The Ack** is RAIL's auto-Ack; Frame Pending is set at RAIL's data-request event from the source-match table (`docs/ack-pending-frame.md`). While the receive ring is full the auto-Ack is paused, decided once a frame passes the address filter, so a frame the ring cannot take goes unacknowledged and its sender retries (`docs/architecture.md`).
- **Time.** RAIL's 32-bit microsecond clock, extended in the main loop, is the radio clock. An RX timestamp is RAIL's end of the sync word.
- **Timed transmit.** RAIL's schedule names the start of the preamble, or with CCA the start of a fixed backoff. Beyond those, its own report of the sync word's end measures 19 µs late without CCA and 147.5 µs with it, within 1 µs, so the back-end leads by those amounts and the report then reads 0 to −1 µs. RAIL refuses a moment already gone, which completes as a channel access failure. The start runs through a PRS channel from the protocol timer.
- **AES.** `rail/mg21_aes.c` runs the block cipher on `RADIOAES`: 6.84 µs a block against 219 µs in software, so a secured Enhanced Ack costs 68 µs and a 121-octet frame 0.29 ms. It is a DMA engine with descriptors for key, mode and data, and it cannot be preempted, so one block runs with interrupts masked.
- **Enhanced Ack.** Enabled once a host sets a key, so a Zigbee host keeps its radio as it was: RAIL's `ENH_ACK` option, the data-request event for data frames too, and a 256 µs turnaround, the one Silicon Labs' OpenThread port gives it. The Ack is built at the early data-request event, from what has arrived of the frame.
- **The serial link is a limit on a timed frame.** The FTDI bridge's latency timer defaults to 16 ms on Linux, so a frame requested 8 ms ahead misses and one requested 20 ms ahead makes it.
- **Shims.** The archive expects sixteen symbols from its host (critical sections, `memcpy`, clock queries, chip revision, the PA conversion, and the PRS channel a scheduled transmission starts from); `rail/rail_shims.c` answers them from scratch.
- **The TrustZone attribution.** RAIL's sequencer reaches EMU and BUFC with non-secure transactions, which the reset attribution rejects: frames land in the buffer and are never delivered. The adapter opens the attribution before `RAIL_Init` (peripherals non-secure, `SAU.CTRL.ALLNS` set — both halves are needed) and closes it before every reset.
- **Power.** "Highest" resolves to the 10 dBm PA; the dBm-to-level lookup is this tree's, over the SDK's published curve.
- **Not yet done.** The crystal's CTUNE from the token is applied by neither back-end, and whether RAIL's Frame Pending decision always lands before the Ack is loaded is what the `ackLate` counter watches.

Where the CCA edge sits depends on the stick's position, because the host's USB port lifts the noise floor:

| stick position | quiet channel reads | CSMA-CA sends from |
|---|---|---|
| straight in the host's USB port | −67 to −70 dBm | −72 dBm: 1 to 2 dB to spare at the host's −71 |
| on a USB extension cable | −81 to −85 dBm | −86 dBm: 15 dB to spare |
| on a USB hub | −49 to −51 dBm | −55 dBm |

**Use an extension cable.** The ConBee II shows the same effect (`docs/hardware_samr21.md`).

### 10.7 The bootloader

The deSiLabs bootloader serves three products, told apart by the DeviceConfig in the user data page. Read from its source; the shipped binary is built from it. Figure 2 shows how a running image is updated.

![A sequence between GCFFlasher, the running image and the bootloader. The flasher sends the reset frame, WRITE_PARAMETER 0x26 for the watchdog TTL, to the running image. The image hands back its clocks, the UART and its pads, then lets the watchdog reset the board into the bootloader; this step is amber. About 2.3 s after the frame the bootloader sends its ID packet unsolicited, 81 82 with its version and the app CRC. The flasher sends the update request, 81 03, with size, target, type 61 and CRC-32, and the bootloader answers 81 83 with a status. For every page the bootloader sends a data request, 81 04 with offset and length, and the flasher answers 81 84 with one 256-byte page. The bootloader then installs: it checks, erases, decrypts and writes. The flasher sends an ID request, 81 02, and the bootloader's 81 82 carries the app CRC, which equals the image's: app checksum OK.](images/cb3-update.png)

*Figure 2 — updating a ConBee III. Amber is the hand-back. Source: `tools/diagrams/cb3_update.py`.*

- **The container** is nested: an outer GCF of type 60 carrying an inner container whose product magic must equal the DeviceID (`0xDEC0DE03` for the ConBee III) and whose image is AES-encrypted, type 61..65. A plain image (type 60 inside) is not installed. `docs/build-and-flash.md` has the packaging.
- **The layout:** bootloader `0x0000..0x3FFF`; the app from `0x4000` to `0x7BFF0`, where the bootloader writes a 16-byte AppInfo trailer; the staging area for an update to `0xF7FFF`; NVM3 above. The image guard is therefore 491 504 B.
- **The reset record** at the bottom of RAM (`0x20000000`, 32 bytes, signature `0xF00F`) carries the boot decision, so both `cb3` linker scripts keep `.data` and `.bss` above it.
- **The stay request `0x205` is no route:** it leaves the stick silent until a power cycle. An image hands the board to the bootloader as in Figure 2.
- **A cold boot** runs the full bootloader: it verifies the app, sends its ID unsolicited and waits about 1 s before starting it.
- **Read-back:** a ConBee III image cannot be read back over the link; what GCFFlasher checks is the app CRC the bootloader reports.

### 10.8 Board handles

| item | value |
|---|---|
| host link | USART0, TX `PB1`, RX `PB0`, 115 200 8N1, no flow control, through an FTDI bridge (VID `0x0403`, PID `0x6015`); on macOS `/dev/cu.usbserial-<serial>` |
| what the bridge means | the host sees a serial port, not the SoC: the tools take `<port>@115200`, and recovery runs through the bootloader's dialog, not a USB reset |
| link heartbeat | the image reports its receive path every 2 s, which tells a link that only transmits from one that hears nothing |
| button | `PA4`, low-active; the pull-up is on the board |
| watchdogs | the bootloader's (about 8 s) and the image's (2 s) |
| identity | the EUI-64 from the manufacturing token (§10.2); never rewritten |
| read-only probe | `tools/cb3_probe.py` reads a stock stick's version, state and parameters and refuses every command that writes |

### 10.9 The stock firmware's serial protocol

The vendor firmware is a coordinator: its serial protocol (SLIP framing, an additive 16-bit checksum) carries APS data, network state and persistent Zigbee parameters, with nothing at the 802.15.4 level. A `cb3` RCP is therefore a different firmware, not a modification of that one. What carries over is everything under the protocol: the flash layout, the container, the bootloader handshake, the pins and the identity. GCFFlasher opens the bootloader on a stock stick by writing parameter `0x26`; this tree's image recognises the same frame and hands the board over (§10.7).
