# Architecture

How the firmware is put together: the two images, the layers, the data path in each direction, the buffers, the state machines and the interrupt rules. Read it before changing code. The wire contract is `docs/protocol.md`, the silicon facts are the hardware docs, and the boards are `docs/boards.md`.

| § | section |
|---|---|
| 1 | [Overview and the two images](#1-overview-and-the-two-images) |
| 2 | [Layers and modules](#2-layers-and-modules) |
| 3 | [RX path](#3-rx-path) |
| 4 | [TX path](#4-tx-path) |
| 5 | [Buffers and pools](#5-buffers-and-pools) |
| 6 | [State machines](#6-state-machines) |
| 7 | [Concurrency and ISR rules](#7-concurrency-and-isr-rules) |
| 8 | [Debug records](#8-debug-records) |
| 9 | [Build-time options](#9-build-time-options) |

## 1. Overview and the two images

The host runs the network stack, Zigbee or Thread; the firmware implements only the 802.15.4 PHY/MAC surface, speaks HDLC-framed Spinel over the board's host link, and keeps no state across a reset (Figure 1).

![Two stacks side by side over one board bar that names the ConBee I, ConBee II, ConBee III, RaspBee I and RaspBee II. Left, Phoscon RCP: the host holds the host application and the Zigbee stack, ZDO, APS and NWK, drawn in amber; a rule marked Spinel, HDLC frames over USB or a UART, separates it from the firmware, which holds Spinel, the MAC and the PHY. Right, the vendor deCONZ firmware: the host holds only deCONZ; a rule marked deCONZ serial protocol, APS-level commands, sits lower, and the firmware holds ZDO, APS and NWK in amber, then the MAC and the PHY.](images/host-split.png)

*Figure 1 — the same radio, two firmware architectures. Amber is the Zigbee stack, the one thing that moves across the line. The vendor firmware holds the whole stack (Microchip's BitCloud); this one holds the radio and the Spinel contract. The containers differ accordingly: about 52 kB for the ConBee II RCP image against 163.6 kB for the vendor's. Source: `tools/diagrams/host_split.py`.*

One tree builds one of two images, selected by `DS_MODE`:

| image | `DS_MODE` | the host link carries |
|---|---|---|
| probe (default) | `probe` | 8-byte debug records (§8) |
| RCP | `rcp` | HDLC-framed Spinel; records travel in-band as `RCP_LOG_CRASH_DUMP` |

- **Portable core.** `rcp/` never touches hardware, so it runs in the host test suites; everything that does lives under `hal/`, one directory per silicon family.
- **No allocation, no blocking.** Every buffer is a fixed pool with a counted overflow rule, every multi-step activity is a state machine advanced one step per loop pass, and handlers do bounded hardware work only.
- Both run the same `app/main.c`; `DS_MODE` only decides which of `app/main_rcp.c` and `app/main_probe.c` supplies the loop hooks. No `#ifdef` selects between them.
- Both recognise GCFFlasher's reset request in the incoming byte stream (`app/flasher_trap.c`) and reset into the bootloader, so a board can be re-flashed without a replug.
- The identity on the wire is set per board in `targets/<board>/product.h`; `docs/protocol.md` lists every board's.

## 2. Layers and modules

Calls go down; events come back up through a contract. Above the board edge every board runs the same code; below it the tree splits by silicon, and only the silicon layer touches a register (Figure 2).

![The firmware's layers, top to bottom: app/ (main.c, main_rcp.c, main_probe.c, rcp_app.c, flasher_trap.c, monitor_app.c); the app_main.h contract; rcp/, the portable core and the only amber run (hdlc, spinel, session, props, props_stream, props_debug, mac, mac_sec, srcmatch, scan); the board edge, hal.h, radio.h, com.h, dbg.h and board.h; the shared links and queue (com/usb_cdc.c, com/uart.c, hal/dbg.c, usb/); the driver contracts uart_drv.h, mon_drv.h and hal_diag.h; then three silicon columns, each over its boards: hal/samr21/ for the ConBee II and RaspBee II, hal/mg21/ for the ConBee III with its own/ and rail/ radio back-ends and the hatched RAIL library, and hal/avr/ for the ConBee I and RaspBee I. lib/ stands beside every run; arrows say calls go down and events come back up.](images/layers.png)

*Figure 2 — the layer stack. Amber marks `rcp/`, the one run that is the same on every board and compiles on the host. Contracts sit on the rule between two runs. A silicon column's blocks drop the column's prefix (`radio` in `hal/avr/` is `avr_radio.c`). The hatched block is the RAIL library the ConBee III's `rcp` image links (`docs/build-and-flash.md`). Source: `tools/diagrams/layers.py`.*

- **The core/glue split** is the load-bearing rule (`docs/coding-rules.md`): nothing in `rcp/` includes a register header or calls `HAL_*` or `Radio_*`. That is what makes the wire behaviour testable without a board.
- **One silicon, one directory.** `hal/samr21/`, `hal/mg21/` and `hal/avr/` share no source. The ATmega256RFR2 carries the AT86RF233's register set, yet `hal/avr/` has its own radio driver: access, interrupts and core differ, and a shared file would be all `#if`.
- **One host link per image.** `com/usb_cdc.c` serves the ConBee II's native USB; `com/uart.c` serves every UART board through the driver contract `hal/uart_drv.h`, so it frames Spinel without naming a SoC. A board names its link in its `board.cmake`.
- **The ConBee III has two radio back-ends** behind the same `hal/radio.h`: `rail/` on Silicon Labs' RAIL library, which the `rcp` image uses, and `own/`, this tree's own sequencer program, which no image selects because it does not yet acknowledge. `-DDS_MG21_RADIO` selects one (§9); the `probe` image drives no radio and links neither.

The contracts:

| contract | what crosses it |
|---|---|
| `app/app_main.h` | the loop's hooks (`App_Boot`, `App_DataOut`, `App_Poll`), defined by one mode file |
| `hal/hal.h` | lifecycle, time, reset, panic, IRQ context, device identity |
| `hal/radio.h` | the PHY/MAC surface the core consumes |
| `hal/com.h` | the host link: init, attach, receive handler, write, poll |
| `hal/dbg.h` | the debug-record queue (§8) |
| `board/board.h` | the board descriptor: identity, link, radio, flash layout |
| `hal/uart_drv.h` | what a UART driver supplies to `com/uart.c` |
| `hal/mon_drv.h` | what a board supplies to the bench register monitor |
| `hal/hal_diag.h` | the SAMR21's bring-up stopwatch, clock witnesses and post-mortem row |

Every module's job is in the header comment of its file; the silicon facts behind each `hal/` directory are in its hardware doc.

## 3. RX path

Interrupts only fill rings; the main loop does all protocol work (Figure 3).

![The receive path in three columns. Left, in an interrupt-context band: the host link's bytes enter RcpApp_PushFromIrq, and the radio's frames enter the radio vector, which reads, filters and starts the Ack back to the radio inside the 192 µs turnaround. Middle, two amber rings: rcpRxRing, 512 bytes, drop-oldest, and the Radio_Frame ring, 8 slots, drop-newest. Right, in a main-loop band: Hdlc_RxByte decodes the host's bytes into Rcp_Feed, whose reply goes down to Hdlc_Encode; rcpAppRxFrame turns each radio frame into a STREAM_RAW notification for Hdlc_Encode too. Hdlc_Encode fills the TX ring, sized by the board (§5), drop-whole, which goes to the host in COM_TX_MAX packets.](images/rx-path.png)

*Figure 3 — the receive path. The amber rings are where interrupt and loop meet, each with its overflow rule, and every drop is counted. The Ack is the one thing that never reaches the loop. Source: `tools/diagrams/rx_path.py`.*

- **Host to RCP.** The link's vector pushes bytes into a ring (on a UART, a run of up to 32 octets per entry). The loop decodes them one byte at a time and feeds each frame to the session; the answer is encoded into the TX ring and leaves in `COM_TX_MAX`-byte packets.
- **Radio to host.** On a frame's end the radio vector reads it, filters it, starts the Ack when one is due and pushes it into an 8-slot ring. The loop turns each frame into a `STREAM_RAW` notification.
- **A frame that finds the ring full is not acknowledged**, so its sender retries.
- An FCS-invalid frame is dropped and counted, except in full promiscuous mode. While an energy scan runs, delivery is paused, so the scan's result cannot be pushed out of the reply ring.

Who builds the Ack depends on the back-end; the core's Ack decision and the source-match table are the same everywhere (`docs/ack-pending-frame.md`):

| back-end | Ack built by |
|---|---|
| `hal/samr21/`, `hal/avr/` | the radio vector, in firmware |
| `hal/mg21/rail/` | RAIL's auto-Ack; Frame Pending is set at its data-request event |
| `hal/mg21/own/` | none: this back-end sends no Ack |

## 4. TX path

A `SET STREAM_RAW` is answered once, when the radio reports the outcome; the request's TID ties the answer to the request (Figure 4).

![A sequence between the host, the session and the radio. The host sends SET STREAM_RAW with tid 3, carrying the PSDU and a trailer; the session checks that the stream is enabled, idle and well formed. If it is refused, a VALUE_IS LAST_STATUS with tid 3 goes back at once, carrying BUSY, INVALID_STATE or PARSE_ERROR. Otherwise the session calls Radio_Tx, which copies the PSDU and returns; Radio_Poll then takes one register step per loop pass through CCA, transmit and the Ack. The radio reports Rcp_TxDone to the session, and only then does VALUE_IS LAST_STATUS with tid 3 go back to the host, carrying OK, NO_ACK or CCA_FAILURE. A bracket marks that nothing is answered in between.](images/tx-path.png)

*Figure 4 — the transmit path. The TID, in amber, leaves with the request and comes back with the outcome. Source: `tools/diagrams/tx_path.py`.*

- `Radio_Tx` copies the PSDU and returns at once; `Radio_Poll` advances the transmission one register step per loop pass.
- A frame that asks for an Ack goes through the radio's retry engine (CSMA, Ack wait), the only way to tell `OK` from `NO_ACK`.
- Every back-end reports one of four outcomes, which become `OK`, `CCA_FAILURE`, `NO_ACK` or `FAILURE` (`docs/protocol.md`).

### 4.1 MAC security and timed transmit

`rcp/mac_sec.c` holds the three MAC keys and the frame counter, secures a frame before it reaches the radio, and composes an Enhanced Ack's security. It is core code under a host suite: the state is the caller's, and the block cipher is a seam (`lib/aes/aes.h`) each silicon fills. A board carries this with `DS_THREAD`; which boards do is `docs/protocol.md`. All of it stays out of the way until a host sets a key, so a host that sets none keeps the plain radio.

| | `hal/samr21/` | `hal/mg21/rail/` | `hal/avr/` |
|---|---|---|---|
| block cipher | the transceiver's AES engine, over SPI | `RADIOAES`, by DMA | the transceiver's AES engine |
| Enhanced Ack | composed in the radio vector | RAIL's `ENH_ACK`, at its data-request event | composed in the `RX_END` vector |
| radio clock | CLKM through TC4 and TC5, 1 MHz, from a second clock tree | RAIL's microsecond clock | Timer/Counter1, its wraps counted by a vector |

`Radio_ClockUs` is that clock, read by `RCP_TIMESTAMP` and stamped on every received frame. `Radio_TxAt` starts a frame at a moment on it, one attempt with at most one CCA; a moment already gone completes as `CCA_FAILURE`. `Radio_TxAck` hands back the Ack a transmission received, for the completion to carry.

## 5. Buffers and pools

There is no allocator: no `malloc`, no arena, no VLAs. Every buffer is a fixed, named pool.

| pool | size | when full |
|---|---|---|
| host RX byte ring | 512 B | drops the oldest byte, counted |
| HDLC RX frame | 332 B | the worst-case escaped frame; longer is dropped and the decoder resyncs |
| host TX byte ring | 512 B on USB, 4096 B on a UART (`PRODUCT_TX_RING`) | drops the whole reply, counted; never splits one |
| session value | 160 B | one request in flight: the largest value is a 155-byte `STREAM_RAW` notification |
| radio RX ring | 8 × 127 B | drops the newest frame, counted, unacknowledged |
| radio TX slot | 127 B | one frame in flight; a second is `BUSY` |
| source-match table | 32 short + 16 extended addresses | a list that does not fit is refused whole |
| debug-record rings | 2 × 64 × 8 B | drops the oldest record, counted (§8) |

## 6. State machines

Every multi-step activity is a small, explicit state machine: no loop spins, every wait is a deadline checked once per pass. Figure 5 shows the two that share the radio with another owner.

![Two state machines. Radio TX in samr21_radio.c: IDLE, nothing in flight, goes on Radio_Tx to TO_ARET, in amber, which waits for the radio and loops on itself while an Ack is on air or a frame waits; on TX_ARET_ON to WAIT_ARET, where the state settles; on TX_START to WAIT_END, the frame on air and then its Ack; and back to IDLE on TRX_END with the outcome, or from any phase on a missed deadline with TIMEOUT. The energy-scan dwell in rcp/scan.c: IDLE goes on Start to TUNE (retune, arm), SETTLE (the settle time), MEASURE (request a level) and READ (take it once), which loops back to MEASURE while samples are left; when none are left it goes to RESTORE (the host's receive state), EMIT (the result) and DONE, then back to IDLE. SUSPEND, in amber, is entered from any phase that issues actions when a transmission borrows the radio, and resumes into TUNE.](images/machines.png)

*Figure 5 — the two machines that share the radio. Each arrow is one step in one loop pass; amber is where a machine yields the transceiver. Source: `tools/diagrams/machines.py`.*

| machine | owner | states |
|---|---|---|
| HDLC decoder | `rcp/hdlc.c` | idle, in frame, escaped, overflow |
| session | `rcp/session.c` | idle, TX in flight |
| radio TX | the radio driver | Figure 5 |
| energy-scan dwell | `rcp/scan.c` | Figure 5 |
| radio vector phase | the radio driver | off, receive, Ack on air |
| host link TX pipe | the link driver | down, ready, armed |

## 7. Concurrency and ISR rules

The radio vector preempts everything but a UART link's vector, because the Ack's turnaround is a deadline; the RaspBee II's UART has a shorter one, its two octets of receive buffer at 115 200 (Figure 6). Names below are the SAMR21's; every board keeps the rules.

![Four rows by priority. Priority 0, which preempts all below: the RaspBee II's UART vector, which puts octets into the ring before its two-octet buffer overflows. Priority 1: the radio vector, in amber, which handles one frame: read, filter, maybe start the Ack, push one slot. Priority 2, where neither preempts the other: the ConBee II's USB vector, which puts bytes into the ring, and SysTick, the millisecond. The main loop, in thread context, does everything else: HDLC, the session, the TX steps and flash writes, then HAL_Wfi. A dashed arrow from the loop to the radio vector says Rf_Lock: the loop masks it. A line below lists what never runs in a handler: flash programming, encoding, a spin, a record from the radio vector.](images/contexts.png)

*Figure 6 — who may run when. Amber is the radio vector, with the Ack's deadline. Source: `tools/diagrams/contexts.py`.*

- **One owner of the SPI at a time.** The loop takes `Rf_Lock`, which masks the radio line; the vector needs no lock, because nothing that uses the SPI can preempt it. A transmission takes the radio only between frames.
- **One reader of the radio's IRQ status.** Reading it clears it, so only the vector reads it, and passes on what it does not handle.
- **Nothing stalls the bus.** A stalled store blocks every vector on a Cortex-M0+, so the watchdog feed skips while its register synchronises (`docs/hardware_samr21.md`).
- **Shared state is `volatile`**, and a value wider than the core's word that a handler writes is read under `HAL_IrqLock`.
- **Deadlines are compared as `(int32_t)(now - deadline)`**, so the 32-bit millisecond tick may wrap.
- **Only a completed loop pass feeds the watchdog.** While booting, every bounded wait feeds it; from the first pass on, only the main loop does, and never a handler, which keeps running while the loop hangs. A watchdog reset therefore means the loop stopped; the SAMR21 and AVR boards report the reset cause at the next boot.

## 8. Debug records

Every diagnostic is one 8-byte record (Figure 7); there are no debug strings, and a host decodes records against a table read out of the source.

![The debug record: four little-endian u16 fields in amber, magic 0x0000, component (which part spoke), line (a source line or a value) and reason (what happened). Below, the queue: a handler ring and a loop ring of 64 slots each, where a full ring drops the oldest; both feed the drain, which batches records to the image's way out: in the probe image the host link's IN stream, in the RCP image in-band as RCP_LOG_CRASH_DUMP, and the post-mortem row in flash, which keeps the last record across a flash.](images/debug-record.png)

*Figure 7 — the debug record and its path. Source: `tools/diagrams/debug_record.py`.*

- The stream has no header or checksum; it stays aligned by the fixed size. The `line` field often carries a value — a counter, a register read-back — rather than a line number.
- A host that attaches late has missed the boot records, so the boot set is replayed once the link is up, behind a marker.
- The post-mortem row makes a crashed app readable without the app running. Only the probe image has it: a rewrite stalls the CPU (`docs/hardware_samr21.md`).

## 9. Build-time options

| option | default | effect |
|---|---|---|
| `DEBOARD` | — | the board: `cb2`, `rb2`, `cb3` or `cbrb1` |
| `DS_MODE` | `probe` | the image: `probe` or `rcp` |
| `DS_MG21_RADIO` | `rail` on `cb3` | the radio back-end of a ConBee III `rcp` image; any other board refuses `rail` |
| `DS_LINK_BAUD` | set by the board | the UART host link's line rate |
| `DS_THREAD`, `DS_THREAD_TX_TIMING` | set by the board | MAC security and timed transmit (§4.1) in the `rcp` image |
| `DS_FLASHER_ACK` | set by the board | answer the flasher's reset frame, for a link that never disconnects |
| `DS_BOARD_DIAG` | set by the board | the SAMR21 boards' clock and radio witnesses, and the probe image's post-mortem row |
| `DS_MONITOR`, `DS_LOOP_DIAG` | set by the board, or off | extra diagnostics in the probe image |

`cmake/version.cmake` writes the version an `rcp` image reports on every build (`docs/build-and-flash.md`).
