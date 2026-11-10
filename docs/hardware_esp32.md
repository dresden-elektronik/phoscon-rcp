# Hardware — ESP32 802.15.4 parts (`xiaoc6`, experimental)

The silicon facts behind `src/hal/esp32/`: Espressif's RISC-V parts with an 802.15.4 radio, the layer this tree shares between them, the app contract of the ESP-IDF bootloader the boards keep, and the boards. The ESP32 targets are an experiment, not products of the family (`docs/boards.md`). Read it before changing anything under `src/hal/esp32/`. Sources: the ESP32-C6 Technical Reference Manual v1.2 (TRM), ESP-IDF v5.4.1 (`components/soc`, `hal`, `bootloader_support`, `riscv`), esptool 5.6.0 and Seeed Studio's XIAO ESP32-C6 wiki; a fact measured on a board says so.

Section numbers continue `docs/hardware_avr.md`, so this document starts at §12.

## 12. ESP32 802.15.4 parts

Both images run on the XIAO ESP32-C6: the probe image, which drives no radio, and the `rcp` image, whose radio back-end passes the ConBee II rigs of `docs/testing.md` §3 (§12.6). The TRM documents every block the images use but not the 802.15.4 radio: its registers are public only as ESP-IDF's `ieee802154_reg.h` and driver, and its RF bring-up and calibration only as Espressif's closed `libphy.a` and `libbtbb.a`.

### 12.1 The family layer

One directory, `src/hal/esp32/`, drives the blocks these parts share; one directory per chip, `src/hal/esp32/<chip>/`, holds the numbers; the board's include path picks the chip. The split rests on a field-by-field comparison of ESP-IDF's register headers:

| block | ESP32-C6 → ESP32-H2 | ESP32-C6 → ESP32-C5 | in this tree |
|---|---|---|---|
| 802.15.4 MAC, base `0x600A3000` on all three | the same 156 fields; two debug registers added | the same | one driver |
| USB Serial/JTAG, base `0x6000F000` | the same | two registers added | `esp32_usj.c` |
| SYSTIMER | the same; base `0x6000A000` → `0x6000B000` | the same | `esp32_clock.c`, base per chip |
| timer-group watchdog | the same; base `0x60008000` → `0x60009000` | four fields differ | `esp32_wdt.c` |
| CPU interrupt controller | the same PLIC | a CLIC, nesting in hardware | `esp32_irq.c` for the PLIC; a CLIC part brings its own |
| interrupt sources (MAC, USB, SYSTIMER, TG0 WDT) | 12, 48, 57, 53 → 21, 37, 45, 42 | 12, 54, 61, 58 | per chip |
| eFuse | 21 fields differ | moved to `0x600B4800` | the MAC words per chip, the EUI-64 shared |
| crystal, PLL | 40 → 32 MHz, 480 → 96 MHz | 40 or 48 MHz | per chip |

`esp32_regs.h` carries the family's offsets and fields, `<chip>/esp32_chip.h` the bases, sources, eFuse words and memory bounds; `test/unit/test_esp32_regs.py` compares all of them with ESP-IDF's headers.

### 12.2 The ESP32-C6

| item | value |
|---|---|
| core | RV32IMAC, up to 160 MHz, single HP core plus an LP core; machine mode only here |
| memory | 512 KiB HP SRAM at `0x40800000`, instruction and data alike; flash mapped through a 64 KiB-page MMU into one window from `0x42000000` shared by both buses |
| clocks | 40 MHz crystal; SYSTIMER counts at 16 MHz from it whatever the CPU runs at. The HP root is the 480 MHz PLL divided by 3, 160 MHz; the CPU divides it by 1, 2 or 4, AHB by its own divider |
| identity | the factory MAC in eFuse block 1: the low 32 bits at `EFUSE + 0x44`, the high 16 and `MAC_EXT` at `+0x48`. The EUI-64 is MAC[47:24], then `MAC_EXT` low byte first, then MAC[23:0], as ESP-IDF composes it for 802.15.4 |

The bench XIAO's part reads as ESP32-C6FH4 revision v0.2 with 4 MB of embedded flash. Its `MAC_EXT` reads `FF FE`, the usual value, because the eFuse holds it: no constant is inserted, and the host suite pins a vector where it is not.

### 12.3 Boot, interrupts and watchdogs

The board's ESP-IDF second-stage bootloader stays, and the image is an ESP-IDF app in the board's app slot. What the bootloader requires, from its v5.4.1 source:

| requirement | how the image meets it |
|---|---|
| segment 0 is flash-mapped and begins with the app descriptor (magic `0xABCD5432`); without it the image is refused | `esp32_app_desc.c`, in `.flash.appdesc`, which esptool's `elf2image` puts first |
| exactly two flash-mapped segments on a part whose buses share the window | read-only data from `0x42000020`, code from `0x42100020` (`<chip>/esp32c6.ld`) |
| a flash segment's file offset equals its address modulo the 64 KiB page | `elf2image` pads |
| RAM segments end below the bootloader's own loader segment, `0x4086E610` on the C6 | the linker region ends there; 442 KiB |
| the descriptor's eFuse block revision range admits the chip, its MMU page size is the image's | 0 to 99, 64 KiB |
| checksum and appended SHA-256 | `elf2image`; the bootloader validates them on every boot |

The bootloader loads the RAM segments, maps the flash ones with the cache on, and calls the entry point on its own stack, with its own `mtvec` and interrupts off. Memory protection it sets is unlocked, so machine mode is not bound by it. The startup takes its own stack and vectors first; there is no `.data` to copy.

- **Traps are vectored only.** `mtvec` is 256-byte aligned; an exception enters at its base and CPU line n at base + 4n (TRM 1.6.2). A peripheral source reaches a line through the interrupt matrix (map register at `INTMTX + 4 × source`), and the PLIC enables, types and prioritises the line. ESP-IDF drives that PLIC at `0x20001000`; the TRM describes the same controller as INTPRI at `0x600C5000` with another register order. The bench board's interrupts run through `0x20001000`.
- **Every line is level-type and nothing nests.** The core clears MIE on entry; the dispatcher calls the line's handler, which clears its own source. Code an interrupt runs is placed in `.iram.text`, so a cache miss never lands in it.
- **Watchdogs.** The ROM arms the RTC watchdog and MWDT0 for the flash boot; the bootloader ends both, and ESP-IDF's default configuration re-arms the RTC watchdog for the app. The bench XIAO's bootloader leaves it off. The image switches it off either way, and records it when it was on, and arms MWDT0: a system reset 2 s after the last feed, counted from the crystal. The super watchdog stays fed by hardware, as the bootloader left it.
- **The CPU clock.** The bench XIAO's bootloader leaves the root on the PLL, the CPU at 80 MHz (its divider 2) and AHB at 40 MHz. The image sets the CPU's divider to 1, 160 MHz, and touches nothing else: the PLL already runs, and the timer, the watchdog and the link do not count in CPU cycles. The witnesses report the registers and the frequency they make.
- **Reset** is the HP system's software reset (`LP_AON_SYS_CFG`, bit 31): the ROM and the bootloader run again. The flasher trap's reset frame lands there too. The reset cause reads 3 after it, 21 after esptool's line-state reset. It, the panic mark and the handed-over `mstatus` are reported from `HAL_Init`, inside the boot set the loop replays, because a host on this link is rarely attached when the board resets.

### 12.4 The XIAO ESP32-C6

| item | value |
|---|---|
| host link | the chip's USB Serial/JTAG controller on the USB-C socket: CDC-ACM, VID `0x303A`, PID `0x1001`, the chip's MAC as its serial number; it enumerates in hardware |
| flash layout | bootloader `0x0`, partition table `0x8000`, `nvs` `0x9000`, `otadata` `0xE000`, the app slot `ota_0` `0x10000`, 2 MiB, then `spiffs` and `coredump`; one app slot, no factory slot |
| RF switch | GPIO3 low enables it; GPIO14 low selects the ceramic antenna, high the U.FL socket. The image drives both at boot, the ceramic antenna unless built with `-DDS_ESP32_ANTENNA_UFL=ON` |
| user LED, BOOT | GPIO15, which the probe image toggles with every 2 s heartbeat of the loop; GPIO9, held while plugging in for the ROM's download mode |
| UART0 | TX GPIO16, RX GPIO17, on the edge pins |

The pins are the board's data, in `targets/xiaoc6/esp32_board.h`; `hal/esp32/esp32_gpio.c` drives them.

The USB link resets the chip on its CDC line state (TRM Table 32.3-2):

| RTS | DTR | the controller |
|---|---|---|
| 0 | 0 | clears the download-mode flag |
| 0 | 1 | sets the download-mode flag; a reset with it set enters the ROM's download mode |
| 1 | 0 | resets the chip |
| 1 | 1 | nothing |

A host that opens the port with both lines asserted, pyserial's default and a plain open, leaves the board running, and so does closing it. One that lowers DTR before RTS passes through RTS=1/DTR=0 and resets it; pyserial with `dtr=False, rts=False` does exactly that on the bench board. esptool uses the same states to reach download mode with the running image untouched, so no button is needed to flash.

The link has no line rate and no framing errors; a host that is not reading leaves the IN FIFO full, the chunk waits and the record queue drops its oldest. The FIFO takes no octet while a packet waits for the host, and only `SERIAL_IN_EMPTY` says the host took it, so the driver pumps from that interrupt: a reply whose chunk met a full FIFO otherwise left the board with the next reply, every Spinel answer one request late. A 64-octet packet is followed by a zero-length one, without which a host holds it, as ESP-IDF's driver does. The USB start-of-frame number moves while a host is attached, which the heartbeat reports: on the bench board it advances by 2000 frames between two 2 s heartbeats, the host's millisecond and the board's agreeing.

Two audits of `docs/testing.md` §5 do not run as written on this board:

| audit | here |
|---|---|
| D, the container against the vendor's | there is no container; `tools/esp32_check.py` parses the app image itself, and agrees with `esptool image-info` on this tree's image and on the board's factory ESP-IDF app |
| S, an interrupted update keeps the old app | not runnable: the partition table has one app slot, so an interrupted write leaves no app. Recovery is the ROM's download mode, which needs no button |

### 12.5 The radio's PHY

The `rcp` image brings the PHY up at boot, before its radio back-end (§12.6). The sequence, written from scratch after reading ESP-IDF's `esp_phy`, `modem_clock` and start-up code:

| step | why |
|---|---|
| the modem clock domains' ICG maps: each domain set to run in the PMU's active state (and its modem state where ESP-IDF's initial map has it) | the bootloader opens only `MODEM_APB`, the I2C master and `LP_APB`; ESP-IDF opens the rest when its first radio comes up. Left gated, the coexistence arbiter breaks the first receive off and the MAC hears nothing after it, whatever the clock enables say |
| the modem clock gates ESP-IDF opens for its PHY and 802.15.4 modules, and the modem security and BLE-timer clocks | the analog I2C master and coexistence in `MODEM_LPCON`; the front end, the shared BT/802.15.4 baseband, the 802.15.4 APB and MAC clocks and the event matrix in `MODEM_SYSCON` |
| a reset pulse over the radio's modem blocks | a software reset does not power the modem domain down, so its blocks are as their last user left them |
| `ets_update_cpu_frequency(160)` | the ROM's delays count CPU cycles at the rate the bootloader set |
| `phy_init_param_set(1)` | the PHY serves Wi-Fi, BLE and 802.15.4 (ESP-IDF's `SOC_PHY_COMBO_MODULE`), and must not come up in Wi-Fi receive |
| `register_chipv7_phy(init table, empty record, full)` | the archive's entry point: answers 1, "the record failed its check", and calibrates in full in about 45 ms. Every boot does, since the firmware keeps nothing |
| `bt_bb_v2_init_cmplx`, then one `phy_param_track_tot(0, 1)` | the baseband, and the PLL tracking, which ESP-IDF repeats every second from a timer and the radio back-end from its poll, between transmissions; a round takes about 5 µs |

| item | state |
|---|---|
| the libraries | `esp-phy-lib`'s `esp32c6/libphy.a` and `libbtbb.a` at the commit ESP-IDF v5.4.1 pins, Apache-2.0, fetched with the ROM address lists of the same tag (`src/lib/PROVENANCE.md`). They reach 94 outside symbols: 87 in the ROM's PHY list, `ets_delay_us` in its main list, `__divdi3` in libgcc, and five the host supplies, `memcpy`, `memset`, `sprintf`, `phy_printf` and `rtc_clk_xtal_freq_get` (`hal/esp32/phy/phy_shims.c`). Neither reaches a coexistence or Wi-Fi library. Their RAM code is in ESP-IDF's `.iram1` sections, which the linker script places with this tree's |
| the init table | ESP-IDF's default for the chip with its 20 dBm cap, evaluated by the fetch tool |
| not done | transmit power per channel |

### 12.6 The 802.15.4 MAC

`hal/esp32/esp32_radio.c` is the `rcp` image's radio back-end. On the bench XIAO against a ConBee II, on channels 26 and 25:

| rig | result |
|---|---|
| `tools/cb3_rcp_check.py` | every cell: promiscuous receive, the retune, the filter, transmit, `OK` and `NO_ACK` |
| `tools/rcp_pair.py` | every cell, both ways: the filter per mode, the Ack policy, the Frame Pending bit, frames longer than one USB packet |
| `tools/rcp_matrix.py` | every cell, both directions |
| frames of 20 to 125 octets to the XIAO | all reported byte-identical |
| broadcast bursts from the XIAO, each reported sent | at −10 dBm, 17 of 360 lost on channel 26, 1 of 120 on channel 25 and 1 of 280 on channel 11, though the same receiver hears it about 9 dB stronger on 26 than on 11; none of 120 on 26 at +10 dBm. ConBee II to ConBee II loses none of 160, on 26 at −12 dBm (its cap there) and on 11 at −10 dBm. The loss is the XIAO's on channel 26, the PLL tracking every second leaves it as it was, and its cause is open |

| fact | consequence |
|---|---|
| the TRM has no chapter on it | `hal/esp32/esp32_radio.c` is written from ESP-IDF's `ieee802154_reg.h` and its driver; the MAC's layout is the same on the C6, H2 and C5 below its debug registers (§12.1) |
| it writes received frames to RAM by DMA, and the access filters (APM) admit only the HP CPU until an app clears them, which ESP-IDF's app start does and its bootloader does not | `HAL_Init` clears them; without it the MAC takes a frame in, its `RX_LENGTH` says so, and the buffer stays empty |
| the receive buffer holds the PHR's length octet and the PSDU, whose two FCS octets are replaced by the frame's RSSI, signed dBm, and LQI | the MAC checks the CRC itself and does not keep it; the loop computes the FCS again before it hands a frame up, so a host sees the PSDU on air |
| the channel is its `HOP` field, MHz above 2402: 3 + 5 (channel − 11) | — |
| a receive abort raises an event only for the reasons enabled in `RX_ABORT_INTR_CTRL`; a bad CRC or a preamble with no frame it recovers from by itself and counts in its own counters. After a coexistence break it stays idle | the driver enables that reason too, and re-arms |
| the coexistence arbiter's priorities for the MAC are set as if no other radio were there, as ESP-IDF does without its coexistence library | no coexistence library is linked |
| on transmit, the buffer is the length octet, counting the two FCS octets the MAC appends, then the PSDU without them; an Ack Request makes the MAC wait for the Ack, which it receives into the receive buffer with its hardware Ack reception on, and a missing one ends in transmit abort 16 | the driver waits in its own state for `ACK_RX_DONE` or the abort |
| `STOP` ends a receive or a transmission, and a receive takes a new channel only from a fresh receive command after one | the driver stops the MAC before every command and waits, bounded, until `TXRX_STATE` reads idle |
| `TX_DONE` and the Ack's event, or the Ack-timeout abort, can arrive in one interrupt | the vector handles them in that order in one pass |
| it has no CSMA-CA of its own: `CCA_TX_START` is one CCA and the transmission after it | the backoff is software's, on the MAC's timer 0, with a random count from the chip's RNG; retries too |
| register `0x6C` holds the next hardware Ack's Frame Pending bit in bit 0 and the Ack-transmit timeout in `[31:16]` | only bit 0 is written: a whole-word write clears the timeout, and every hardware Ack then fails |
| with its hardware Ack transmission on, it acknowledges a 2003 or 2006 frame that passes its address filter by itself, and raises `ACK_TX_DONE`; told to receive again before then, it sends the Ack and idles | the driver waits for `ACK_TX_DONE`, or the two receive aborts that end an Ack, before it re-arms. 2015 frames' Enhanced Acks are software's, as ESP-IDF builds them |
| CCA is energy-based, at ESP-IDF's −60 dBm over 8 symbols; an energy measurement's result is `ED_SCAN_CFG`'s `ED_RSS`, in dBm | — |
| transmit power is a 4-bit index: 15 for +20 dBm, else 3 + (dBm + 15) / 3 from −15 dBm (ESP-IDF's mapping) | — |
| the LQI it reports is a small number, 10 or 11 for a frame at −40 dBm | its scale is the hardware's; how it maps onto 802.15.4's 0..255 is open |
