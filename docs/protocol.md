# Protocol

The host-facing contract of the RCP image, byte-exact: the host link, the framings, the properties, the `STREAM_RAW` data path and the status codes. It is written for the author of a host stack, who can then run a Zigbee or Thread network through the RCP or use it as a sniffer.

The contract is the raw-radio subset of OpenThread's Spinel RCP interface — protocol 4.3, RCP API 4 — so a host written against OpenThread's `RadioSpinel` drives it with the same calls. Where this firmware departs from upstream, the section says so.

What has run on it is `docs/testing.md`. Phoscon Zigbee Stack, the reference host, is to be released separately and recognises the `DSPINEL` prefix of `NCP_VERSION` (§10), so the prefix stays fixed.

| § | section |
|---|---|
|  | [Features per board](#features-per-board) |
| 1 | [Terms](#1-terms) |
| 2 | [Transport](#2-transport) |
| 3 | [HDLC framing](#3-hdlc-framing) |
| 4 | [Spinel framing](#4-spinel-framing) |
| 5 | [Property model](#5-property-model) |
| 6 | [Host startup sequence](#6-host-startup-sequence) |
| 7 | [STREAM_RAW](#7-stream_raw) |
| 8 | [Reference captures](#8-reference-captures) |
| 9 | [Status codes](#9-status-codes) |
| 10 | [Property table, with 10.1 the energy scan and 10.2 source match](#10-property-table) |

## Features per board

| feature | `cb2` | `rb2` | `cb3` | `cbrb1` |
|---|---|---|---|---|
| receive, transmit with CSMA and retries, immediate Ack | yes | yes | yes | yes |
| promiscuous receive (§6), energy scan (§10.1), source match (§10.2) | yes | yes | yes | yes |
| Enhanced Ack, Link Metrics probing | yes | yes | yes | yes |
| MAC security, `TRANSMIT_SEC` (§7.1) | yes | yes | yes | yes |
| timed transmit, `TRANSMIT_TIMING` (§7) | yes | yes | yes | yes |
| a Thread 1.4 host with no skip flag | yes | yes | yes | yes |
| a CSL child | not tested | not tested | not tested | not tested, unlikely behind this link |
| transmit power cap in dBm at the transceiver, channels 11 to 25 / 26 (§10) | −3 / −12 | −6 / −17 | 10 / 10 | −11 / −11 |

`cb3` is the image on RAIL throughout this document, and the line rate of each board is §2. The ConBee III on RAIL does not see an Ack while promiscuous, so a frame that asks for one completes as `NO_ACK`; a host with `MAC_PROMISCUOUS_MODE` off is not affected.

What the RCP does not do:

| not implemented | consequence for a host |
|---|---|
| persistent state | the RCP keeps nothing across a reset; the host restores every property after `RESET` |
| `PEEK`, `POKE`, `PHY_FREQ`, `NET_*` | `INVALID_COMMAND` or `PROP_NOT_FOUND` |
| beacon and discover scans | only the energy scan exists (§10.1) |
| per-frame power, sleep-to-transmit | the matching `RADIO_CAPS` bits are clear; the matching TX request fields are ignored |
| a noise-floor measurement | `MD_NOISE` is always −128 |
| more than one interface | `IID` is 0 |

## 1. Terms

| term | meaning |
|---|---|
| PSDU | an 802.15.4 frame, 0..127 bytes; its last two bytes are the FCS |
| notification | an unsolicited RCP→host `VALUE_IS`, always with TID 0 |
| TID | the transaction id in the Spinel header, 0..15 |
| ARET | the radio's automatic retransmission and acknowledgment engine |

## 2. Transport

A bidirectional byte pipe carrying HDLC-framed Spinel in both directions. The link is the board's:

| board | link | line rate |
|---|---|---|
| ConBee II | native USB CDC: VID `0x1CF1`, PID `0x0030`, bulk OUT EP2 to the RCP, bulk IN EP1 from it | none: line coding is ignored |
| RaspBee II | UART on the Raspberry Pi header | 115 200 baud |
| ConBee III | UART behind the stick's USB-serial bridge | 115 200 baud |
| ConBee I, RaspBee I | UART behind an FTDI bridge, or on the Pi header | 57 600 baud |

- Inbound bytes go into a 512-byte ring; a host that overruns it loses the oldest bytes.
- A reply leaves in 64-byte packets as the link accepts them, so a frame larger than one packet still arrives whole. A reply that does not fit the outbound ring is discarded whole, never truncated.
- **No hardware flow control.** A host leaves it off: with it on, a stock OpenThread host's first exchange with a ConBee III times out.
- **An FTDI bridge holds received bytes for its latency timer**, 16 ms by default on Linux, so a peer's quick reply can reach a host in one read with the Ack before it: an OpenThread host then handles the reply first and counts a good Enhanced Ack as failed. As root: `echo 1 > /sys/bus/usb-serial/devices/<ttyUSBn>/latency_timer`, until the stick is replugged. On macOS the timer is also 16 ms, and the `IOSSDATALAT` ioctl sets it: the FTDI driver takes the value as milliseconds in 8 bits and keeps it after the port is closed. A host that sends one frame at a time and waits for each completion gets its completions in whole periods of the timer, so behind a 16 ms timer it sends at most 62 frames a second.
- **An OpenThread host** takes `spinel+hdlc+uart://<port>?uart-baudrate=<line rate>&uart-init-deassert`. A board the table above does not mark as a Thread host needs `skip-rcp-compatibility-check`.
- The same byte stream carries GCFFlasher's 16-byte reset frame. It contains no `0x7E`, so the HDLC decoder ignores it; the RCP recognises it separately and resets into the bootloader. It is recognised only outside a Spinel frame: from a flag followed by a header octet up to the next flag, the same octets are a payload, such as a datagram in a transmit request. A link idle for 50 ms also ends a frame, so the flasher still gets through after a host that died mid-frame.

## 3. HDLC framing

A frame is `FEND [escaped payload] [escaped FCS] FEND`. The FCS is computed over the unescaped payload only.

| item | value |
|---|---|
| flag (`FEND`) | `0x7E` |
| escape (`FESC`) | `0x7D`; the escaped byte is the original XOR `0x20` |
| FCS length | 2 bytes |
| maximum frame | a value is at most 160 bytes (`RCP_VALUE_MAX`), about 165 with its header and FCS; a transmit completion that carries an Ack can be as large. The decoder overflows a frame that unescapes to 332 bytes (`HDLC_FRAME_MAX`) |
| FCS algorithm | reflected CRC-16/X.25: polynomial `0x8408`, shift right, init `0xFFFF` |
| FCS on the wire | `crc ^ 0xFFFF`, low byte first |
| residue over payload+FCS | `0xF0B8` |

- **Escaping.** The encoder escapes the superset `{0x11, 0x13, 0x7E, 0x7D}`: `0x11`/`0x13` (XON/XOFF) are escaped as well, so a UART's software flow control cannot eat a frame. The decoder unescapes any escaped byte, not only that superset.
- **Worked example.** Payload `89 06 25 13` → `crc = 0x7E64` → wire FCS `9B 81`; the escaped frame is `7E 89 06 25 7D 33 9B 81 7E` (the `0x13` arrives as `7D 33`).
- **Decoder.** A `FEND` closes a frame; repeated `FEND`s are idle. A frame shorter than its FCS, or whose residue is not `0xF0B8`, is dropped; anything before the first `FEND` is discarded, so noise cannot desynchronise it. A frame that unescapes to 332 bytes is dropped as an overflow.
- **Encoder.** Writes nothing if the escaped frame does not fit.

## 4. Spinel framing

![One frame at three magnifications. The HDLC frame on the wire: FEND 0x7E, the escaped Spinel frame (0x7E, 0x7D, 0x11 and 0x13 go out as 0x7D and the byte XOR 0x20), the FCS (CRC-16/X.25, low byte first), FEND 0x7E. The Spinel frame inside it: a 1-byte header, a 1-byte command, a property id as a packed uint of 1 to 5 bytes, and the value. The header byte: bits 7 and 6 are FLG, always 1 0, so a header is 0x80 to 0xBF; bits 5 and 4 are the IID, 0 on the wire; bits 3 to 0, in amber, are the TID, 0 for an unsolicited frame, and an answer echoes the request's. Last, the worked example of §3: the payload 89 06 25 13 goes out as 7E, 89 (the header, TID 9, in amber), 06 (VALUE_IS), 25 (the property), 7D 33 (0x13 escaped), 9B 81 (the FCS), 7E.](images/frame.png)

*Figure 1 — one frame on the host link: HDLC (§3) carries a Spinel frame, whose first byte carries the transaction id. The example row is the worked example of §3. Source: `tools/diagrams/frame.py`.*

| field | bits | rule |
|---|---|---|
| `FLG` | 7:6 | must be `0b10`, so a header byte is `0x80`..`0xBF` |
| `NLI`/`IID` | 5:4 | `0` on the wire; the decoder tolerates any value |
| `TID` | 3:0 | transaction id; `0` means unsolicited, a response echoes the command's TID |

A frame whose `FLG` is not `0b10` is not Spinel and is not decoded.

A frame is `header, command, [packed property id], value`. Only the commands that operate on a property carry a property id; for every other command the value starts immediately after the command byte.

| id | command | direction | behaviour |
|---|---|---|---|
| 0 | `NOOP` | host→RCP | no reply |
| 1 | `RESET` | host→RCP | reinitialise to power-on state; answer `LAST_STATUS = 114`, TID 0 (§6) |
| 2 | `PROP_VALUE_GET` | host→RCP | value or error (§5) |
| 3 | `PROP_VALUE_SET` | host→RCP | echo or error (§5) |
| 4 | `PROP_VALUE_INSERT` | host→RCP | one entry into a list property; `VALUE_INSERTED` or error (§10.2) |
| 5 | `PROP_VALUE_REMOVE` | host→RCP | one entry out of a list property; `VALUE_REMOVED` or error (§10.2) |
| 6 | `PROP_VALUE_IS` | RCP→host | a reply, a confirmation or a notification |
| 7 | `PROP_VALUE_INSERTED` | RCP→host | confirms an `INSERT`, carrying the request's value |
| 8 | `PROP_VALUE_REMOVED` | RCP→host | confirms a `REMOVE`, carrying the request's value |
| others | `NET_*`, `PEEK`, `POKE`, … | | `LAST_STATUS = INVALID_COMMAND` |

**Packed uint** (the property id and several values): 7 bits per byte, least-significant group first, the MSB of each byte means "more bytes follow". A 32-bit value needs at most 5 bytes; a value that does not terminate within 5 bytes is rejected.

| value | packed bytes |
|---|---|
| `0x0B` | `0B` |
| `0xB0` | `B0 01` |
| `0x0403` | `83 08` |

**Value types** used by this surface:

| char | type | encoding |
|---|---|---|
| `b` | `BOOL` | 1 byte, `0x00`/`0x01` |
| `C` | `UINT8` | 1 byte |
| `c` | `INT8` | 1 byte, two's complement |
| `S` | `UINT16` | 2 bytes, little-endian |
| `L` | `UINT32` | 4 bytes, little-endian |
| `X` | `UINT64` | 8 bytes, little-endian |
| `E` | `EUI64` | 8 bytes, big-endian |
| `U` | `UTF8` | zero-terminated string (the terminator is part of the value) |
| `i` | packed uint | the varint above |
| `ii` | two packed uints | `PROTOCOL_VERSION` (major, minor) |
| `A(i)` | list of packed uints | varints to the end of the value: `CAPS` |
| `A(C)` | list of `UINT8` | bytes to the end of the value: `PHY_CHAN_SUPPORTED`, `PHY_CHAN_PREFERRED`. `MAC_SCAN_MASK` reads back with a count byte first |

A list value carries its own length, which makes it the exception to the rule that a value's size is inferred from its type. `MAC_SCAN_MASK` is the only writable list in the table, and a SET takes either shape: upstream's bare channel numbers, or a count byte matching the bytes that follow. A first byte that is neither that count nor a channel from 11 to 26 is `INVALID_ARG`.

## 5. Property model

- **GET** `id` → `VALUE_IS id value`, or `VALUE_IS LAST_STATUS code` when the read fails. The answer carries the command's TID.
- **SET** `id value` → on success `VALUE_IS id value`, where the value is re-read through the GET handler (the property echo); on failure `VALUE_IS LAST_STATUS code`. Because every implemented property is readable, every accepted SET is confirmed with the property echo — there is no property here that answers a bare `LAST_STATUS = OK`.
- A SET of a read-only property answers `UNIMPLEMENTED`. An unknown property id answers `PROP_NOT_FOUND` for GET and SET alike. A wrong length or an out-of-range value answers `INVALID_ARG`, and the value is not stored.
- A GET whose value cannot be produced in the reply buffer answers `NOMEM`.
- A frame that does not decode — a non-Spinel header, a missing property id, an unterminated varint — is counted and left unanswered: there is no trustworthy TID to answer to.
- **Notifications** use TID 0. `SET STREAM_RAW` is the one write that is not a table update, and its answer is deferred to the radio outcome (§7).
- `LAST_STATUS` (id 0) is read-only. A GET returns OK; the error and reset codes are delivered *as* a `VALUE_IS` of property 0, never as the value of another property.

## 6. Host startup sequence

A host brings the link up in the order of Figure 2: a bare `0x7E` to flush the transport, the five identity queries, then `RESET` (payload `2`, "stack"), which the RCP answers with an unsolicited `VALUE_IS LAST_STATUS = 114` (`RESET_SOFTWARE`) with TID 0.

![A sequence between the host and the RCP. The host sends a bare 0x7E, then five GETs, each answered by a VALUE_IS: PROTOCOL_VERSION 4.3, NCP_VERSION DSPINEL/1.0, the radio and the version, INTERFACE_TYPE 3 (Spinel), RCP_API_VERSION 4 and RCP_MIN_HOST_API_VERSION 4; a bracket marks these as answered from power-on, before the reset. Then RESET 2, whose type is accepted and ignored, and the unsolicited VALUE_IS LAST_STATUS = 114 with TID 0, in amber. Two alternatives follow. To form a network: SET PHY_ENABLED 1, PHY_CHAN and PHY_TX_POWER; SET MAC_15_4_LADDR, SADDR and PANID; SET MAC_RX_ON_WHEN_IDLE and MAC_RAW_STREAM_ENABLED, each answered by VALUE_IS. Or, to start a sniffer: SET PHY_ENABLED 1 and PHY_CHAN; SET MAC_PROMISCUOUS_MODE 2 (FULL); SET MAC_RX_ON_WHEN_IDLE and MAC_RAW_STREAM_ENABLED; then every frame heard arrives as an unsolicited VALUE_IS STREAM_RAW with TID 0.](images/startup.png)

*Figure 2 — host startup: identify, reset, then become a coordinator or a sniffer. Amber is the one answer the reset produces. Source: `tools/diagrams/startup.py`.*

The RCP is live and serving Spinel from power-on: it answers the identity queries **before** the reset. It emits no unsolicited power-on `LAST_STATUS`; the only reset code it sends is `114`, in response to `RESET`. The `RESET` payload (the reset type) is accepted and ignored.

A frame still on its way when `RESET` arrives is finished by the radio but never answered: its completion carries a TID from before the reset, which a host that has started over may be using again. Until the radio is done, a transmit request is answered `BUSY`.

To **form a network** (coordinator) the host then sets, in order: `PHY_ENABLED = 1`, `PHY_CHAN`, `PHY_TX_POWER`, `MAC_15_4_LADDR`, `MAC_15_4_SADDR`, `MAC_15_4_PANID`, `MAC_RX_ON_WHEN_IDLE = 1`, `MAC_RAW_STREAM_ENABLED = 1`, and reads `PHY_TX_POWER`, `PHY_RSSI`, `PHY_RX_SENSITIVITY` and `PHY_CCA_THRESHOLD` for telemetry. From then on every frame the coordinator sends or receives goes through `STREAM_RAW`.

To **start a sniffer** the host sets `PHY_ENABLED = 1`, `PHY_CHAN`, `MAC_PROMISCUOUS_MODE = 2` (FULL), `MAC_RX_ON_WHEN_IDLE = 1` and `MAC_RAW_STREAM_ENABLED = 1`, then reads unsolicited `STREAM_RAW` notifications. To stop, it clears `MAC_PROMISCUOUS_MODE`, then `PHY_ENABLED`, then `MAC_RAW_STREAM_ENABLED`.

## 7. STREAM_RAW

`STREAM_RAW` is property `0x71`. It is not in the property table: the session handles a SET specially, and a GET of `0x71` answers `PROP_NOT_FOUND`. TX completion is reported through `LAST_STATUS`, not through `STREAM_RAW`.

![Two values, each drawn as a coarse strip and then its fixed block magnified to one byte per cell. The TX request: a 2-byte length at offset 0, the PSDU of n bytes with its FCS at offset 2, and the 16-byte trailer at n+2. In the trailer, amber is what this firmware uses: txChannel at n+2, maxRetries at n+4, enableCSMACA at n+5, isHeaderUpdated, isSecurityProcessed, the 4-byte txDelay and txDelayBase, and rxChannelAfterTx. Dashed, read and dropped: maxCsmaBackoffs and isRetransmission. The RX notification: the same length and PSDU, then 26 bytes of metadata at n+2. In amber: MD_POWER (the RSSI), MD_FLAG, the channel, the LQI, the 8-byte timestamp, the RX error code, the Ack's key index and its 4-byte frame counter. Dashed, a constant: MD_NOISE (-128). Outlined, as lengths: the MD_PHY, MD_VEND and MD_MAC length prefixes.](images/stream-raw.png)

*Figure 3 — the two `STREAM_RAW` values. Amber marks the bytes this firmware uses; the timing and security fields apply on a board with those features. The tables below give each field's type and rule. Source: `tools/diagrams/stream_raw.py`.*

### 7.1 Host → RCP: TX request (`SET STREAM_RAW`)

The value is a 2-byte little-endian length, the PSDU, and a trailer of up to 16 bytes. As in upstream's decoder, only `txChannel` is required: a host may end the trailer after any field, and the fields it leaves out take the default below.

| offset | len | type | field | default |
|---|---|---|---|---|
| 0 | 2 | `S` | `psduLength` (little-endian) | required |
| 2 | n | — | PSDU, the complete 802.15.4 frame | required |
| n+2 | 1 | `C` | `txChannel`, 11..26; `0` keeps the current channel | required |
| n+3 | 1 | `C` | `maxCsmaBackoffs` (parsed but unused) | `0` |
| n+4 | 1 | `C` | `maxRetries` (clamped to the radio's four bits) | `3` |
| n+5 | 1 | `b` | `enableCSMACA` (`1` = CCA/backoff, `0` = transmit immediately) | `1` |
| n+6 | 1 | `b` | `isHeaderUpdated`: the host already wrote the key index and frame counter | `0` |
| n+7 | 1 | `b` | `isRetransmission` (ignored) | `0` |
| n+8 | 1 | `b` | `isSecurityProcessed`: the host already secured the frame | `0` |
| n+9 | 4 | `L` | `txDelay` in µs (`0` = as soon as possible); honoured with `TRANSMIT_TIMING` | `0` |
| n+13 | 4 | `L` | `txDelayBase`: the low half of `RCP_TIMESTAMP` that `txDelay` counts from | `0` |
| n+17 | 1 | `C` | `rxChannelAfterTx` (`0` = keep); honoured with `TRANSMIT_TIMING` | `0` |

A `txChannel` other than 0 tunes the radio to it for the frame, and the receiver then stays on `rxChannelAfterTx`, or on `txChannel` where that is 0 or not honoured, until the host next writes `PHY_CHAN`. Every such write retunes the radio, even one that repeats the property's value: that is how an OpenThread host brings the radio back after a scan.

The PSDU carries its own FCS, as it does on the receive side; the radio regenerates those two bytes rather than appending them. With the whole trailer, the value is `n + 18`, and bytes beyond it are ignored. A full 127-byte PSDU is therefore a 145-byte value; the session's value path (`RCP_VALUE_MAX`, 160 bytes) carries it whole, and the RX path decodes it (`docs/architecture.md`).

With `TRANSMIT_TIMING`, `txDelayBase + txDelay` is the moment the end of the SFD is at the antenna, on the same clock as `RCP_TIMESTAMP` and the RX timestamp. A timed frame gets one attempt, with a single CCA if `enableCSMACA` is set. A moment already gone completes as `CCA_FAILURE`, the one transmit failure a host's CSL scheduler retries rather than asserts on.

Refusals, each answered immediately with the command's TID:

| condition | answer |
|---|---|
| `MAC_RAW_STREAM_ENABLED` is false | `LAST_STATUS = INVALID_STATE` |
| a transmission is already in flight | `LAST_STATUS = BUSY` |
| the value ends before `txChannel`, the length is 0 or beyond 127, or the channel is outside 11..26 and not 0 | `LAST_STATUS = PARSE_ERROR` |
| the radio driver refuses to accept the frame | `LAST_STATUS = INVALID_STATE` |
| a frame with the Security Enabled bit that this firmware cannot secure: no key was set, or it is not key ID mode 1 at level 5 (ENC-MIC-32) and the host did not secure it | `LAST_STATUS = INVALID_STATE` |

On acceptance the RCP does **not** answer. It records the request's TID and replies only when the radio reports the outcome. This **deferred completion** is a `VALUE_IS LAST_STATUS` carrying the request's TID, which is the token passed down to `Radio_Tx` and returned by the TX-done handler. Its value is three fields, and up to three more once a key was set, in the order the host's `HandleTransmitDone` unpacks them:

| # | len | type | field |
|---|---|---|---|
| 1 | 1..5 | `i` | `status`, packed uint |
| 2 | 1 | `b` | `framePending`: the received ACK's Frame Pending subfield |
| 3 | 1 | `b` | `headerUpdated`: the frame's key index and frame counter are final |
| 4 | 28 + the Ack | | once a key was set and on `OK`: the Ack as a radio frame in the layout of §7.2, or an empty frame when the radio has none |
| 5 | 1 | `C` | once a key was set, for a secured frame with `headerUpdated`: the key index |
| 6 | 4 | `L` | and its frame counter |

`framePending` is the Frame Pending bit of the Ack the radio received, so it is 1 only for a transmission whose Ack said a frame is waiting; it is 0 after `NO_ACK`, `CCA_FAILURE`, a timeout, or a frame that asked for no Ack. Without a key the completion stops at field 3, so a Zigbee host sees what it always did. The immediate refusals use the same three fields with `framePending` 0, because the host routes any reply bearing the TX TID to its transmit-done handler. What the RCP writes into an Ack *it* sends is a separate question: `docs/ack-pending-frame.md`.

The `status` field names the outcome:

| radio outcome | `status` |
|---|---|
| transmitted (TRX_END) | `OK` (0) |
| clear-channel assessment failed | `CCA_FAILURE` (18) |
| ARET exhausted its retries without an acknowledgment | `NO_ACK` (17) |
| the driver's TX phase timed out (no TRX_END) | `FAILURE` (1); `CCA_FAILURE` (18) once a key is set, since OpenThread's host aborts on `FAILURE` here |

`NO_ACK` comes only from a frame that asked for an Ack and exhausted its retries; a frame that asked for none completes `OK` or `CCA_FAILURE`.

### 7.2 RCP → host: RX notification (unsolicited `VALUE_IS STREAM_RAW`, TID 0)

Offsets are relative to the start of the value (after the property id). The layout matches upstream `NcpBase::PackRadioFrame`.

| offset | len | type | field |
|---|---|---|---|
| 0 | 2 | `S` | `frameDataLen` (little-endian; `0` for an RX error) |
| 2 | n | — | PSDU |
| n+2 | 1 | `c` | `MD_POWER`: RSSI in dBm |
| n+3 | 1 | `c` | `MD_NOISE`: always `-128` (unused) |
| n+4 | 2 | `S` | `MD_FLAG` (little-endian) |
| n+6 | 2 | `S` | `MD_PHY` length = 10, then the struct |
| n+8 | 1 | `C` | channel the frame was received on |
| n+9 | 1 | `C` | LQI, 0..255 |
| n+10 | 8 | `L64` | timestamp in µs, little-endian: with `TRANSMIT_TIMING` the radio clock at the end of the SFD; elsewhere the moment of reception, in milliseconds × 1000 and wrapping at 32 bits |
| n+18 | 2 | `S` | `MD_VEND` length = 1, then the struct |
| n+20 | 1 | `i` | RX error code, packed; `0` = OK |
| n+21 | 2 | `S` | `MD_MAC` length = 5, then the struct |
| n+23 | 1 | `C` | the key index of the secured Enhanced Ack this RCP sent for the frame; `0` otherwise |
| n+24 | 4 | `L` | that Ack's frame counter; `0` otherwise |

The length before the PSDU and the metadata after it are a fixed 28 bytes, 2 and 26, so the value is `n + 28` and the whole notification for a 5-byte PSDU is 33 bytes. A zero-length PSDU is still delivered with its metadata.

`MD_FLAG` bits:

| bit | value | meaning |
|---|---|---|
| 0 | `0x0001` | TX: the frame was sent, not received |
| 2 | `0x0004` | `BAD_FCS`: the frame's FCS failed |
| 3 | `0x0008` | DUPE (defined, not set by this firmware) |
| 4 | `0x0010` | `ACKED_FP`: the Ack this RCP sent for that frame carried Frame Pending set |
| 5 | `0x0020` | `ACKED_SEC`: the Ack this RCP sent for that frame was a secured Enhanced Ack |

`ACKED_FP` is set on exactly the frames this RCP acknowledged with Frame Pending one (§10.2): the only way a host learns what an Ack it never sees said. `RadioSpinel` reads it into `mAckedWithFramePending`. An RX notification never carries `MD_TX`, because a transmission completes with `LAST_STATUS`.

## 8. Reference captures

A ConBee II running this firmware, frame by frame, to compare a host's bytes against. The stream of debug records (§10), if on, comes between these frames.

| step | host sends | RCP answers | decoded |
|---|---|---|---|
| GET `PROTOCOL_VERSION` | `7e 81 02 01 c5 b2 7e` | `7e 81 06 01 04 03 db 0a 7e` | TID 1, `ii` = 4.3 |
| GET `INTERFACE_TYPE` | `7e 82 02 03 b3 7d 5e 7e` | `7e 82 06 03 03 ec 26 7e` | TID 2, `C` = 3 |
| GET `RCP_API_VERSION` | `7e 83 02 b0 01 22 6a 7e` | `7e 83 06 b0 01 04 ca c6 7e` | TID 3, the two-byte packed id `0xB0`, `i` = 4 |
| `RESET` | `7e 84 01 02 8b 93 7e` | `7e 80 06 00 72 fc 57 7e` | TID 0, `LAST_STATUS` = 114 |
| SET `PHY_ENABLED` = 1 | `7e 85 03 20 01 39 62 7e` | `7e 85 06 20 01 84 5b 7e` | the property echo |
| SET `PHY_CHAN` = 26 | `7e 86 03 21 1a 7d 5e f0 7e` | `7e 86 06 21 1a c3 c9 7e` | the request's FCS has an escaped byte |
| SET `PHY_TX_POWER` = −3 | `7e 87 03 25 fd 14 18 7e` | `7e 87 06 25 f4 68 bc 7e` | the echo reads −12: the cap on channel 26 |
| SET `MAC_15_4_SADDR` = 0 | `7e 88 03 35 00 00 dc 85 7e` | `7e 88 06 35 00 00 8b eb 7e` | |
| SET `MAC_15_4_PANID` = `0xBEEF` | `7e 89 03 36 ef be 58 57 7e` | `7e 89 06 36 ef be 0f 39 7e` | little-endian |
| SET `MAC_RAW_STREAM_ENABLED` = 1 | `7e 8a 03 37 01 59 08 7e` | `7e 8a 06 37 01 e4 31 7e` | |
| SET `STREAM_RAW`: a 12-byte broadcast, no Ack, CSMA off, a trailer of 4 bytes | `7e 8b 03 71 0c 00 41 88 01 ef be ff ff 00 00 5a 15 4a 1a 00 00 00 1e 7c 7e` | `7e 8b 06 00 00 00 00 90 e4 7e` | the deferred completion: `OK`, Frame Pending 0, `headerUpdated` 0 |

## 9. Status codes

| code | name | code | name |
|---|---|---|---|
| 0 | `OK` | 11 | `NOMEM` |
| 1 | `FAILURE` | 12 | `BUSY` |
| 2 | `UNIMPLEMENTED` | 13 | `PROP_NOT_FOUND` |
| 3 | `INVALID_ARG` | 14 | `DROPPED` |
| 4 | `INVALID_STATE` | 15 | `EMPTY` |
| 5 | `INVALID_COMMAND` | 16 | `CMD_TOO_BIG` |
| 6 | `INVALID_INTERFACE` | 17 | `NO_ACK` |
| 7 | `INTERNAL_ERROR` | 18 | `CCA_FAILURE` |
| 9 | `PARSE_ERROR` | 20 | `ITEM_NOT_FOUND` |
| 10 | `IN_PROGRESS` | 112 | `RESET_POWER_ON` |
| | | 113 | `RESET_EXTERNAL` |
| | | 114 | `RESET_SOFTWARE` |
| | | 120 | `RESET_WATCHDOG` |

The RCP emits `OK`, `FAILURE`, `UNIMPLEMENTED`, `INVALID_ARG`, `INVALID_STATE`, `INVALID_COMMAND`, `NOMEM`, `BUSY`, `PARSE_ERROR`, `PROP_NOT_FOUND`, `NO_ACK`, `CCA_FAILURE`, `ITEM_NOT_FOUND` and `RESET_SOFTWARE`. `INVALID_STATE` is the answer when a property cannot be honoured in the current state: a GET of `HWADDR` on a part with no factory address, a `SET STREAM_RAW` while the raw stream is disabled, and a `SET STREAM_RAW` the radio refuses.

## 10. Property table

Access is R = GET, W = SET. Types are the Spinel type letters of §4.

| id | property | type | access | notes |
|---|---|---|---|---|
| `0x0000` | `LAST_STATUS` | `i` | R | error and reset reporting; a GET returns `OK` |
| `0x0001` | `PROTOCOL_VERSION` | `ii` | R | major 4, minor 3 → bytes `04 03` |
| `0x0002` | `NCP_VERSION` | `U` | R | `DSPINEL/1.0; <radio>; phoscon-rcp <version>`. The radio is `AT86RF233` (ConBee II, RaspBee II), `EFR32MG21` (ConBee III) or `ATmega256RFR2` (ConBee I, RaspBee I); the version is the release, or the release and commit between releases (`docs/build-and-flash.md`). A host keys on the `DSPINEL/1.0` prefix alone |
| `0x0003` | `INTERFACE_TYPE` | `C` | R | `3` = SPINEL |
| `0x0005` | `CAPS` | `A(i)` | R | the five capabilities true of this firmware (below) |
| `0x0008` | `HWADDR` | `E` | R | the factory EUI-64 (see below) |
| `0x0020` | `PHY_ENABLED` | `b` | R/W | radio on/off; default 0 |
| `0x0021` | `PHY_CHAN` | `C` | R/W | 11..26; default 0; any other value is `INVALID_ARG` |
| `0x0022` | `PHY_CHAN_SUPPORTED` | `A(C)` | R | the channels 11..26, on every board |
| `0x0024` | `PHY_CCA_THRESHOLD` | `c` | R/W | CCA threshold in dBm; default -71. Programmed into the radio; the AT86RF233 snaps it down to its 2 dB steps from -94 to -64 dBm (`docs/hardware_samr21.md`), and a GET echoes the value the host wrote |
| `0x0025` | `PHY_TX_POWER` | `c` | R/W | dBm at the transceiver; default 0. Capped per board, lower on channel 26 (ConBee II -3 / -12, RaspBee II -6 / -17, ConBee III 10 / 10, ConBee I / RaspBee I -11 / -11; `docs/hardware_samr21.md`, `docs/hardware_avr.md`); a GET reads back the capped value for the current channel, and the host's own value is kept, so it applies again when the channel changes |
| `0x0026` | `PHY_RSSI` | `c` | R | always -128: nothing updates it, so measure with the energy scan (§10.1) |
| `0x0027` | `PHY_RX_SENSITIVITY` | `c` | R | per silicon, a declared capability, not a measurement: -96 for the AT86RF233 (cb2, rb2), -104 for the MG21 on-die radio (cb3), -100 for the ATmega256RFR2 (cbrb1) |
| `0x0029` | `PHY_CHAN_PREFERRED` | `A(C)` | R | the channels 11..26, on every board |
| `0x0030` | `MAC_SCAN_STATE` | `C` | R/W | 0 idle, 2 an energy dwell running; 1 and 3 answer `UNIMPLEMENTED` (10.1) |
| `0x0031` | `MAC_SCAN_MASK` | `A(C)` | R/W | list of channel numbers; default the whole 11..26 band (10.1) |
| `0x0032` | `MAC_SCAN_PERIOD` | `S` | R/W | milliseconds per channel; default 200 (10.1) |
| `0x0034` | `MAC_15_4_LADDR` | `E` | R/W | the host-set extended address, big-endian |
| `0x0035` | `MAC_15_4_SADDR` | `S` | R/W | short address; default `0xFFFE` |
| `0x0036` | `MAC_15_4_PANID` | `S` | R/W | PAN id; default `0xFFFF` |
| `0x0037` | `MAC_RAW_STREAM_ENABLED` | `b` | R/W | gate for `STREAM_RAW`; default 0 |
| `0x0038` | `MAC_PROMISCUOUS_MODE` | `C` | R/W | 0, 1 or 2 (below); default 0 |
| `0x003B` | `MAC_RX_ON_WHEN_IDLE` | `b` | R/W | stored and echoed; default 0. Nothing reads it: the receiver is on while `PHY_ENABLED` and `MAC_RAW_STREAM_ENABLED` are both set, whatever this says |
| `0xB0` | `RCP_API_VERSION` | `i` | R | `4` |
| `0xB1` | `RCP_MIN_HOST_API_VERSION` | `i` | R | `4` |
| `0xB2` | `RCP_LOG_CRASH_DUMP` | `C` | R/W | in-band debug-record stream (below) |
| `0x0800` | `RCP_MAC_KEY` | struct | W | key ID mode, ignored; key index; previous, current and next key, each 16 octets with a 16-bit length. Answered with `LAST_STATUS`, never echoed; a GET is `PROP_NOT_FOUND`. Held in RAM, forgotten on `RESET` |
| `0x0801` | `RCP_MAC_FRAME_COUNTER` | `L` or `Lb` | W | the next frame counter; with the flag set, only a larger value replaces it |
| `0x0802` | `RCP_TIMESTAMP` | `X` | R | the radio's clock in microseconds, at 1 µs. A GET may carry a payload, which is ignored |
| `0x0803` | `RCP_ENH_ACK_PROBING` | struct | W | short address, extended address, metrics byte (LQI `0x02`, link margin `0x04`, RSSI `0x08`, at most two used; `0` ends it), up to 8 sources. An Ack to one carries the metrics of the frame before it |
| `0x0804` | `RCP_CSL_ACCURACY` | `C` | R | the clock's accuracy, 40 ppm, 802.15.4's limit for the crystal |
| `0x0805` | `RCP_CSL_UNCERTAINTY` | `C` | R | a timed frame's uncertainty, in units of 10 µs: 20 |
| `0x120B` | `RADIO_CAPS` | `i` | R | the `OT_RADIO_CAPS_*` bits this radio has (below) |
| `0x1303` | `MAC_SRC_MATCH_ENABLED` | `b` | R/W | default 0 (10.2) |
| `0x1304` | `MAC_SRC_MATCH_SHORT_ADDRESSES` | `A(S)` | R/W, INSERT, REMOVE | default empty (10.2) |
| `0x1305` | `MAC_SRC_MATCH_EXTENDED_ADDRESSES` | `A(E)` | R/W, INSERT, REMOVE | default empty (10.2) |

- **`HWADDR`** is the factory EUI-64 deCONZ knows the stick by (the NVM user row, the ConBee III's manufacturing token, the AVR boards' EEPROM), most significant byte first and read-only; a part with no valid factory address answers `INVALID_STATE`. `MAC_15_4_LADDR` is the address the host sets for the radio's filter.
- **`CAPS`** is a list of capability numbers, as OpenThread tests membership: `CONFIG_RADIO` (34), `RCP_API_VERSION` (64), `RCP_MIN_HOST_API_VERSION` (65), `RCP_LOG_CRASH_DUMP` (67), `MAC_RAW` (513).
- **`RADIO_CAPS`** advertises `ACK_TIMEOUT`, `ENERGY_SCAN`, `TRANSMIT_RETRIES`, `CSMA_BACKOFF`, `RX_ON_WHEN_IDLE`, `TRANSMIT_SEC` and `TRANSMIT_TIMING`, and nothing else. A host that sets `MAC_RX_ON_WHEN_IDLE` for that bit is answered, but the setting changes nothing (above).
- **`MAC_PROMISCUOUS_MODE`:** 0 OFF — the address filter, and the RCP acknowledges; 1 NETWORK — every frame on this PAN; 2 FULL — every frame, FCS-invalid ones flagged, no Acks (the sniffer mode). Any other value is `INVALID_ARG`.
- **`RCP_LOG_CRASH_DUMP` (`0xB2`)** streams the debug records in-band once a host writes 1 to it, until it writes 0: a `RESET` does not stop it; an empty write, OpenThread's request for a crash dump, is answered `OK` and starts nothing. Each notification is a little-endian `u16` count and up to 19 eight-byte records.
- **Handled outside the table:** `STREAM_RAW` (`0x71`, §7); `STREAM_DEBUG` (`0x70`) is the probe image's record channel. The scan events `0x33` and `0x39` are notifications, not properties: a GET answers `PROP_NOT_FOUND`.

### 10.1 The scan family

One energy measurement of one channel: the RCP retunes, measures for `MAC_SCAN_PERIOD` milliseconds, reports the highest level, and puts the host's receive state back (Figure 4). That is upstream's link-raw energy scan; what this firmware adds is listed at the end.

![A timeline with the host above and the RCP below. The host sends SET 0x30 = 02; the RCP tunes, then takes measurements, each a request, a confirmation and a read, drawn in amber. A SET STREAM_RAW arrives and borrows the radio for a transmission; the RCP tunes again and measures on. A bracket under the measurements says MAC_SCAN_PERIOD: the board's clock ends it. The RCP then restores the host's receive state and emits, and the host receives VALUE_IS 0x39 with the channel and its level in dBm, then 0x30 = 00: the result, then the completion. Not to scale.](images/scan-dwell.png)

*Figure 4 — one energy-scan dwell. Amber is a measurement; the period, on the board's own clock, ends the dwell. Source: `tools/diagrams/scan_dwell.py`.*

| property | fresh value | a write |
|---|---|---|
| `MAC_SCAN_STATE` (`0x30`) | `0` | `2` starts a dwell; `0` is accepted and changes nothing; `1`, `3` (beacon, discover) answer `UNIMPLEMENTED` |
| `MAC_SCAN_MASK` (`0x31`) | channels 11..26 | stores the list, in either shape of §4; a value in neither shape, or a channel above 31, is `INVALID_ARG` |
| `MAC_SCAN_PERIOD` (`0x32`) | `200` | stored as written; used bounded to 1..1000 ms, so a mistaken value cannot hold the radio |

The dialog: `SET 0x31 = 01 0F` (channel 15), `SET 0x32 = 64 00` (100 ms), `SET 0x30 = 02`, each echoed; then, unsolicited, `VALUE_IS 0x39 0F <dBm>` and `VALUE_IS 0x30 00`.

| a start is refused when | answer |
|---|---|
| `PHY_ENABLED` is 0, or a dwell already runs | `INVALID_STATE` |
| the mask is empty, holds more than one channel, or a channel outside 11..26 | `INVALID_ARG` |

- **`0x39`** carries the channel and the highest level seen, in dBm. `+127` means nothing usable was measured.
- **A transmission outranks a dwell:** the measurement in progress is abandoned, and the dwell re-tunes after it. The period keeps running meanwhile.
- **Endings.** The period ends a dwell. `PHY_ENABLED` or `MAC_RAW_STREAM_ENABLED` going to 0 ends it early with `+127` and a completion; a `RESET` ends it silently. `SET 0x30 = 0` does not cancel one.
- **While a dwell runs**, received frames are not delivered, so the result cannot be pushed out of the reply ring by a busy channel.

Extensions, none of them upstream behaviour: `0x39` and `0x30 = 0` sent as notifications (upstream ends a scan with a callback inside its own process); the period bounded where it is used; an early end that still reports and completes (upstream's disarm wedges a host in its own timeout); and the interlock with transmissions, which arrive on the same wire.

### 10.2 The source-match family and the Ack's Frame Pending bit

A coordinator tells a polling device whether it has a frame queued for it, in the Frame Pending bit of the Ack to its Data Request. The host keeps the list of addresses with something queued in the RCP:

| id | property | type | access | default | value |
|---|---|---|---|---|---|
| `0x1303` | `MAC_SRC_MATCH_ENABLED` | `b` | R/W | `0` | `0`: every Ack to a Data Request carries Frame Pending one. `1`: only for an address in a table |
| `0x1304` | `MAC_SRC_MATCH_SHORT_ADDRESSES` | `A(S)` | R/W, INSERT, REMOVE | empty | concatenated 16-bit addresses, little-endian, no count prefix |
| `0x1305` | `MAC_SRC_MATCH_EXTENDED_ADDRESSES` | `A(E)` | R/W, INSERT, REMOVE | empty | concatenated 8-byte addresses, in the byte order `MAC_15_4_LADDR` uses |

The ids and wire shapes are upstream's, so a host that drives an OpenThread RCP drives this one with the same calls. `INSERT` and `REMOVE` carry one entry and answer `VALUE_INSERTED` / `VALUE_REMOVED` with it.

Four points differ from upstream deliberately:

| point | upstream | here | why |
|---|---|---|---|
| GET of `0x1304` / `0x1305` | no handler, `PROP_NOT_FOUND`; a SET is answered with a bare `LAST_STATUS OK` | readable, and a SET is answered with the stored list | §5's rule: every accepted SET is confirmed with its property echo. A host that waits for the value of the key it set accepts either |
| a SET that does not fit | clears, adds until the table is full, answers `NOMEM`, leaves a partial list | refused whole, table unchanged | a partial list claims "nothing queued" for addresses the host believes it wrote |
| duplicate `INSERT` | stores a duplicate | stores one | a following `REMOVE` then removes the address rather than one copy of it |
| `REMOVE` of an absent entry | a stack-native error code | `ITEM_NOT_FOUND` (20) | a defined status a host can test |

`RESET` clears both lists and sets `ENABLED` back to `0`.

**What the Ack says.** The RCP composes the Ack per frame (`docs/ack-pending-frame.md`):

| received frame, OFF mode, addressed to this node, AR set | Frame Pending in the Ack |
|---|---|
| Data Request, `ENABLED` = 0 | 1: the coordinator cannot know, so it claims pending (IEEE 802.15.4-2024 §10.22.3) |
| Data Request, `ENABLED` = 1, source address in the matching list | 1 |
| Data Request, `ENABLED` = 1, source address not in it | 0 |
| a MAC command frame with security enabled | 1: its command identifier is behind the auxiliary security header |
| any other frame | 0 |

A table update takes effect before the SET, INSERT or REMOVE is answered, so the next poll is answered from the new table. `MD_FLAG` bit 4 tells the host what each Ack said (§7.2).
