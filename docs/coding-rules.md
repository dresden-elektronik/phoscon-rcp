# Coding Rules

The binding conventions for every C source in this repository (firmware `src/`, host tools `tools/`), on every board. Read it before writing code; a change that breaks a *mandate* is not merged. A board-only rule says so and points at `docs/boards.md`.

| marker | meaning |
|---|---|
| *mandate* | fixed |
| *default* | may be revised by decision |

| § | section | covers |
|---|---|---|
| 1 | [Language and build](#1-language-and-build) | C99, no libc headers, no assembly, streams, CMake |
| 2 | [Naming](#2-naming) | case and prefix per kind of symbol |
| 3 | [State machines everywhere](#3-state-machines-everywhere) | the shape every multi-mode component takes |
| 4 | [Structure: core / glue split](#4-structure-core--glue-split-testability) | the testability rule, dependency direction, pools, fault injection, done |
| 5 | [Debug interface](#5-debug-interface-binary) | the 8-byte binary record; no strings |
| 6 | [Style](#6-style) | layout and comments, applied by hand |
| 7 | [Memory](#7-memory) | no allocation, no implicit `memcpy`, static bounds |
| 8 | [References](#8-references-where-the-rules-come-from) | where the rules come from |

Design context: `docs/architecture.md`, `docs/boards.md`, the hardware docs (`docs/hardware.md`), `docs/protocol.md`, `docs/testing.md`.

## 1. Language and build

- **C99**, freestanding: `-std=c99 -Os -mcpu=cortex-m0plus -mthumb`, linked `-nostdlib` (toolchain `cmake/arm-none-eabi.cmake`; these are the SAMR21 boards' flags and startup — the ConBee III builds with `cmake/arm-none-eabi-m33.cmake`, the ConBee I / RaspBee I with `cmake/avr-gcc.cmake`; see `docs/build-and-flash.md`). No C++. The only assembly is the startup files and the four core instructions in `hal.h`, one set per instruction set: `wfi`, `dsb 0xf`, `cpsid i`, `cpsie i` on the Cortex-M parts; `sleep`, `cli`, `sei` on the ATmega256RFR2 (`cbrb1`), whose `HAL_Dsb` is an empty compiler barrier because the core has no write buffer to drain. The AVR watchdog restarts only on its own instruction, which `hal/avr/avr_wdt.c` reaches through the compiler built-in `__builtin_avr_wdr`: a built-in, not assembly. *mandate*
- **No `bool`/`_Bool`.** Booleans are `int` (0/1), named accordingly (`int isReady;`, `int hasFrame;`). *mandate*
- **Binary parsing/writing goes through `U_BStream`** (`git.sr.ht/~cryo/u_bstream`); **string building/parsing through `U_SStream`** (`git.sr.ht/~cryo/u_sstream`). No ad-hoc pointer arithmetic, no hand `memcpy`, no `sprintf`/`strlen` on target (host tools may use libc). *mandate*
- **No verbatim reuse of reference code.** `src/` is written from scratch; reference sources (vendor firmware, other projects' ports, SDK examples) are for understanding only. The vendored `U_BStream`/`U_SStream` under `src/lib/` are the only code from outside this project; the USB device stack under `src/usb/` is the project's own released `usbd` driver, adopted into the tree. *mandate*
- **Standard library: on target, only `<stdint.h>`.** Any other header or feature (`stddef.h`, `stdarg.h`, `string.h`, …) needs approval first; host tools/tests may use libc. *mandate* — approved exceptions: the RAIL adapter's one translation unit (`hal/mg21/rail/mg21_rail.c`), which includes the vendor library's headers (`<stdbool.h>`, `<stddef.h>` and, through a stand-in in `hal/mg21/rail/compat/`, `<string.h>`) because RAIL's API is written in them; no other file of the tree includes them, and the adapter's own code uses `int` for booleans (`docs/hardware_mg21.md`).
- **CMake only.** One project builds the firmware (`src/`); the root project builds the desktop tests (`test/`). No Makefiles. *mandate*

## 2. Naming

| Thing                                            | Case                                      | Example                                                      |
| ------------------------------------------------ | ----------------------------------------- | ------------------------------------------------------------ |
| Public function (in a module header)             | UpperCamel, `Module_Operation`            | `HAL_Init`, `Radio_SetChannel`, `Props_Get`                  |
| Private / static function                        | lowerCamel, `moduleOperation`             | `rcpAppQueueReply`, `clockWaitWdtSync`, `usbcBanksClear`     |
| Types (struct/union/enum)                        | UpperCamel                                | `Props_State`, `Rcp_Session`, `Radio_Frame`, `Dbg_Component` |
| Enum constants, macros, constants                | UPPER_SNAKE                               | `RADIO_TX_OK`, `DBG_COMP_CORE`, `HDLC_FRAME_MAX`             |
| Global variables (avoid; module file-scope only) | `s_` module prefix                        | `s_commit`                                                   |
| Files                                            | snake_case, one module per `.c`/`.h` pair | `samr21_radio.c` / `.h`, `props.c` / `.h`                    |

- The module prefix *is* the lower/upper-cased module name (`hal` → `HAL_`/`hal…`, `props` → `Props_`/`props…`); it is always on public symbols so headers can be included in any order without collisions. *mandate*
- On USB boards the adopted device stack (`usb/usbd_*`, the project's own released driver) keeps its upstream `usbd_*` / `usbd_state_t` symbols verbatim so future syncs stay a plain diff; the one naming exception (`docs/boards.md`). *mandate*
- One public header per module: declarations and types only. *default*

## 3. State machines everywhere

Any component with more than two meaningful modes is an **explicit state machine**:

- A named enum whose members name what it *is doing* (e.g. `usbd_state_t`, the `RADIO_TX_*` result set).
- One `…_Step(void)`/`…_Poll(void)` entry point advances it; transitions are an explicit `switch` (or table), never scattered flags.
- State is owned by one machine, which others observe or command through the public interface, never poke; ISRs only *enqueue* events and change state atomically, with protocol handling in loop context.

Applied at minimum to: USB bring-up and the CDC class (USB boards), the radio (RX/TX/promiscuous; the AT86RF233 on the SAMR21 boards), and the RCP session/HDLC link layer — board scope in `docs/boards.md`. *mandate*

## 4. Structure: core / glue split (testability)

Every non-trivial module splits into:

- **core** — pure C: state machines, parsers, serializers, checks, on plain buffers + `U_BStream`/`U_SStream`, no registers, hardware or globals. Runs on the desktop; unit-tested without target tooling.
- **glue** — thin register reads/writes, IRQ handlers, clock/flash: no logic beyond mapping a core decision onto hardware. A function that needs a `volatile` pointer to pass belongs here. *default*

### 4.1 Dependency direction

**Core never calls out.** No `HAL_*`, no `Radio_*`, no register or clock read inside a pure module; `app/` and the board HAL (`hal/samr21/`; `docs/boards.md`) call core, core calls nothing. Time is a parameter (`uint32_t nowMs`); remembered state is caller-owned. This is the rule that lets most host suites need no fake. *mandate*

### 4.2 State is caller-owned

Module state lives in a caller-provided struct or behind one explicit reset function — never a hidden file-scope variable. Two instances must coexist in one process, and a test must reset the module without restarting. *mandate*

### 4.3 Every pool states and tests its full-pool policy

A module that owns buffers says what happens when they are full, and counts it: a stated, counted drop policy (drop-newest, as the radio RX ring does; or drop-oldest, as the RCP RX ring and the debug record queue do), never block, never overwrite a slot in flight. The counter is readable through the diagnostic channel, so the board can be checked against the host's count. *mandate*

### 4.4 Fault injection is part of the suite

For every boundary, feed truncated, oversized, escape-heavy and malformed input and assert the *bounded* response (drop, resync, error status), not merely "no crash". Byte parsers also get one deterministic pseudo-random stream test (fixed seed, bounded length) asserting the decoder never overruns its buffer and always resyncs. *mandate*

### 4.5 A module without a suite is not finished

Definition of done for a core module: host-clean under `-Wall -Wextra -std=c99`, registered in `test/CMakeLists.txt`, green in `ctest`, bounds and error paths covered. Glue is exempt by construction (§4.1); the board covers it. *mandate*

See **`docs/testing.md`** for the harness, fakes and what is not unit-tested.

## 5. Debug interface (binary)

The firmware contains **no debug strings** — no logging text, no formatted panic messages, no banners. Diagnostics are fixed-size 8-byte (`DBG_FRAME_SIZE`) **binary debug records** (little-endian, built with `U_BStream`); an extension is a new version with a different magic, never a change to v1:

```
offset  size  field
0       u16   magic      0x0000 (constant, identifies a debug record)
2       u16   component  DBG_COMP_* id of the emitting module
4       u16   line       source line (__LINE__)
6       u16   reason     module-local reason code (DBG_…_RSN_*)
```

- Emission is `Debug_Emit(component, line, reason)` via the HAL IO (CDC on USB boards — `docs/boards.md`); the call site uses `DBG(comp, reason)`, which captures `__LINE__`. Nothing to format. *mandate*
- Component and reason codes are plain `enum`s in one central header (`src/hal/dbg.h`: `DBG_COMP_*`, per-module `…_RSN_*`, plus local `#define`s). Adding a code is one line; codes are never reused or renumbered. *mandate*
- The **string table lives on the host side**: `tools/dbg.py` maps `(component, reason)` → text, so firmware growth is independent of verbosity. *mandate*
- Every state-machine error branch, every boot-failure path and the panic handler emit a record. The ConBee II has **no LED**, so pre-USB failures are device-side invisible; recovery is power-cycle → bootloader window → re-flash. The WDT (bootloader-armed with `ALWAYSON`, ~8 s, un-disablable on the SAMR21 targets) is fed from the main loop and by every bounded bring-up wait, so a hung app auto-resets into the bootloader window (`docs/boards.md`). *mandate*

See `docs/architecture.md` for the record queue and shadow/replay, and `docs/hardware_samr21.md` for the watchdog and recovery.

## 6. Style

**The style is applied by hand; there is no formatter in the tree.** clang-format cannot express the single-statement brace rule.

- **`tools/token_equiv.py REV <files>`** proves a comment or formatting pass changed no *code*, by stripping comments and whitespace and diffing the tokens against a git revision. It says nothing about the built image.
- **Vendored code keeps its upstream style.** `src/lib/` (`u_bstream`/`u_sstream`, BSD-3) is never restyled, so an upstream diff stays readable. *mandate*
- Braces on their own line (Allman); a block that holds a control statement or a declaration keeps them. *default*
- **A single-statement body is written without braces.** One statement, on one line, no comment; anything longer keeps its braces. *default*
- **Comments are brief, and mostly absent.** A comment that narrates *what* is deleted; one survives only for a hardware or protocol quirk the code cannot express — every register write carries its datasheet chapter/section as the clearest exception. Deeper detail belongs in the hardware doc for the silicon (a measurement) or `docs/architecture.md` / `docs/protocol.md` (design). *mandate*
- Declarations at the top of a function (`for`-scoped allowed but not required); one return-statement idiom (early returns for errors); no `goto` except one cleanup jump. *default*
- Errors are explicit status returns (the HAL returns `int`, 1 = ok / 0 = fail; core modules return status enums such as `RADIO_TX_OK`, `SPINEL_STATUS_*`); no exceptions, no `assert()` in production paths — a violated invariant calls `Panic_Enter`. *default*
- Sizes, addresses and bit positions are named constants (`0x17` → `TRX_XAH_CTRL_1`), never raw magic numbers in logic. *mandate*
- **Indentation is 4 spaces.** *mandate*
- **A blank line separates a conditional block** (`if`/`else`/`for`/`while`/ `switch`) from the code above and below it; not where the line above opens a block (`{`) or the line below closes one (`}`). *default*

## 7. Memory

- **There is no libc.** GCC turns a struct assignment, a zero loop or a byte-copy loop into `memcpy`/`memset` calls a `-nostdlib` image cannot call (the link fails with `undefined reference to 'memset'`). Two shapes avoid it, both in use: a `volatile` pointer (the USB HAL's endpoint-bank clear on USB boards, the identity zeroing in `props.c`) or `U_BStream` (the packet copy in `rcp_app.c`). *mandate*
- **No dynamic allocation of any kind** — no `malloc`, no arena, no bump allocator. Every path has a fixed bound; the pools are listed in `docs/architecture.md`. *mandate*
- All buffers are statically sized from named constants; a build-time assertion (a negative-size array, `typedef char name[(cond) ? 1 : -1];`) fails the build if a frame or path exceeds its buffer, and core unit-tests cover the bounds. *mandate*
- Buffer ownership is explicit and single-writer: a pool slot is free, filled by a producer, or in flight. Which producer owns which pool, and the full-pool policy, is stated in the owning module (§4.3) and unit-tested. *mandate*
- Stack usage is bounded; the ISR path must not allocate or call anything that can. *default*

## 8. References (where the rules come from)

- `U_BStream` / `U_SStream`: `git.sr.ht/~cryo/u_bstream`, `git.sr.ht/~cryo/u_sstream` (BSD-3); vendored under `src/lib/`.
- `docs/boards.md`, `docs/architecture.md`, `docs/hardware_samr21.md`, `docs/hardware_mg21.md`, `docs/hardware_avr.md`, `docs/protocol.md`, `docs/testing.md` — the design and behaviour behind these rules.
- `src/lib/PROVENANCE.md` — the vendored libraries and the one fetched library (RAIL), with their licences.
