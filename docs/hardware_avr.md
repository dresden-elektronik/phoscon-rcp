# Hardware — ATmega256RFR2 (`cbrb1`, the ConBee I and RaspBee I)

The silicon facts behind `src/hal/avr/`: the ATmega256RFR2 with its on-die transceiver on dresden elektronik's deRFmega256-23M12 module, its bootloader, where the identity lives, and how its transceiver differs from the AT86RF233. Read it before changing anything under `src/hal/avr/`. Sources: Microchip's ATmega256RFR2 datasheet, the module's user manual V1.5, the vendor's bootloader source V1.02, GCFFlasher's source and the legacy deCONZ AVR firmware; a fact measured on a board says so.

Section numbers continue `docs/hardware_mg21.md`, so this document starts at §11.

## 11. ATmega256RFR2 — the ConBee I and RaspBee I silicon

### 11.1 The part and the module

| item | value |
|---|---|
| core | 8-bit AVR; the module's 16 MHz crystal |
| memory | 256 KiB flash in 256-byte pages, 32 KiB SRAM, 8 KiB EEPROM |
| flash layout | the application from `0x0`, the bootloader in the 8 KiB boot section at `0x3E000` (`docs/build-and-flash.md`, Figure 6) |
| radio | 2.4 GHz 802.15.4 on the die, its registers memory-mapped: AT86RF233 register `n` sits at `0x140 + n`, no SPI |
| front end | internal PA and LNA (about 20 dB), controlled by `PD6` (`PA_CSD`, front end on), `PG0` (`PA_CTX`, driven by the transceiver when `PA_EXT_EN` is set) and `PG1` (antenna select) |
| host link | UART0 on `PE0`/`PE1`, 8N1, 57 600 for the `rcp` image and 38 400 for `probe`: behind an FTDI bridge on the ConBee I, on the Pi header on the RaspBee I. Both run one image |
| power | the module can conduct 22.5 dBm; the products are sold at 10 mW, so the firmware's cap is the vendor's setting (§11.5) |

### 11.2 The bootloader

"RaspBee Bootloader" V1.02 sits in the boot section, and every reset enters it. It prints its banner and starts the application 0.5 s later unless the flasher's keys arrive first (Figure 1). Two builds are in the field, `basic` (seen on a RaspBee I) and `premium` (on a ConBee I); the board under either is the same, and a `GCFTool -b cbrb1` container installs under both.

![A sequence between GCFFlasher on the host and the V1.02 bootloader. The flasher resets the board, by FTDI CBUS on a ConBee I or GPIO17 on a RaspBee I; the bootloader answers with its banner, RaspBee Bootloader basic or premium, on every reset. The flasher then sends 1A 1C A9 AE in one write, unlocking and starting the update; a bracket marks that this must land within 0.5 s of the reset. The bootloader answers READY; the flasher sends the 10-byte header: size, target, type and CRC-8. For every page, the bootloader asks GET with the page number as a little-endian u16 and a semicolon, and the flasher sends 256 bytes, the last page padded with 0xFF; a reset inside this loop aborts the update. The bootloader then reports # VALID CRC, read back from flash, and STARTING APP, and jumps to address 0; on a bad image it reports # INVALID CRC, erases page 0 and stays in the bootloader.](images/avr-bootloader.png)

*Figure 1 — the bootloader dialog. Amber is the key write that opens the update, which must land inside the start window. Source: `tools/diagrams/avr_bootloader.py`.*

- **The container** is type 1, target 0: 14 header bytes, then the XTEA-encrypted image. The bootloader writes pages from address 0 whatever the target says, and checks a CRC-8 Dallas over the plain image after the write.
- **An image must have an even size that is not a multiple of 256.** The bootloader fills pages by 16-bit words, so an odd length loses its last byte; for a whole number of pages it asks for one page past the end, and GCFFlasher answers by restarting. `tools/gcf_check.py --board cbrb1` refuses both, and an image that would reach the boot section.
- **The boot contract:** the image is a flat binary linked at 0 below `0x3E000` with its reset vector at 0. It starts with the CPU at 8 MHz (the bootloader's prescaler), SRAM not cleared, and it leaves the lock bits, the boot section and the EEPROM alone.
- **Fuses** (production `CE 90 F8`): boot section 8 KiB, reset into the bootloader, `EESAVE` set, so the EEPROM — and the MAC in it — survives a chip erase.
- **A reset during the upload aborts it.** A partly written image fails its CRC and keeps rebooting into the bootloader, which stays reachable.

### 11.3 The EEPROM identity

The stick's MAC is the **basic set**, the last 32 bytes of the EEPROM (`0x1FE0..0x1FFF`): magic `0xDE90`, id, length 8, `mac[8]`, 18 reserved bytes, a CRC-16 over the first 12 bytes.

| id | CRC | MAC byte order |
|---|---|---|
| 1 | a proprietary CRC-16, stored in either byte order | big-endian |
| 2 | CRC-16/MCRF4XX (reflected `0x8408`, init `0xFFFF`), little-endian | little-endian |

The firmware reads only the basic set and writes nothing: `hal/avr/avr_ident.c` validates it under the `avr_ident` suite, accepting both versions, the `0xDE00` magic early modules wrote, and a MAC beginning `00:50:C2:FF:FF` that older modules stored the other way round. The rest of the EEPROM holds the vendor firmware's configuration sets and a bootloader record, all left alone.

### 11.4 The transceiver against the AT86RF233

The ATmega256RFR2 carries the AT86RF233's register set at `0x140 + n`: the state machine, the commands, the `TRAC_STATUS` codes, the address filter, CCA and ED control and the CSMA fields behave as `docs/hardware_samr21.md` describes them. Nothing carries over as code — `hal/avr/` has its own driver — but the knowledge does. What differs:

| aspect | AT86RF233 | ATmega256RFR2 |
|---|---|---|
| access | SPI command bytes | ordinary loads and stores |
| sleep and reset | `SLP_TR` and `/RST` pins | `SLPTR` and `TRXRST` bits in `TRXPR` |
| interrupts | one line; `IRQ_STATUS` cleared by reading it; `TRX_END` for both directions | eight vectors; write 1 to clear; `RX_END` and `TX_END` separate |
| received frame | PHR, PSDU, LQI, ED, RX status in one buffer read | the length in `TST_RX_LENGTH`, the PSDU and LQI in the buffer; ED from `PHY_ED_LEVEL`, the CRC from `PHY_RSSI.RX_CRC_VALID` |
| `RX_SAFE_MODE` | protects a frame until it is read | protects it until the bit is cleared: a receive path that leaves it set after a dropped frame keeps every later frame out (measured) |
| RSSI and ED base | −94 dBm | −90 dBm |
| transmit power steps | `0x0` = +4 dBm … `0xF` = −17 dBm | `0x0` = +3.5 dBm … `0xF` = −16.5 dBm, then the module's PA on top |
| clock output | `CLKM` for the MCU | none: the MCU clock comes from fuses and `CLKPR` |
| power | always on | gated by `PRTRX24`; the frame buffer is reachable only with it cleared |
| `PART_NUM` | `0x0B` | `0x94` |

### 11.5 The `cbrb1` image

How the image built here meets the part, and how each step is checked:

| step | what the image does | checked by |
|---|---|---|
| registers and vectors | its own register map (no avr-libc header); 77 vector slots, an undefined one reaching the panic path | the `avr_regs` suite, against avr-libc |
| clock | `CLKPR` cleared from the bootloader's divide-by-2 to 16 MHz | the link reads cleanly at 38 400 with `UBRR0 = 25`, which only 16 MHz gives |
| millisecond | Timer/Counter0, CTC at clk/64, `OCR0A = 249` | a 2 500 ms replay measures 2 498.3 ms on the bench |
| watchdog | 2 s, armed under the timed sequence and read back | `WDTCSR` reads `0x0F` |
| host link | USART0 re-initialised from the bootloader's state, interrupt driven, framing errors and overruns counted | the `avr_baud` suite; witnesses `UCSR0B 0x98`, `UCSR0C 0x06`, rate 38 462 |
| identity | the basic set read from the EEPROM (§11.3) | `HWADDR` returns the factory EUI-64 |
| reset | `HAL_SystemReset` drains the last octet and lets the watchdog reset into the bootloader | the bootloader's banner 356 ms after the flasher's frame |
| the flasher's reply | the ConBee I's FTDI port never disconnects, so the image sends the vendor firmware's reply to the reset frame; without it GCFFlasher waits out the 0.5 s window | the `flasher_trap` suite; GCFFlasher reports `command UART reset done` |
| radio | the ConBee II's scheme: `RX_ON` in every mode, the filter and the Ack composed in the `RX_END` vector (`TX_START` 176 µs after the frame), transmission through `TX_ARET_ON` | 300 of 300 Ack-requesting frames acknowledged first time, none late |
| power | capped at the vendor's setting, index `0x0E` (−11.5 dBm at the transceiver) | the radiated level is not measured |

The images' flash and SRAM sizes are the figures of `docs/build-and-flash.md`.

Measured on a **ConBee I** against ConBee IIs and a ConBee III on channel 22: every cell of `tools/cb3_rcp_check.py --baud 57600`, `tools/rcp_pair.py` (the nine Frame Pending cells included) and `tools/rcp_matrix.py`. An immediate Ack starts 212 to 230 µs after the frame on a third stick's clock, idle or back to back, with an empty or a full source table. A Phoscon Zigbee Stack coordinator and router form a network through it in either role. The **RaspBee I** on a Raspberry Pi 5 passes `tools/rcp_matrix.py` against a ConBee II and a ConBee III, and a Phoscon Zigbee Stack coordinator and router form a network through it in either role. The vendor app `0x260B0500` found on that RaspBee I ignores GCFFlasher's reset frame, so its first update needs the reset line (`docs/build-and-flash.md`).

### 11.6 Open questions

| open | what settles it |
|---|---|
| how the ConBee I's board differs from the RaspBee I's (PA pin, reset wiring, LEDs) | the schematic |
| the RSSI and ED offset on a board (base −90 dBm against the AT86RF233's −94) | a measured comparison against a ConBee II |
| the dBm the module radiates at the vendor's setting | a measurement; the manual's figure is an image |

### 11.7 Thread host requirements

The `rcp` image carries what a Thread 1.4 host requires, and a Zigbee host, which sets no keys, sees none of it. **It runs on a ConBee I: a stock Thread 1.4 host starts on it with no skip flag, leads a network with a ConBee II as its child, and carries pings of 1 232 octets both ways, in about 1 s. The unmodified Home Assistant app runs on it at `baudrate` 57600, and a Matter plug switches through it, the command taking about 170 ms and the state about 290 ms. No Thread host has run on a RaspBee I yet.**

| what | how the image does it |
|---|---|
| host link | 57 600, the slowest rate the Home Assistant app offers. 115 200 is 2.1 % off at 16 MHz, outside the USART's error limit, and 1 000 000 leaves the USART 30 µs against vectors that mask interrupts for hundreds |
| what waits for that link | 4 096 octets: a full frame takes about 30 ms to leave and a neighbour's fragments arrive every 5 ms, and a frame the ring refuses has already been acknowledged |
| block cipher | the transceiver's AES engine, a block holding the interrupts for 40 µs |
| clock | Timer/Counter1 at 16 MHz, its 4 096 µs wraps counted by a vector, read as 32-bit microseconds |
| transmit | `TX_ARET` as before; a version 2 frame asking for an Ack once a host has set keys, and a timed frame, take the ConBee II's own path (`docs/hardware_samr21.md`) |
| Enhanced Ack | composed in the `RX_END` vector, started without its MIC, which follows into the buffer while the header is on air |

**The Ack's slot.** The Enhanced Ack misses the 176 µs of an immediate Ack: on an AVR core stepped in QEMU, `TX_START` comes 189 to 217 µs after the frame with a short source table, and up to 251 µs with a full one; the board reports 209 µs. That is inside the 441 µs this firmware allows and the 864 µs a sender waits. Under serial load, 4 131 of 4 131 frames were answered and none abandoned. Only the 38 octets the Ack can overwrite are copied ahead of the slot; the frame's tail stays in the buffer until the MIC is in.

**The MIC.** With nothing else running it is in the buffer 81 to 86 µs before its first octet reaches the air. It takes some 550 µs, so it runs with the short vectors let in: the USART holds three octets, about 520 µs at this rate. Those vectors can use up the margin: under steady serial traffic an Ack to an extended source took 1 052 µs, a first octet late, and about one in 200 went out with a wrong MIC. `DBG_RF_RSN_ISR_MIC_CYCLES` reports the longest since the last report, every 30 s.

**Flashing it.** The `rcp` image listens at 57 600 and the bootloader at 38 400, so the image cannot answer a reset frame at 38 400, and a replug does not help: the bootloader's 0.5 s can pass before the host has the port. GCFFlasher 4.12.4 or newer sends the frame at 38 400, then at 57 600, and uploads at 38 400. If neither is answered, it resets a ConBee I over CBUS0 of its FT230X, from macOS and Windows, and a RaspBee has GPIO17. With an older flasher, `tools/cbrb1_flash.py` sends the reset frame at 57 600 and uploads at 38 400 itself.

| open | what settles it |
|---|---|
| hours under the Home Assistant app, and a sleepy child of this stick itself | a long run, with no other router in the network for the child to pick |
| commissioning a new Matter device through it | a run with a phone |
| a RaspBee I | the same image on its Pi |
| GCFFlasher 4.12.4 over the `rcp` image from Linux or on a RaspBee I; it has run on a ConBee I from Windows and macOS | a flash on each |
| CSL | unlikely behind this link: a 127-octet frame takes about 30 ms to cross it, against a stock host's 2 ms lead. Timed frames left on time at one every 60 ms and not all at one every 30 ms; their interval is mostly within 6 µs, with a 12 µs outlier seen |

### 11.8 The boards

The first generation, no longer sold, served by one `cbrb1` image.

| | ConBee I | RaspBee I |
|---|---|---|
| host link | UART0 behind an FTDI bridge, 57 600 | UART0 on the Pi header, 57 600 |
| reset into the bootloader | FTDI CBUS, driven by the flasher | GPIO17, driven by `tools/rb2_flash.py` |
| bootloader | RaspBee Bootloader V1.02 (`premium` seen) | the same (`basic` seen) |
| identity | the EEPROM's basic set | the same |

What differs from the SAMR21 boards, as it bears on a port:

| aspect | SAMR21 boards | AVR boards |
|---|---|---|
| core | 32-bit Cortex-M0+ | 8-bit AVR: `int` is 16 bits, checked for the portable code by the `avr_int` suite |
| radio access | SPI to a separate chip | memory-mapped on the die |
| what a flash must keep | the last 4 KiB of flash | nothing in flash: the identity is in the EEPROM, which a chip erase may take |
| clock | 48 MHz from the radio's CLKM | 16 MHz from the module crystal, after clearing the bootloader's divide-by-2 |
