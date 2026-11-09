# Statistics

What the RCP images achieve on real sticks, measured with the rigs of `docs/testing.md`. Each figure is generated from the values in its script, which are updated when a measurement changes.

## 1. Throughput

Between two ConBee IIs a full frame reaches the receiving host 108 times a second with CSMA-CA, 100 kbit/s of MAC payload, and 126 times a second without it: the air, the backoff and the Ack take most of each cycle. A ConBee III receives at the same rate once its host raises the link to 1 000 000 Bd with `UART_BITRATE` (`docs/protocol.md`). As a sender it reaches 89 and 111 at that rate with its bridge's latency timer at 1 ms. At its power-on 115 200 Bd and 16 ms timer, the link holds it to 69 frames a second received and 31 sent.

![Horizontal bars on one scale from 0 to 200 frames a second, two bars per group: amber with CSMA-CA, white without, each labelled with its frames a second and kbit/s of MAC payload. ConBee II to ConBee II over USB CDC: 108.0 fps and 100.3 kbit/s with CSMA-CA, 125.5 and 116.4 without. ConBee II to ConBee III into UART 115 200: 68.8 fps and 63.9 kbit/s, 68.0 and 63.1; each bar continues hatched to what the sender saw acknowledged, 110 and 129 frames a second. ConBee II to ConBee III into UART 1 000 000: 110.5 fps and 102.5 kbit/s, 128.3 and 119.1. ConBee III to ConBee II from UART 115 200: 31.3 fps and 29.0 kbit/s either way. From UART 460 800 with a 1 ms timer: 87.2 fps and 80.9 kbit/s, 97.7 and 90.6. From UART 1 000 000 with a 1 ms timer: 89.0 fps and 82.6 kbit/s, 110.9 and 103.0.](images/throughput.png)

*Figure 1 — acknowledged full frames a second that reach the receiving host, and the MAC payload they carry. Amber is CSMA-CA on; hatched is what the receiver's radio acknowledged and its host never saw. Source: `tools/diagrams/throughput.py`.*

| condition | value |
|---|---|
| tool | `tools/rcp_throughput.py`, one direction a run |
| frame | 127-octet PSDU with its FCS, 116 octets of MAC payload, Ack requested |
| channel | the quietest of an energy scan; traffic on the channel lowers every bar |
| run | 10 s, `maxRetries` 3, CSMA-CA as the request's `enableCSMACA` |
| sender's host | macOS |
| ConBee III link | the rate in the label, set with `--bitrate` past 115 200; the bridge's latency timer at 16 ms, its power-up value, unless the label says 1 ms |

- **A ConBee III receiving faster than its UART carries acknowledges frames its host never sees.** At 115 200 Bd a full frame's report takes about 14 ms, so about 69 a second reach the host; the radio still acknowledges every frame, and the sender is told `OK` for frames that are then lost. At 460 800 Bd and above every report arrives.
- **A ConBee III sending waits on its bridge's latency timer**, once for the request and once for the completion, so behind the 16 ms timer it sends at most 62 frames a second at any line rate. The timer is the host's to set (`docs/protocol.md`).
- **At 1 000 000 Bd a ConBee III that sends with CSMA-CA leaves about one request in a thousand without a completion**, and the host times out. The link has no flow control. At 460 800 Bd every request completed, at 87 frames a second with CSMA-CA and 98 without.
