# AGENTS.md — agent contract for Phoscon RCP

Read this first. It is the contract for working in this repository: the hard rules, the exact commands, and where the documentation lives. The full text of every rule is in the doc it points at — point at it, do not restate it. **If `AGENTS.local.md` exists, read it next**: maintainers keep the bench, its ports, what may be flashed and the local tool paths there, outside the tracked tree.

## What this project is

Phoscon RCP (repository `phoscon-rcp`, build artifact `phoscon-rcp`): Spinel radio co-processor (RCP) firmware for the deCONZ ConBee/RaspBee family (`README.md`). Its reference host is Phoscon Zigbee Stack (repository `phoscon_zbstack`), released separately. A host runs the Zigbee stack; this firmware runs on the adapter's MCU and exposes its 802.15.4 radio as HDLC-framed Spinel. It implements only the PHY/MAC surface, is stateless across reset, and is driven entirely by the host. The deCONZ NVRAM and the bootloader must survive every flash. One tree builds two images per board — `probe` (debug records, the default) and `rcp` — for four board targets:

| target | boards | silicon layer | facts |
|---|---|---|---|
| `cb2` | ConBee II | `src/hal/samr21/` | `docs/hardware_samr21.md` |
| `rb2` | RaspBee II | `src/hal/samr21/`, UART host link | `docs/hardware_samr21.md` §9 |
| `cb3` | ConBee III | `src/hal/mg21/`: the `rail` back-end of the `rcp` image; the `own` back-end stays in the tree, unselected (`-DDS_MG21_RADIO=own`); `probe` drives no radio | `docs/hardware_mg21.md` |
| `cbrb1` | ConBee I, RaspBee I | `src/hal/avr/`, no source shared with `src/hal/samr21/` | `docs/hardware_avr.md` §11 |

What each board supports is `docs/protocol.md`, and what has run on it is `docs/testing.md`. Do not restate either here.

## Hard constraints (full text: `docs/coding-rules.md`)

- C99 freestanding; on target the only standard header is `<stdint.h>` — §1.
- No `bool`/`_Bool`: booleans are `int` 0/1 — §1.
- No C++, and no assembly beyond the four core instructions (`HAL_Wfi`, `HAL_IrqLock`, `HAL_IrqUnlock`, `HAL_Dsb`) — §1.
- No libc and no dynamic allocation of any kind: statically sized, named pools only — §7.
- All binary I/O goes through `U_BStream`; all strings through `U_SStream` — §1.
- Naming: public `UpperCamel` (`HAL_Init`), private/static `lowerCamel` (`halDoX`), types `UpperCamel`, constants `UPPER_SNAKE`; state machines everywhere — §2, §3.
- The core/glue split is a design rule: core never calls `HAL_*`/`Radio_*` or reads a register; time arrives as `nowMs`, state is caller-owned — §4. A core module without a host suite is not finished — §4.5.

## Commands (details: `docs/build-and-flash.md`, `docs/testing.md`)

Host suites — plain C99, host toolchain, no target toolchain and no hardware:

```
cmake -B build/test -S . && cmake --build build/test
ctest --test-dir build/test --output-on-failure
```

Doc lints — no build, no hardware, run before committing a doc change:

```
python3 tools/unwrap_md.py --check README.md CONTRIBUTING.md AGENTS.md docs/*.md   # one line per paragraph
python3 tools/docrefs.py                                                          # every §N resolves
python3 tools/diagrams/all.py --check                                             # every figure current (Chrome)
```

Firmware — the default image is `probe`; `-DDS_MODE=rcp` selects the RCP image. **Never pass `-DCMAKE_BUILD_TYPE=Release`.**

| target | toolchain file | configure |
|---|---|---|
| `cb2`, `rb2` | `cmake/arm-none-eabi.cmake` | `cmake -B build/fw -S src -G Ninja -DDEBOARD=cb2 -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake` |
| `cb3` | `cmake/arm-none-eabi-m33.cmake` | `cmake -B build/fw-cb3 -S src -G Ninja -DDEBOARD=cb3 -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi-m33.cmake` |
| `cbrb1` | `cmake/avr-gcc.cmake` (`avr-gcc` 14, `docs/build-and-flash.md` §11) | `cmake -B build/fw-cbrb1 -S src -G Ninja -DDEBOARD=cbrb1 -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/avr-gcc.cmake` |

Then `cmake --build <dir>`. Measured `.bin` sizes per board and image are the figures of `docs/build-and-flash.md` §3; update them in `tools/diagrams/image_sizes.py` when they change. Versions and the release steps: `docs/build-and-flash.md` §12.

Package the app as a GCF container with `GCFTool`, guard it, then flash:

```
GCFTool -b cb2 build/fw/phoscon-rcp.bin        # -b rb2 / -b cb3 / -b cbrb1
python3 tools/gcf_check.py build/fw/phoscon-rcp.bin.GCF build/fw/phoscon-rcp.bin --board cb2
GCFFlasher -t 20 -f build/fw/phoscon-rcp.bin.GCF -d <port>    # GCFFlasher 4.12.6 or newer
```

- **Pre-flash guard (mandatory).** Every flash path runs `tools/gcf_check.py --board <board>` before GCFFlasher — magic, the board's type range (never plaintext), the board's target, the region the image must not reach, and freshness against the `.bin` (`docs/build-and-flash.md` §6). A failed build must never be packaged or flashed: a stale `.GCF` is otherwise flashed silently.
- Before anything is flashed for the first time, a board target passes the audit gates of `docs/testing.md` §5, in its order.

## Documentation is the project's memory

- Findings go into the doc that **owns the subject**; there is no diary. History — how a fact was found, what was tried and removed — does not go into a tracked doc; git keeps the commits.
- The doc set: `README.md` (entry point: what this is, how to flash it) · `CONTRIBUTING.md` (issues, not pull requests; the rules and checks for a change) · `docs/boards.md` (the family map, and adding a board) · `docs/architecture.md` (layers, modules, data flow, buffers, state machines) · `docs/protocol.md` (host-facing Spinel contract) · `docs/hardware.md` (index of the hardware docs) · `docs/hardware_samr21.md` (`§1`–`§9`) · `docs/hardware_mg21.md` (`§10`–`§10.9`) · `docs/hardware_avr.md` (`§11`–`§11.8`) · `docs/ack-pending-frame.md` (the Ack's Frame Pending bit) · `docs/build-and-flash.md` (toolchain, artifacts, packaging, flashing, guard, recovery, releases) · `docs/testing.md` (suites, rig, emulator gates) · `docs/coding-rules.md` (the binding C conventions).
- The reader is a human engineer new to the project: lead each section with its conclusion, one claim per paragraph, tables over prose, and a generated figure (`tools/diagrams/`) over complex prose.
- **Keep the docs short.** Every paragraph must answer "does a reader of a mature project need this?" Detail the code and its comments already carry, research narrative and measurement logs do not belong in a doc. Point to a document rather than a section unless the section matters.
- House rules for every doc: no `file:line` references, no dates, no `row N`, no "we tried"; impersonal present tense; section numbers are cited from source comments, so a renumbering rewrites every citation in the same commit (`tools/docrefs.py` checks that every `§N` points at a section that exists); the hardware docs share one numbering, and a new silicon starts at the next free top-level number; one purpose per doc.
- **Public sources only.** Docs cite datasheets, SDKs and upstream projects by name and version; no paths into an untracked reference corpus, no bench serial numbers or device paths, no home-directory paths, and never the name of the keyed packaging tool.
- **No hard-wrapped prose.** One paragraph or list item per line, however long it is. `tools/unwrap_md.py` (dry run) and `tools/unwrap_md.py --apply` (fix) join wrapped prose without touching tables or code blocks.
- A design decision belongs in `docs/architecture.md` or in the hardware doc of the silicon it is about, not in a rule here. Where sources conflict, the code wins.

## Working agreements

- **The wire identity is fixed.** `NCP_VERSION` starts with `DSPINEL/1.0` on every board, followed by the radio and the version: Phoscon Zigbee Stack picks this firmware's protocol variant by that prefix, so it does not follow the project name (`docs/protocol.md`).
- **Explicit ports.** Several sticks of one kind share a port-name prefix; always pass the port explicitly, to this tree's tools and to GCFFlasher.
- **No physical flashing without authorisation.** Do not flash a board unless the task explicitly says so (`AGENTS.local.md` records standing authorisations).
- **One vendor library may be linked: RAIL, by the ConBee III's `rail` radio back-end.** The library and its headers are fetched at configure time into a cache outside the tree by `tools/fetch_rail.py`, every file pinned by SHA-256, and never enter the repository or `src/`; only this tree's adapter (`src/hal/mg21/rail/`) lives in `src/`. Provenance and licence: `src/lib/PROVENANCE.md`; mechanism and cache: `docs/build-and-flash.md` §10.
- **Reference material is read, never copied.** Vendor sources, other stacks and datasheets exist to be understood; nothing is copied verbatim into `src/` - read it, then write from scratch. The vendored libraries in `src/lib/` (`src/lib/PROVENANCE.md`) are never restyled.
- **Commits.** Small, descriptive commits; one concern per commit; commit doc updates with the code they describe.
- **Sub-agents.** One writer at a time when a step rebuilds the tree — the verification for every step rebuilds everything and runs the suites, so parallel writers race on the build directories. A worker touches only its assigned files, and reports cite `file:line` and datasheet references for every claim (the no-`file:line` rule binds documents, not reports).
