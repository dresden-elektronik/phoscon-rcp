# Testing

What proves what in this tree: the host suites, the radio rigs, and the audits a new board target passes before it is first flashed. Read it before adding a module, a suite or a board.

| § | section |
|---|---|
| 1 | [Two layers of testing](#1-two-layers-of-testing) |
| 2 | [Host suites](#2-host-suites) |
| 3 | [Radio rigs](#3-radio-rigs) |
| 4 | [Conventions](#4-conventions) |
| 5 | [Auditing a new board target](#5-auditing-a-new-board-target) |
| 6 | [Hosts](#6-hosts) |

## 1. Two layers of testing

| layer | runs on | proves | cost |
|---|---|---|---|
| host suites (`test/`) | the build host, no target toolchain | logic, parsers, state machines, bounds, error paths, wire bytes | seconds |
| radio rigs and the board's records | real sticks | the silicon: the only thing that can falsify a hardware assumption | a flash |

The offline layer is cheap because of the core/glue split: a core module computes on buffers passed in and returns decisions, so a test drives it directly with nothing to stub (`docs/coding-rules.md`).

## 2. Host suites

```sh
cmake -B build/test -S . && cmake --build build/test
ctest --test-dir build/test --output-on-failure
```

Plain C99 with a small in-house harness (`test/tu/`); no dependency beyond a compiler and CMake. Captured frames are inlined as test vectors and pinned byte for byte. A suite's own header comment says what it proves.

| suite | covers |
|---|---|
| `hdlc`, `spinel` | the framing: escaping, the FCS, resync, the header byte, packed integers |
| `props`, `props_stream`, `props_debug` | the property table, the `STREAM_RAW` layouts, the crash-dump batch |
| `session` | captured host dialogs replayed byte for byte; TID rules; the deferred TX completion |
| `mac`, `srcmatch`, `scan` | header parsing and filtering, the source-match table, the energy-scan dwell |
| `rcp_app` | the app glue on the fakes of `test/fakes/` (link, radio, tick): no input on the host link may cost the board its watchdog. Noise, frames cut or run together, more than the receive ring holds, fuzzed requests to every property, a timed frame due long after its request, and a link that refuses writes; after each, every pass is healthy and a version request is still answered |
| `aes`, `mac_sec` | the AES library's known answers; securing a frame, against frames computed with an independent AES-CCM |
| `props_no_thread`, `props_no_timing` | the property table of a build without Thread, such as the ConBee III's `own` back-end, and of one with MAC security but no timed transmit, whose wire surfaces must not move |
| `dbg_queue`, `dbg_decoder`, `flasher_trap`, `mon_proto` | the debug-record queue; the witness tables of `tools/dbg.py`; the flasher's reset request in the raw byte stream; the bench register monitor's commands and address policy |
| `uart_baud`, `usart_baud`, `avr_baud`, `avr_clock_us`, `tick_plan` | each silicon's divisor and clock arithmetic |
| `samr21_basic`, `mg21_ident`, `avr_ident` | each board's identity parse |
| `mg21_regs`, `mg21_vectors`, `avr_regs`, `cb3_layout`, `mg21_reset_record` | register transcriptions, vector tables and flash layout against the vendor's own descriptions |
| `mg21_phy`, `mg21_rail_pa`, `mg21_radio_gen` | the ConBee III radio's arithmetic, power lookup, and generated data |
| `gcf_guard`, `image_guard` | every rule of the pre-flash guard, each with a container that passes and one that does not; the ConBee III's image size guard on both sides of its bound |
| `avr_int` | the portable code compiled with a 16-bit `int`, as the AVR boards run it, and no literal `1` of `int` width shifted by a variable count |

`mg21_radio_map` runs only where the maintainers' full radio record is present.

`tools/audit_replay.sh` is not a suite. Given the vendor's SDK trees, it compares the vendor-derived records it regenerates with their checked-in copies, runs the suites, and prints the built-image facts a review cites.

## 3. Radio rigs

The rigs prove on real radios what no offline test can. Every tool takes its nodes explicitly: a ConBee II is its CDC port, and a UART board is `<port>@<baud>`, or `--baud` where the tool has one. `tools/rcp_matrix.py` also reaches a RaspBee on a Pi as `ssh://<host>/dev/<tty>@115200`, where `<host>` may be `user@host`; the other tools run on the Pi. `tools/rcp_pair.py` and `tools/rcp_matrix.py` default to channel 26 and PAN `0xBEE5`; a run uses a quiet channel and a test PAN, never a live network's.

| tool | proves |
|---|---|
| `tools/rcp_pair.py` | two nodes: the receive filter per mode, the Ack policy, the Ack's Frame Pending bit, a frame larger than one USB packet |
| `tools/rcp_matrix.py` | every board against every other, both directions (Figure 1) |
| `tools/cb3_rcp_check.py` | a ConBee III (or, with `--baud`, any UART RCP) against a ConBee II |
| `tools/rcp_enh_ack.py` | two sticks, the second a reference RCP (or, swapped, the one under test): Enhanced Acks both ways, a frame encrypted here decrypting there, probing, and timed frames as the second radio stamps them, every byte checked with an independent AES-CCM |
| `tools/rcp_scan_gates.py`, `tools/rcp_energy_sweep.py` | the energy-scan contract, and with `--peer` that a MAC node still hears, filters and acknowledges after a dwell and after a RESET; a sweep with the peer silent and then transmitting |

Sticks keep their addresses until reset: reset the idle ones between runs, or two radios answer one frame. Pin a stock host's role in a pairing (`routereligible disable`), since a child that turns router changes its RLOC mid-test.

A stock OpenThread host writes its settings file synchronously, and on a Raspberry Pi SD card that has stalled the host for over 5 s: a child times out mid-attach and forms its own network, or the host reports no response from the RCP. Keep a host's working directory on a RAM disk in a rig, so a failure there is the firmware's.

Before asserting anything a rig handshakes each node (a freshly flashed stick answers nothing for up to about 12 s), reads back every setting a cell depends on, and warms the link until a frame arrives. It writes to a CDC port in 32-byte pieces: macOS truncates a large write, and the firmware drops a truncated frame silently.

What `rcp_pair.py` asserts:

| receiver's mode | broadcast | to the receiver | third address | foreign PAN | Ack-requesting frame |
|---|---|---|---|---|---|
| FULL | delivered | delivered | delivered | delivered | — |
| NETWORK | delivered | delivered | delivered | dropped | `NO_ACK`: a promiscuous node must not acknowledge |
| OFF | delivered | delivered | dropped | dropped | `OK` |

The Frame Pending cells write the receiver's source-match table and check the bit from both sides — the Ack the sender received and the flag the receiver reports: set for a poll while the table is off, or when the poller's address is in the table; clear for a data frame, an empty table or another address. A cell repeats an exchange that ends in `NO_ACK` up to three times, because the channel loses about one frame in twenty to FCS errors and the cell is about what the Ack says.

![A six-by-six matrix: senders down the side, receivers across the top, both listing two ConBee IIs, two ConBee IIIs, the RaspBee II and the ConBee I. The diagonal is empty, because a node does not test itself; every other square holds two cells, B for the burst and A for the Ack. The pair from the first ConBee II to the RaspBee II is drawn in amber, and a panel beside the matrix explains it: in the burst the receiver is promiscuous and passes when 90 % of the burst arrives byte-identical; in the Ack the receiver is a MAC node and passes on LAST_STATUS 0 within three attempts; every other node's radio is off, because a bystander on the same PAN and address would acknowledge in the receiver's place.](images/matrix.png)

*Figure 1 — what `tools/rcp_matrix.py` runs. Six nodes make 30 ordered pairs and 60 cells. The figure shows the method, not a result. Source: `tools/diagrams/matrix.py`.*

**A run is a gate only on a quiet channel:** on a busy one, the band's traffic corrupts bursts and cells fail with no fault in the firmware.

For the energy scan the change is the gate, not the level: a neighbour network swings 10–14 dB between two sweeps, so the sweep asserts that the peer's channel rose while quiet channels stayed at the floor.

## 4. Conventions

- **A module without a suite is not finished.** It compiles host-clean under `-Wall -Wextra -std=c99`, its suite runs in `ctest`, and its bounds and error paths are exercised. A red suite blocks the next commit that touches its module.
- **Pin the wire layout** byte for byte: it catches what re-reading the specification misses.
- **Diagnostics are records, not strings** (`docs/architecture.md`). `tools/dbg.py` decodes them from the header's own tables; `tools/spinel_console.py` drives an RCP; `tools/rcp_debug_stream.py` reads the RCP image's in-band records.
- **A hash only proves difference.** Each record carries its source line, so adding a line anywhere changes the image. To show a change is cosmetic, compare masked disassembly.
- **The instrument is part of the system.** A rig, a decoder or a record channel can be wrong; a change justified by a broken instrument is undone, not documented.
- **The board has the last word**, and flashing it always goes through the pre-flash guard (`docs/build-and-flash.md`).

## 5. Auditing a new board target

A new board target is not verified by running it: everything that can be settled without a board is settled first, in the order of Figure 2. The audits were written for the ConBee III and apply to any new target.

![Three stages left to right, joined by arrows with an amber gate on each. Static, with no board and no flash: A register transcription, B guard mutation test, C layout arithmetic, D container against the vendor's, E pre-flash guard refusals, F link audit of the ELF, G vector table identity, H bring-up arithmetic, I core unchanged, J what the image may write, K radio provenance. Bench, read-only, on an untouched board: L what the board is, M what it holds (MAC, PAN, key), N the bootloader's report, O the cold-boot window. The first flash, guarded and with recovery written down: P what landed (the app CRC), Q M again (identity survived), R the DFU route taken, S an interrupted update, T the two-node rig.](images/audit-gates.png)

*Figure 2 — the audits before and at a new board's first flash. Amber is a gate: a stage starts only when the one before is recorded as passing. Source: `tools/diagrams/audit_gates.py`.*

| audit | how |
|---|---|
| A | regenerate the register records from the vendor headers and CMSIS (`tools/mg21_regs_gold.py`, `tools/mg21_radio_gold.py`), then `ctest -R mg21`: nothing changes and the suites pass |
| B | corrupt one transcribed or reference value at a time: every corruption turns a suite red and names the symbol |
| C | `ctest -R cb3_layout`, compared with the linker script and the image guard: the same numbers |
| D | a container this tree writes against the vendor's for the same binary: identical, or every difference explained |
| E | `test/unit/test_gcf_guard.py`: every mutated container refused, naming its rule |
| F | `readelf`, `nm`, the map and the startup's disassembly: vectors at the origin, every section inside its region |
| G | `ctest -R mg21_vectors`: each handler at the slot the part's own interrupt numbers give it |
| H | the divisor and tick suites, then the board's clock witnesses: the line rate, the millisecond and the watchdog period the code really produces |
| I | all suites, and the other boards' images byte-identical: a new silicon layer did not touch shared code |
| J | every flash-writing call site listed and justified: the image never writes the identity, NVM3 or outside its area |
| K | where every radio claim comes from (`docs/hardware_mg21.md`); a linked library recorded in `src/lib/PROVENANCE.md` |
| L–O | `tools/cb3_probe.py`, which by construction sends only read commands: the board's identity and network state before any flash, and how its bootloader is entered |
| P–T | after the guarded first flash: the bootloader's app CRC equals the built image's, the identity read in M survived, the update route works, an interrupted update leaves the old app running, and the two-node rig passes |

An audit passes when its criterion is stated and met, not when its tool ran. An audit that cannot be run is recorded in the board's hardware doc.

## 6. Hosts

What has run on this firmware, unmodified. The hosts are other projects; this records only that they drive the radio.

| host | boards | what ran | not yet |
|---|---|---|---|
| OpenThread, as a Thread 1.4 host and in the Home Assistant border router app | ConBee II, ConBee III, ConBee I; the host alone on the RaspBee II | starts with no skip flag, leads a network, secured Enhanced Acks; a Matter plug and a sleepy Matter sensor on the ConBee III for hours, and on the ConBee II and ConBee I with the sensor behind the plug | a CSL child; a sleepy child directly on the ConBee II or ConBee I; the border router app on the RaspBee II |
| Ziggurat, through Home Assistant's ZHA | ConBee II, ConBee I | forms a network, a bulb joins on the ConBee II and switches on both | a sleepy device, a long run |
| zigbee-on-host, through Zigbee2MQTT | ConBee II | forms a network, a bulb joins and switches | a sleepy device, a long run |
| Phoscon Zigbee Stack | ConBee II, ConBee III, ConBee I, RaspBee II, RaspBee I | the reference host, as coordinator and router | a long run on the RaspBee II or RaspBee I |
