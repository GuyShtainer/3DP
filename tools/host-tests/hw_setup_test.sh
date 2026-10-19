#!/bin/bash
# usage: tools/host-tests/hw_setup_test.sh      (make host-tests-setup-flow)
# Release P4b: hw_setup.c over a fake hw_ctr seam (a temp directory), one process per case; plain and ASan+UBSan. The ROM cases use roms/*.nds (SKIP without).
set -eu
cd "$(dirname "$0")/../.."
OUT=build/generated-setup
CC=${HOSTCC:-cc}
ROM=${PLAT_ROM:-$(ls roms/*.nds 2>/dev/null | head -1 || true)}
python3 -I tools/gen_filesys_table.py --ref "${PLAT_REF:-$(cat .plat_ref 2>/dev/null || echo ../pokeplatinum)}" --out "$OUT/setup_fs_table.c"
WARN="-Wall -Wextra -Werror -Wno-unused-parameter"
SRC="tools/host-tests/hw_setup_test.c source/hw/hw_setup.c source/hw/hw_setup_core.c source/hw/hw_cfgio.c source/hw/hw_cfg.c source/hw/hw_input.c source/hw/hw_log.c $OUT/setup_fs_table.c"
$CC -O1 $WARN -Isource/hw $SRC -o "$OUT/hw_setup_test"
$CC -O1 -g $WARN -fsanitize=address,undefined -fno-sanitize-recover=all -Isource/hw $SRC -o "$OUT/hw_setup_test_asan"
LOG=$(mktemp); trap 'rm -f "$LOG"' EXIT
for bin in hw_setup_test hw_setup_test_asan; do
  echo "== $bin =="
  for c in resolve notneeded-rom notneeded-off notneeded-bench import new wrongrom select home split cfgkeys pathrt writerom caps longname folders queue badname importfail importfail2 s1keep s1confirm s1restore s2badrom s3odd cfgio homecheck utf8 cjk spaces; do
    "$OUT/$bin" "$c" ${ROM:+"$ROM"} | tee -a "$LOG"
  done
done
# A ROM-less run must say so in the summary line (RF2 L4): a green run without the real-ROM evidence is not a full pass.
SK=$(grep -c '^SKIP' "$LOG" || true)
CASES=$(grep -c '^hw_setup_test ' "$LOG" || true)
if [ "$SK" -gt 0 ]; then echo "hw_setup_test summary: $CASES case runs, $SK SKIPPED (no roms/*.nds: the ROM cases need the player's own dump, the public tree has none) - the ROM evidence is MISSING"
else echo "hw_setup_test summary: $CASES case runs, 0 skipped (real ROM $ROM)"; fi
