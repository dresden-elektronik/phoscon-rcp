# The Ack's Frame Pending bit

How this firmware sets the Frame Pending bit in the Ack to a polling device's Data Request — the bit that tells a sleepy child whether its parent holds a frame for it — and why it is done this way. Read it before touching the Ack path of a radio driver. Sources: IEEE 802.15.4-2024, the AT86RF233 datasheet, OpenThread's SAMR21 and EFR32 ports, Silicon Labs' RAIL headers, and the vendor deCONZ firmware.

## The firmware's behaviour

The host fills the source-match table (`docs/protocol.md`); the firmware takes the bit of every Ack it sends for a Data Request from it.

| radio back-end | who builds the Ack | Frame Pending in it | detail |
|---|---|---|---|
| AT86RF233 (ConBee II, RaspBee II) | this firmware, in the radio vector: the radio receives in basic `RX_ON` and never auto-acks | Data Request: the table's answer, or 1 while `MAC_SRC_MATCH_ENABLED` is 0. Secured MAC command: the same, or 1 with no source address to look up. Anything else: 0 | §5 |
| ATmega256RFR2 (ConBee I, RaspBee I) | the same scheme on the on-die transceiver | the same | §5 |
| EFR32MG21 on RAIL (ConBee III, `rcp` default) | RAIL's auto-Ack | set at RAIL's data-request event: 1 while `MAC_SRC_MATCH_ENABLED` is 0, otherwise the table's answer; RAIL's default is 0 | §7 |
| EFR32MG21 on the own back-end | no Ack is sent | — | `docs/hardware_mg21.md` |

| § | section |
|---|---|
| 1 | [The standard's requirements](#1-the-standards-requirements) |
| 2 | [AT86RF233 support](#2-at86rf233-support) |
| 3 | [Timing budget](#3-timing-budget) |
| 4 | [Two ways to set the bit](#4-two-ways-to-set-the-bit) |
| 5 | [The firmware's behaviour on the AT86RF233 and ATmega256RFR2](#5-the-firmwares-behaviour-on-the-at86rf233-and-atmega256rfr2) |
| 6 | [Open measurements](#6-open-measurements) |
| 7 | [RAIL's Ack, on the ConBee III](#7-rails-ack-on-the-conbee-iii) |

## 1. The standard's requirements

IEEE 802.15.4-2024 §10.22.3: a coordinator sets the bit "accordingly to indicate whether a frame is actually pending for the device. If this is not possible, the coordinator shall set the Frame Pending field of the Ack frame to one."

- The decision is per address, not global.
- A coordinator that cannot know answers "pending".
- The two errors are not equal: a bit set with nothing queued costs one extra poll; a bit clear with something queued lets a sleepy child stop polling.

## 2. AT86RF233 support

One global bit, `AACK_SET_PD`, copied into every Ack the radio composes for a Data Request, and a frame filter for one address pair. **There is no per-address table in hardware**, so an Ack that answers per address is software's: either software flips the global bit in time, or it composes the Ack itself.

## 3. Timing budget

Air timing at 250 kbit/s: a symbol is 16 µs, an octet 32 µs, the preamble and SFD 160 µs. A short-addressed Data Request carries a nine-octet MAC header.

![A microsecond scale from 0 to 840. On the air: SHR to 160 µs, PHR to 192, the nine-octet MAC header (FCF, DSN, PAN, destination, source) to 480, the command octet to 512, the FCS to 576, then the 192 µs turnaround, and the Ack from 768. Event rules mark RX_START at 192, AMI at 480, TRX_END at 576 and the Ack due at 768. Two amber bars show the time left to decide the Frame Pending bit: 288 µs from AMI, 192 µs from TRX_END. A last lane shows this firmware: the radio vector runs from its stamp, 9 µs after TRX_END, to TX_START 167 µs later, and the Ack's preamble follows 16 µs after that, at 768; a vector that cannot start by stamp + 441 µs abandons the Ack.](images/ack-timing.png)

*Figure 1 — the Ack's timing budget for a Data Request, to scale. Amber is the time left to decide the Frame Pending bit from each event; the last lane is this firmware's sequence (§5). Source: `tools/diagrams/ack_timing.py`.*

| event | time into the frame |
|---|---|
| `RX_START` | 192 µs, after the PHY header |
| `AMI`, the address match | 480 µs, after the nine-octet header |
| `TRX_END` | 576 µs, after the command octet and the FCS |
| the Ack's start | 768 µs: `TRX_END` + 192 µs (twelve symbols) |

Deciding at `TRX_END` leaves 192 µs; deciding at `AMI`, with the source address already in the buffer, leaves 288 µs.

## 4. Two ways to set the bit

| | the radio composes the Ack, software writes one bit | software composes the Ack |
|---|---|---|
| who ships it | the vendor deCONZ firmware (in its `TRX_END` handler); Silicon Labs' RAIL | OpenThread's SAMR21 port; **this firmware** |
| decision window | 192 µs from `TRX_END`, 288 µs from `AMI` | the firmware sets the start time; the first octet must be in the buffer 176 µs after it |
| failure mode | too slow, and the bit stays at its default ("pending") | a late Ack, which the sender retries |
| what else it allows | nothing | enhanced Acks, Link Metrics |

The vendor's first approach keeps the safe default and clears the bit only when its table says nothing is queued. OpenThread's SAMR21 port reads the frame in stages during reception and DMA-loads its own Ack 180 µs after the frame. On the EFR32, both OpenThread and the stock ConBee III firmware keep RAIL's Ack and set the bit at RAIL's data-request event (§7).

## 5. The firmware's behaviour on the AT86RF233 and ATmega256RFR2

The second way, in its simplest form: the whole frame is read once it has ended, then answered from the radio vector. Staged reads are not needed for the frames a coordinator answers. The ATmega256RFR2 runs the same scheme on its own vectors.

| step | what happens |
|---|---|
| stamp | SysTick's cycle counter at entry: no earlier than frame end + `tIRQ` (9 µs), so every later delay is counted from a point that can only make the Ack late, never early |
| read | the whole frame in one Frame Buffer access, octets queued back to back: 2 µs per octet at the 4 MHz SCLK |
| filter | `Mac_ParseHeader` and `Mac_Filter`: the transceiver's third level filter rules, applied in software |
| decide | `Mac_AckDecide` against the radio's copy of the source-match table, held in on-air byte order |
| room | no Ack when the receive ring is full: the host would never see the frame, so the sender must retry |
| compose | `FORCE_PLL_ON`, then four octets into the Frame Buffer: PHR 5, FCF, DSN. The radio computes the FCS |
| send | busy-wait until stamp + 167 µs, then `TX_START`; the preamble starts 16 µs later, 192 µs after the frame at the earliest. An Ack that could not start by stamp + 441 µs is abandoned and counted |
| end | the Ack's own `TRX_END` returns the radio to `RX_ON`. A loop-side recovery puts it back if that `TRX_END` never arrives within 3 ms |

| mode | Ack | Frame Pending |
|---|---|---|
| FULL, NETWORK | never | — |
| OFF | for a frame that passes the software filter, is addressed to this node and asks for one | Data Request: the table's answer, or 1 while `MAC_SRC_MATCH_ENABLED` is 0. Secured MAC command: treated as a Data Request, because its command identifier is hidden and a Thread poll is secured; with no source address to look up, 1. Anything else: 0 |

Measured on two ConBee IIs: every Data Request and data frame is acknowledged, the slowest of 1 500 Acks starts 200 µs after its stamp, and no Frame Pending answer was wrong. The two-stick rig's Frame Pending cells check the table off, empty, and holding the peer's or another short or extended address (`docs/testing.md`). The bit this board *sent* is reported per frame as `MD_FLAG` bit 4; the bit it *received* in a transmission's Ack comes back with the transmit completion (`docs/protocol.md`).

## 6. Open measurements

| open | how to settle it |
|---|---|
| how late an Ack may be for the senders a coordinator serves | the firmware abandons an Ack it cannot start by stamp + 441 µs; sweep the start delay against EFR32 and CC26xx senders |
| whether a host's own traffic delays the vector | a soak with the host transmitting while polls arrive |

## 7. RAIL's Ack, on the ConBee III

RAIL composes the Immediate Ack itself; its Frame Pending bit starts at a configured default and can be changed once, by `RAIL_IEEE802154_SetFramePending()`, while the data-request event is handled — the call returns `RAIL_STATUS_INVALID_STATE` when the Ack can no longer be reached. RAIL has no source-match table on this part: the application's lookup decides.

- **This firmware's `rail` back-end** keeps RAIL's Ack with the default "not pending", reads the requester's address at the data-request event, and sets the bit when the table holds it (or the table is disabled). A refused call is counted as `ackLate`.
- **The event point is the early one.** This firmware opts into `RAIL_IEEE802154_EnableEarlyFramePending()`, so the event comes right after the source address, for every Ack-requesting MAC command. The late point waits for the command byte, which a secured poll hides. The stock ConBee III firmware stays on the late point.
- **A version 2 frame gets an Enhanced Ack** once a host has set a key, and the bit is in the Ack this firmware builds: the table's answer, or with the table off 1 for a command and 0 for a data frame. The ConBee II follows the same rule; its vector builds that Ack (`docs/hardware_samr21.md`).
- **The timing is the standard's**, not the EFR32's: RAIL's 672 µs Ack timeout is 802.15.4's 54-symbol wait minus the Ack's own 12 symbols, and its 192 µs turnaround is the same twelve symbols as §3.

Where the call writes the bit inside the radio is not visible and not measured.
