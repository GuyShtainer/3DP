#!/bin/bash
# usage: tools/host-tests/setup_core_test.sh      (make host-tests-setup)
# Release P4a: host tests for source/hw/hw_setup_core.c. Generates the ROM-check table from $PLAT_REF (the user's own pret checkout),
# builds the test twice (-O1 -Werror, then ASan+UBSan), runs both. Uses roms/*.nds when present (skips the real-ROM checks otherwise).
set -eu
cd "$(dirname "$0")/../.."
REF=${PLAT_REF:-$(cat .plat_ref 2>/dev/null || echo ../pokeplatinum)}
OUT=build/generated-setup
CC=${HOSTCC:-cc}
ROM=${PLAT_ROM:-$(ls roms/*.nds 2>/dev/null | head -1 || true)}
mkdir -p "$OUT"
python3 -I tools/gen_filesys_table.py --ref "$REF" --out "$OUT/setup_fs_table.c"
#: the shared header text has "*start/*end" inside a block comment and is fixed byte for byte.
WARN="-Wall -Wextra -Werror -Wno-unused-parameter"
SRC="tools/host-tests/setup_core_test.c source/hw/hw_setup_core.c $OUT/setup_fs_table.c"
$CC -O1 $WARN -Isource/hw $SRC -o "$OUT/setup_core_test"
$CC -O1 -g $WARN -fsanitize=address,undefined -fno-sanitize-recover=all -Isource/hw $SRC -o "$OUT/setup_core_test_asan"
# A ROM-less run must say so in the summary line (RF2 L4): a green run without the real-ROM evidence is not a full pass.
LOG=$(mktemp); trap 'rm -f "$LOG"' EXIT
echo "== setup_core_test (plain) =="; "$OUT/setup_core_test" ${ROM:+"$ROM"} | tee -a "$LOG"
echo "== setup_core_test (ASan+UBSan) =="; "$OUT/setup_core_test_asan" ${ROM:+"$ROM"} | tee -a "$LOG"
SK=$(grep -c '^SKIP' "$LOG" || true)
if [ "$SK" -gt 0 ]; then echo "setup_core_test summary: $SK SKIPPED real-ROM checks (no roms/*.nds; the public tree has none) - the synthetic checks passed, the real-ROM evidence is MISSING"
else echo "setup_core_test summary: 0 skipped (real ROM $ROM)"; fi
