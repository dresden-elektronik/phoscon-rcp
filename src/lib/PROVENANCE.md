# Vendored libraries

`src/lib/` holds three small BSD-3 libraries used by the firmware and by the host test harness. They are vendored rather than fetched so the build needs no external dependency tree.

| directory | library | origin |
|---|---|---|
| `u_bstream/` | U_BStream — a binary reader/writer over a caller-owned buffer | `git.sr.ht/~cryo/u_bstream` |
| `u_sstream/` | U_SStream — a bounded string builder with the same contract | `git.sr.ht/~cryo/u_sstream` |
| `aes/` | AES-128 and CCM\* (FIPS-197, IEEE 802.15.4), linked only by a board with Thread support | `phoscon_zbstack`, `src/crypto/`, with `aes_table.c` as the block cipher on a board without an engine; its licence is `aes/LICENSE` and its test suite is `test/unit/test_aes.c`. `aes/zb_status.h` is this tree's, a stand-in for the larger header there |

All three are by this firmware's author, which is why they read like the rest of the tree rather than like an external import. Each source file keeps its BSD-3 notice, which is the license that applies to it.

Two rules come with them:

- **They are never restyled or reformatted** (`docs/coding-rules.md` §6). Keeping the upstream text intact keeps a future update a diff instead of a rewrite.
- **The upstream revision is not pinned for the first two.** When a library is updated, record the commit it came from here, so the next update starts from a known point.

## Fetched at build time: the RAIL library

The ConBee III's `rail` radio back-end (the `rcp` image's default, `docs/build-and-flash.md` §10) links Silicon Labs' RAIL library. Nothing of it is vendored: `tools/fetch_rail.py` downloads it at configure time into a cache outside the tree, and only this tree's adapter (`src/hal/mg21/rail/`) is in `src/`. It is recorded here because it is the one third-party binary a build can link.

| item | value |
|---|---|
| origin | `github.com/SiliconLabs/simplicity_sdk`, tag `v2025.6.3`; the archive object is served by the LFS server that tag's `.lfsconfig` names |
| files | `platform/radio/rail_lib/autogen/librail_release/librail_efr32xg21_gcc_release.a` (SHA-256 `526878162077edd2d9eb60fe917ea1206335559f512c1949ca163014f116773a`), the RAIL headers `common/rail.h`, `common/rail_types.h`, `common/rail_assert_error_codes.h`, `common/rail_features.h`, `chip/efr32/efr32xg2x/rail_chip_specific.h`, `protocol/ieee802154/rail_ieee802154.h`, `platform/common/inc/sl_status.h` and the SDK's `License.txt`; every hash is in `tools/fetch_rail.py` |
| licence | per the SDK's `License.txt`: the default is Silicon Labs' Master Software License Agreement (MSLA), and files under `platform/radio/rail_lib/autogen/librail_release` are distributed under the Zlib licence; the headers carry `SPDX-License-Identifier: Zlib` |
| how it is used | linked into the `rail` rcp image only; the headers reach one translation unit of the adapter. Never restyled, never copied into `src/` or the repository |
