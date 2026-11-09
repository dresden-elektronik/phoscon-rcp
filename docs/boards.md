# Boards

The family this firmware serves, what differs between its boards, and how a board plugs into the tree. Read it before porting to a board or choosing one. The silicon facts, each board's bootloader and handles included, are the hardware docs (`docs/hardware.md`).

| § | section |
|---|---|
| 1 | [The family](#1-the-family) |
| 2 | [Board differences](#2-board-differences) |
| 3 | [Shared and per-board code](#3-shared-and-per-board-code) |
| 4 | [The target model, and adding a board](#4-the-target-model-and-adding-a-board) |

## 1. The family

Four build targets cover the five products of the ConBee/RaspBee line (Figure 1).

![Three columns joined by arrows. Products, each with its host link: ConBee II, USB native CDC; RaspBee II, Pi header UART at 115 200 baud; ConBee III, USB through an FTDI bridge at 115 200; ConBee I, USB through an FTDI bridge at 57 600; RaspBee I, Pi header UART at 57 600 baud. Each points at its build target, in amber: cb2, rb2, cb3, and cbrb1, which both the ConBee I and the RaspBee I share. The targets point at their silicon layer: cb2 and rb2 at hal/samr21/ (ATSAMR21E18A with an AT86RF233), cb3 at hal/mg21/ (EFR32MG21, radio on chip), cbrb1 at hal/avr/ (ATmega256RFR2, radio on die).](images/family.png)

*Figure 1 — one tree, five products. A build names a target (`-DDEBOARD=`, amber); a target names the silicon layer its HAL lives in. Source: `tools/diagrams/family.py`.*

## 2. Board differences

| board | MCU and radio | host link | identity | bootloader, app origin |
|---|---|---|---|---|
| ConBee II (`cb2`) | ATSAMR21E18A + AT86RF233 | native USB CDC (VID `0x1CF1`, PID `0x0030`) | NVM user row; the deCONZ NVRAM in the last 4 KB of flash | GCF V2.07; `0x5000` |
| RaspBee II (`rb2`) | the same | SERCOM5 UART on the Pi header, 8N1, 115 200 for the `rcp` image and 38 400 for `probe` | the same | GCF V3 at 38 400; `0x5000`, a type-31 container |
| ConBee III (`cb3`) | EFR32MG21, radio on die | USART0 over an FTDI bridge, 115 200 8N1 | the user data page's manufacturing token | deSiLabs; `0x4000`, a nested AES container (`docs/hardware_mg21.md`) |
| ConBee I, RaspBee I (`cbrb1`) | ATmega256RFR2, radio on die | UART0, 8N1, 57 600 for the `rcp` image and 38 400 for `probe`: FTDI bridge, or the Pi header | the EEPROM's basic set | RaspBee Bootloader V1.02 at the top of flash; `0x0` |

Where each board's flash may be written is Figure 5 of `docs/build-and-flash.md`.

## 3. Shared and per-board code

| part | scope | where |
|---|---|---|
| the RCP core: HDLC, Spinel, the session, properties, MAC, scan | shared, host-tested | `src/rcp/` |
| the debug-record queue, the UART link adapter | shared | `src/hal/dbg.c`, `src/com/uart.c` |
| SoC services, the radio driver, the UART driver | per silicon | `src/hal/samr21/`, `src/hal/mg21/`, `src/hal/avr/` |
| identity, linker layout, image guard, which sources are compiled | per board | `src/targets/<board>/` |

## 4. The target model, and adding a board

A board is data plus the paths that name its silicon and its link. `-DDEBOARD=<board>` includes `src/targets/<board>/board.cmake`; every file the build compiles comes from that file or from the shared core, and a board that leaves out a required setting fails at configure time.

| file in `src/targets/<board>/` | provides |
|---|---|
| `board.cmake` | the sources (`DS_STARTUP`, `DS_HAL_SOURCES`, `DS_LINK_SOURCES`), include paths (`DS_INCLUDE_DIRS`: the silicon layer, and the core's `hal_core.h` directory), the linker script and the flash layout (`DS_DEFAULT_ORIGIN`, `DS_IMAGE_BOUND`); for a vendor radio library, its compile and link options |
| `board.c`, `board.h` | the descriptor behind `src/board/board.h`: identity, link, radio, flash layout |
| `product.h` | the identity on the wire |
| linker scripts | one per app origin |
| `image_guard.cmake` | the build-time size guard |

The startup file and the `hal/<silicon>/` directory belong to the silicon, not the board, and live outside it. A board with a different core also needs its own toolchain file.

Three seams a port easily misses:

- **A wired link is one adapter and one driver.** `com/uart.c` implements the host link over the driver contract `hal/uart_drv.h`; each silicon supplies the driver. A different UART replaces the driver and keeps the adapter.
- **Some `hal/hal.h` entries are SAMR21 facts** — waking the separate radio chip, the bootloader's update flag, the USB serial. A board without them leaves them undefined, so reaching for one is a link error, not a routine that returns a lie.
- **The core reads three descriptor fields:** `ncpVersion`, `linkBaud`, `appOrigin`. The rest is the board's own data.
