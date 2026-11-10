# Hardware — ESP32 802.15.4 parts (`xiaoc6`, experimental)

The silicon facts behind `src/hal/esp32/`: Espressif's RISC-V parts with an 802.15.4 radio, the layer this tree shares between them, the app contract of the ESP-IDF bootloader the boards keep, and the boards. The ESP32 targets are an experiment, not products of the family (`docs/boards.md`). Read it before changing anything under `src/hal/esp32/`. Sources: the ESP32-C6 Technical Reference Manual v1.2 (TRM), ESP-IDF v5.4.1 (`components/soc`, `hal`, `bootloader_support`, `riscv`), esptool 5.6.0 and Seeed Studio's XIAO ESP32-C6 wiki; a fact measured on a board says so.

Section numbers continue `docs/hardware_avr.md`, so this document starts at §12.

## 12. ESP32 802.15.4 parts

The probe image runs on the XIAO ESP32-C6 and can bring its PHY up; the 802.15.4 MAC is not driven yet, and no `rcp` image is offered. The TRM documents every block the probe image uses but not the 802.15.4 radio: its registers are public only as ESP-IDF's `ieee802154_reg.h` and driver, and its RF bring-up and calibration only as Espressif's closed `libphy.a` and `libbtbb.a`.

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

The link has no line rate and no framing errors; a host that is not reading leaves the IN FIFO full, the chunk waits and the record queue drops its oldest. The USB start-of-frame number moves while a host is attached, which the heartbeat reports: on the bench board it advances by 2000 frames between two 2 s heartbeats, the host's millisecond and the board's agreeing.

Two audits of `docs/testing.md` §5 do not run as written on this board:

| audit | here |
|---|---|
| D, the container against the vendor's | there is no container; `tools/esp32_check.py` parses the app image itself, and agrees with `esptool image-info` on this tree's image and on the board's factory ESP-IDF app |
| S, an interrupted update keeps the old app | not runnable: the partition table has one app slot, so an interrupted write leaves no app. Recovery is the ROM's download mode, which needs no button |

### 12.5 The radio's PHY

The PHY's bring-up runs on the bench XIAO: a probe image built with `-DDS_ESP32_PHY_PROBE=ON` opens the modem clocks and calls the archive's entry point, which returns after a full calibration of 45 ms, then the baseband's, and the board keeps running. That the RF path works is not shown until the 802.15.4 MAC, not driven yet, receives a frame.

| item | state |
|---|---|
| the MAC's registers | ESP-IDF's `soc/esp32c6/register/soc/ieee802154_reg.h` and `components/ieee802154/driver`, Apache-2.0: read, never copied |
| the libraries | `esp-phy-lib`'s `esp32c6/libphy.a` and `libbtbb.a` at the commit ESP-IDF v5.4.1 pins, Apache-2.0, fetched with the ROM address lists of the same tag (`src/lib/PROVENANCE.md`). They reach 94 outside symbols: 87 in the ROM's PHY list, `ets_delay_us` in its main list, `__divdi3` in libgcc, and five the host supplies, `memcpy`, `memset`, `sprintf`, `phy_printf` and `rtc_clk_xtal_freq_get` (`hal/esp32/phy/phy_shims.c`). Neither reaches a coexistence or Wi-Fi library. Their RAM code is in ESP-IDF's `.iram1` sections, which the linker script places with this tree's |
| the modem clocks | the gates ESP-IDF opens for its PHY and 802.15.4 modules: the analog I2C master and coexistence in `MODEM_LPCON`; the front end's APB, 80, 160 and calibration clocks, the shared BT/802.15.4 baseband, the 802.15.4 APB and MAC clocks and the event matrix in `MODEM_SYSCON` |
| the ROM's delays | they count CPU cycles at the rate the bootloader set; `ets_update_cpu_frequency` gets the 160 MHz before the PHY runs |
| the entry point | `register_chipv7_phy(init table, calibration record, full)`: the init table is ESP-IDF's default for the chip with its 20 dBm cap, evaluated by the fetch tool; the record starts empty, so the call answers 1, "the record failed its check", and calibrates in full. Every boot does, since the firmware keeps nothing |
| the baseband | `bt_bb_v2_init_cmplx`, after the PHY |
| not done | the PHY's PLL tracking, which ESP-IDF repeats from a timer; transmit power per channel |
