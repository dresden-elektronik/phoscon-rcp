# Changelog

Each release of the firmware, newest first. A stick reports its release in `NCP_VERSION` (`README.md`).

## 0.3.1

- ConBee II, RaspBee II: the Ack of a long frame goes out in its slot. It left up to about 370 µs after a long frame, too late for a sender that waits less, so a sleepy Thread child such as an IKEA MYGGBETT never attached directly to the stick; it now leaves about 200 µs after a frame of any length (`docs/ack-pending-frame.md`).

## 0.3.0

- A RESET drops what still waits for the host, so nothing from before the reset follows the reset status. A stick on USB left receiving by a host that closed the port no longer hands the next host a backlog behind the status; the few frames the link delivers before the RESET arrives still come first (`docs/protocol.md`).
- An experimental build target for the Seeed Studio XIAO ESP32-C6, outside the family; no release carries it (`docs/boards.md`).

## 0.2.0

- ConBee III: the host may raise the link to 1 000 000 Bd with `UART_BITRATE`; a RESET, or a host that starts over at 115 200, returns it to 115 200 (`docs/protocol.md`).
- ConBee III: the link to the host can no longer stall on a lost TX-complete flag.
- ConBee II: a transmission no longer waits a tick between its steps, so more frames a second go out (`docs/stats.md`).
- A transmission in flight at RESET finishes unanswered, and RESET clears the MAC security state, the source match table and every property to its power-on value.
- A frame's own channel stands until the host writes `PHY_CHAN`.
- The watchdog resets a stick whose transmission stays stuck, and the reset cause reaches the host on the ConBee II as well.
- ConBee III: no Ack for a frame the receive ring cannot take, and an Ack's Frame Pending bit travels with its own frame.
- ConBee I, RaspBee I: an Ack that ends while an Enhanced Ack's MIC is computed no longer turns the receiver on early.
- The flasher's reset frame is never matched inside a Spinel frame.
- A property id past 32 bits is malformed and goes unanswered.
- The pre-flash guard refuses a container that is not its `.bin`'s.

## 0.1.0

The first public release, an early one.

- `rcp` images for the ConBee III, ConBee II, RaspBee II, and the ConBee I and RaspBee I.
- Host links: ConBee II over native USB; ConBee III and RaspBee II at 115 200 baud; ConBee I and RaspBee I at 57 600 baud.
- What each board supports is `docs/protocol.md`, and what has run on it is `docs/testing.md`.
