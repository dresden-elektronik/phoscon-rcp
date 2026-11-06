# Contributing

Phoscon RCP is open source, not open contribution, the model SQLite follows: pull requests are not merged. The maintainers make every change, because testing one takes the full set of boards and the board keys. A pull request is closed without review.

**The way to contribute is an issue:** report a problem in the firmware, or request a feature.

## 1. Opening an issue

Use [GitHub issues](https://github.com/dresden-elektronik/phoscon-rcp/issues). A problem in a host or application belongs to that project.

| for | include |
|---|---|
| a problem | the board, the release its `NCP_VERSION` reports, the host and its version, what happened and what you expected, and a log or capture if you have one |
| a feature | what you are trying to do, and which board or host it is for |

The licence lets anyone fork the firmware and change it. The rest of this page is what the maintainers work by, and it serves a fork equally: what a change must keep intact, how to check it, and where each kind of knowledge is written down.

## 2. Rules for a change

| rule | why | full text |
|---|---|---|
| C99, freestanding, no libc, no dynamic allocation; `<stdint.h>` is the only standard header on target | the images run on bare metal with statically sized pools | `docs/coding-rules.md` |
| no `bool`; booleans are `int` 0/1 | one convention across four toolchains | `docs/coding-rules.md` |
| assembly only for the four core instructions (`HAL_Wfi`, `HAL_IrqLock`, `HAL_IrqUnlock`, `HAL_Dsb`) | the rest stays portable C | `docs/coding-rules.md` |
| binary I/O through `U_BStream`, strings through `U_SStream` | bounds-checked, one place to get endianness right | `docs/coding-rules.md` |
| naming: public `UpperCamel`, static `lowerCamel`, constants `UPPER_SNAKE` | readable at a glance which symbols cross a module boundary | `docs/coding-rules.md` |
| core modules never call `HAL_*`/`Radio_*` or touch a register; time arrives as `nowMs` | the core runs unchanged in the host suites | `docs/coding-rules.md` |
| a core module ships with a host suite | it is tested without hardware | `docs/coding-rules.md` |
| no image writes the deCONZ NVRAM or the bootloader, and every container passes `tools/gcf_check.py` before a flash | a stick must always be able to go back to the vendor firmware | `docs/build-and-flash.md` |
| a hardware fact cites its datasheet chapter, says when it comes from vendor source or a measurement, and is not stated as fact until verified on a board | non-obvious register code rests on that citation | — |

Every new source file starts with the licence header the others carry: `SPDX-License-Identifier: BSD-3-Clause` and the dresden elektronik copyright line, in the file's comment syntax. Vendored code in `src/lib/` is kept as received, with its own notices (`src/lib/PROVENANCE.md`); do not restyle it.

## 3. Checking a change

```sh
# host suites: plain C99, no target toolchain, no hardware
cmake -B build/test -S . && cmake --build build/test
ctest --test-dir build/test --output-on-failure

# every board still links (repeat per board, see docs/build-and-flash.md §2)
cmake -B build/fw -S src -G Ninja -DDEBOARD=cb2 -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake
cmake --build build/fw
```

**Never pass `-DCMAKE_BUILD_TYPE=Release`.** The toolchain files set the optimisation and flags each image is sized and tested with.

A change that touches a board's radio or link should also pass the hardware rigs of `docs/testing.md`, and a new board target the audits of `docs/testing.md` before its first flash.

## 4. Documentation

Each subject has one owning doc; a finding goes into that doc, in the same commit as the code it describes. `AGENTS.md` lists the doc set.

| rule | |
|---|---|
| one paragraph or list item per line, never hard-wrapped | `python3 tools/unwrap_md.py --check README.md CONTRIBUTING.md AGENTS.md docs/*.md` |
| keep it short: write what a reader needs, not what the code already says; prefer a generated figure to complex prose; point to a document rather than a section unless the section matters | |
| source comments cite `docs/x.md §N`, so a renumbering rewrites every citation in the same commit | `python3 tools/docrefs.py` checks every citation resolves |
| state what is true, in the present tense; no history, no dates, no `file:line` | the doc describes the tree as it is; git keeps the history |
| tables for facts that compare, prose for reasoning | |

### 4.1 Figures

The figures under `docs/images/` are generated, never drawn by hand: `tools/diagrams/<name>.py` writes `docs/images/<name>.png`, so a change to a figure shows up in a diff. A figure is rebuilt in the commit that changes what it shows.

```sh
python3 tools/diagrams/all.py            # rebuild every figure
python3 tools/diagrams/all.py --check    # is every committed figure current?
python3 tools/diagrams/layers.py         # one figure
```

- **They check the tree before they draw.** Every path, function, ring and state a figure names must exist in `src/`, every label must fit its box (measured in Chrome), and the layer figure also checks the call direction it draws. A stale figure cannot be built.
- **Style** is the Phoscon design language's light tokens (`tools/diagrams/phoscon.css`): amber marks the one subject of a figure, solid arrows are calls, dashed arrows answers, and every figure has a legend. The README's opener (`tools/diagrams/opener.py`) sets its own colours and faces.
- **Captions live in the Markdown**, never in the image, and a figure carries no dates or versions.
- Building needs Python 3 and Chrome; the output is deterministic, which is what `--check` compares.

## 5. Commits

Small commits with one concern each; a doc change goes in with the code it describes. Run the host suites and the doc lints before you commit.
