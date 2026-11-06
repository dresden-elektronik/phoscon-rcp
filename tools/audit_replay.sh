#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# audit_replay.sh - regenerate the inputs a review of the tree is written against.
#
# It re-runs the three register/vector records and the host suites, and prints the built-image facts a
# review cites (section placement, vectors, sizes, witness decoders, toolchain flags). The records need
# the vendor's SDK trees, which are not part of this repository: set SDK, HDRS4, CMSIS and SVD.
#
# It changes nothing in the tree. Builds go into build/, which is ignored; the records are compared
# against their checked-in copies and reported, never silently rewritten.
set -e
cd "$(dirname "$0")/.."

SDK=${SDK:?set SDK to a Simplicity SDK tree}
echo "== records: regenerate and compare with the checked-in copies =="
for pair in tools/mg21_regs_gold.py:test/data/mg21_regs_ref.h \
            tools/mg21_radio_gold.py:test/data/mg21_radio_ref.h \
            tools/mg21_vectors_gold.py:test/data/mg21_vectors.json; do
    gen=${pair%%:*}; out=${pair#*:}
    rm -f /tmp/audit-replay-prev
    cp "$out" /tmp/audit-replay-prev
    SDK="$SDK" python3 "$gen" >/dev/null
    if [ -f /tmp/audit-replay-prev ] && cmp -s /tmp/audit-replay-prev "$out"; then
        echo "   $out unchanged"
    else
        cp /tmp/audit-replay-prev "$out" 2>/dev/null || true
        echo "   $out CHANGED - restored the checked-in copy; re-read the audits that cite it"
    fi
done

echo
echo "== host suites =="
cmake -B build/test -S . >/dev/null
cmake --build build/test >/dev/null
ctest --test-dir build/test 2>&1 | grep -E "tests passed|tests failed"

echo
echo "== built images: sizes the docs quote =="
for b in cb2 rb2 cb3 cb3rcp; do
    f=build/fw-$b/phoscon-rcp.bin
    [ -f "$f" ] && printf "   %-7s %8s B\n" "$b" "$(wc -c < "$f")" || printf "   %-7s (not built)\n" "$b"
done

echo
echo "== the cb3 image, as the notes cite it =="
ELF=build/fw-cb3/phoscon-rcp
if [ -f "$ELF" ]; then
    arm-none-eabi-readelf -S "$ELF" | grep -E "\.isr_vector|\.text|\.data|\.bss|\.noinit" | sed 's/^/   /'
    arm-none-eabi-nm -n "$ELF" | grep -E "_estack|_sdata|_edata|_sbss|_ebss|reset_record|noinit" | sed 's/^/   /'
    arm-none-eabi-objdump -d -j .isr_vector "$ELF" 2>/dev/null | head -4 | tail -1 | sed 's/^/   /'
else
    echo "   build/fw-cb3 missing - configure with -DDEBOARD=cb3 and cmake/arm-none-eabi-m33.cmake"
fi

echo
echo "== the witness decoders the audits judge =="
grep -n "MG21" tools/dbg.py | sed 's/^/   /' | head -20

echo
echo "== the two toolchains, flag by flag =="
for f in cmake/arm-none-eabi.cmake cmake/arm-none-eabi-m33.cmake; do
    echo "   $f"
    grep -E "mcpu|march|mfpu|float-abi|nostdib|nostdlib|Linker|LinkerOptions|-O|flags" "$f" | sed 's/^/     /'
done

echo
echo "== doc lints =="
python3 tools/unwrap_md.py --check README.md CONTRIBUTING.md AGENTS.md docs/*.md | grep -v "already unwrapped" || true
python3 tools/docrefs.py
