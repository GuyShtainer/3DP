#!/bin/bash
# usage: tools/host-tests/build.sh
# The PUBLIC host tests (`make host-tests`): they need only the exported files and the synced port/ tree. Compiles the translated
# NitroSDK functions on the build machine (not the 3DS toolchain) and runs tools/host-tests/asm_to_c_test.c against them, then the
# hardware layer's pure modules. Output lands in port/host-tests/ (git-ignored). Needs `tools/sync_port.sh` first.
# The tests that need the emulator tooling, the oracle, the player's ROM or saves are kept in the private dev tree.
#
# Platforms. macOS (Apple clang + ld64) is the platform these tests were written and run on. Linux is UNTESTED: the two link steps that need
# "dead-strip unused sections, tolerate unresolved symbols" pick the GNU ld equivalents (--gc-sections, --unresolved-symbols=ignore-all) by
# `uname -s`, and any other system stops with a message. If a Linux link or test fails, that is a bug in this script, not in the port: please report it.
set -eu
cd "$(dirname "$0")/../.."
case "$(uname -s)" in
  Darwin) LDFLAGS_X="-Wl,-dead_strip -Wl,-undefined,dynamic_lookup" ;;
  Linux)  LDFLAGS_X="-Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all" ;;
  *) echo "host-tests: unsupported system $(uname -s) (macOS and Linux only; Linux untested)" >&2; exit 2 ;;
esac
SDK=port/subprojects/NitroSDK-4.2.30001
GEN=port/build/subprojects/NitroSDK-4.2.30001/gen
OUT=port/host-tests
SH=$OUT/shadow
CC=${HOSTCC:-cc}
[ -d "$SDK/libraries" ] || { echo "run tools/sync_port.sh first"; exit 2; }
rm -rf "$OUT"; mkdir -p "$SH/nitro/hw/ARM9" "$OUT/obj"

# Shadow headers. The SDK was written for a 32-bit ABI (u32 = unsigned long, DS addresses as integers); the
# host is 64-bit, so: u32/s32 become int (the memory map is redirected below).
sed -e 's/typedef unsigned long u32;/typedef unsigned int u32;/' \
    -e 's/typedef signed long s32;/typedef signed int s32;/' $SDK/include/nitro/types.h > $SH/nitro/types.h
# The patched mmap_global.h (port-edits/, nitrosdk memory map) redirects every DS region to a g_hw* array; HW_ARRAY_ is the cast
# to an address. On the 3DS it is (u32); here u32 is 32 bits wide on a 64-bit host, so the shadow defines it as uintptr_t first (the
# header keeps its own definition behind #ifndef). The test defines the arrays (source/hw/hw_sys.c does on the 3DS).
{ echo '#include <stdint.h>'; echo '#define HW_ARRAY_(a) ((uintptr_t)(a))'; cat $SDK/include/nitro/hw/ARM9/mmap_global.h; } > $SH/nitro/hw/ARM9/mmap_global.h
grep -q '^#ifndef HW_ARRAY_' $SH/nitro/hw/ARM9/mmap_global.h && grep -q 'define HW_IOREG HW_ARRAY_(&g_hwregs)' $SH/nitro/hw/ARM9/mmap_global.h
# gxasm.c / g3x.c include gxasm.h relative to their own directory, so they are compiled from a copy beside a shadow
# copy of the header (since slice 1.2c the patched header has no non-static GX_SendFifo36B declaration; the sed is a no-op guard).
mkdir -p $OUT/gx_src $OUT/include
sed '/^void GX_SendFifo36B/d' $SDK/libraries/gx/include/gxasm.h > $OUT/include/gxasm.h
cp $SDK/libraries/gx/src/gxasm.c $SDK/libraries/gx/src/g3x.c $OUT/gx_src/
printf '#include <nitro/types.h>\n#include <nitro/math/qsort.h>\n#include <nitro/os.h>\n#include <nitro/mi.h>\n' > $SH/nitro.h

DEFS="-DSDK_GCC=1 -DSDK_ARM9 -DSDK_TS -DSDK_FINALROM"
INC="-I$OUT/include -I$SH -Iport-shim/include -I$SDK/include -I$SDK/gen -I$GEN -Iport/lib/crypto/include"
LIBS="fx:fx_mtx22 fx:fx_mtx33 fx:fx_mtx43 fx:fx_mtx44 cp:cp_context gx:gxasm gx:g3x math:qsort \
mi:mi_memory mi:mi_swap mi:mi_uncompress mi:mi_uncomp_stream os:os_system os:os_tcm os:os_context \
os:os_protectionRegion os:os_protectionUnit os:os_vramExclusive os:os_cache os:os_spinLock os:os_thread"
OBJS=""
for e in $LIBS; do
  lib=${e%%:*}; f=${e##*:}; srcdir=$SDK/libraries/$lib/src
  [ "$lib" = gx ] && srcdir=$OUT/gx_src
  $CC -O1 -w -include $SH/nitro.h $DEFS $INC -I$SDK/libraries/$lib -I$SDK/libraries/$lib/include -c $srcdir/$f.c -o $OUT/obj/$f.o
  OBJS="$OBJS $OUT/obj/$f.o"
done
for f in rc4_init rc4_encrypt; do
  $CC -O1 -w $DEFS $INC -c port/lib/crypto/src/$f.c -o $OUT/obj/$f.o; OBJS="$OBJS $OUT/obj/$f.o"
done
# cp_context.c (patched, nitrosdk-asm-to-c.patch) restarts the divider / sqrt unit through HWDEV_Cp*Run: the tests link the pure unit.
$CC -O1 -w -Isource/hw -c source/hw/hw_cp.c -o $OUT/obj/hw_cp.o; OBJS="$OBJS $OUT/obj/hw_cp.o"
# g3x.c / gxasm.c / memory.h route their GX FIFO and command-port stores through HWG3_Port / HWG3_Words (nitrosdk-g3-ports.patch): the pure unit.
$CC -O1 -w -Isource/hw -c source/hw/hw_gx.c -o $OUT/obj/hw_gx.o; OBJS="$OBJS $OUT/obj/hw_gx.o"
$CC -O1 -w -Isource/hw -Iport-shim/include -c source/hw/hw_vram.c -o $OUT/obj/hw_vram.o; OBJS="$OBJS $OUT/obj/hw_vram.o"   # MIi_Cpu* call HWVRAM_Touch (bet 2.1b)
# The test TU is -Wall -Wextra -Werror; the SDK headers are DS-era code, so they come in as system headers.
SYSINC="-isystem $OUT/include -isystem $SH -isystem port-shim/include -isystem $SDK/include -isystem $SDK/gen -isystem $GEN -isystem port/lib/crypto/include -isystem $SDK/libraries/mi -isystem $SDK/libraries/os/include"
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter $DEFS $SYSINC -Isource/hw -c tools/host-tests/asm_to_c_test.c -o $OUT/obj/test.o
$CC $OUT/obj/test.o $OBJS $LDFLAGS_X -o $OUT/asm_to_c_test
$OUT/asm_to_c_test
# Review 1.3 BLOCKER 2 at SDK level: the real pxi_fifo.c / pm.c / os_interrupt.c over the loopback ARM7 (hw_devs.c + hw_pxi*.c).
HW=source/hw
SH2=$OUT/shadow2; mkdir -p $SH2; cp -R $SH/nitro $SH2/nitro      # the same shadow headers plus pxi.h in the shadow <nitro.h> (the real one pulls in wfs, whose 64-bit layout asserts)
cp $SH/nitro.h $SH2/nitro.h; printf '#include <nitro/pxi.h>\n#include <nitro/spi/common/type.h>\n#include <nitro/rtc.h>\n' >> $SH2/nitro.h
INC2="-I$OUT/include -I$SH2 -Iport-shim/include -I$SDK/include -I$SDK/gen -I$GEN -Iport/lib/crypto/include"
OBJS2=""
for e in pxi:pxi_fifo pxi:pxi_init spi:pm spi:tp rtc:external rtc:internal rtc:convert os:os_interrupt os:os_irqTable os:os_mutex os:os_thread os:os_context os:os_system os:os_spinLock os:os_cache os:os_tcm \
         os:os_protectionRegion os:os_protectionUnit os:os_vramExclusive; do
  lib=${e%%:*}; f=${e##*:}; o=$OUT/obj/p2_$f.o
  $CC -O1 -w -include $SH2/nitro.h $DEFS -DGX_LCD_SIZE_X=256 -DGX_LCD_SIZE_Y=192 $INC2 -I$SDK/libraries/$lib -I$SDK/libraries/$lib/include -c $SDK/libraries/$lib/src/$f.c -o $o
  OBJS2="$OBJS2 $o"
done
for f in hw_devs hw_rtc hw_input hw_card hw_log hw_pxi hw_pxi_arm7 hw_snd hw_dma hw_cp hw_gx hw_vram hw_census hw_cfg hw_dgt hw_time hw_timer hw_pump hw_ctx hw_budget; do
  $CC -O1 -w -include $SH2/nitro.h $DEFS $INC2 -I$HW -c $HW/$f.c -o $OUT/obj/p2_$f.o; OBJS2="$OBJS2 $OUT/obj/p2_$f.o"
done
$CC -O1 -Wall -Wextra -Werror -I$HW -c tools/host-tests/hw_savewb_stub.c -o $OUT/obj/p2_hw_savewb_stub.o; OBJS2="$OBJS2 $OUT/obj/p2_hw_savewb_stub.o"   # bet 2.6b: the write-back glue needs libctru; no-ops here
SYSINC2="-isystem $OUT/include -isystem $SH2 -isystem port-shim/include -isystem $SDK/include -isystem $SDK/gen -isystem $GEN -isystem port/lib/crypto/include -isystem $SDK/libraries/mi -isystem $SDK/libraries/os/include"
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter $DEFS $SYSINC2 -include $SH2/nitro.h -I$HW -c tools/host-tests/pxi_sdk_test.c -o $OUT/obj/pxi_sdk_test.o
$CC $OUT/obj/pxi_sdk_test.o $OBJS2 -pthread $LDFLAGS_X -o $OUT/pxi_sdk_test
$OUT/pxi_sdk_test
# Bet 1.3: the pure modules of source/hw (time, timers, pump, thread hand-off, log, config, MD5/SHA-1/HMAC); no SDK, no libctru.
HW=source/hw
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -pthread -I$HW tools/host-tests/hw_os_test.c $HW/hw_time.c $HW/hw_timer.c $HW/hw_pump.c \
  $HW/hw_ctx.c $HW/hw_log.c $HW/hw_cfg.c $HW/hw_dgt.c $HW/hw_cp.c $HW/hw_dma.c $HW/hw_pxi.c $HW/hw_pxi_arm7.c $HW/hw_snd.c $HW/hw_blz.c $HW/hw_card.c $HW/hw_rtc.c $HW/hw_input.c $HW/hw_wmq.c \
  -o $OUT/hw_os_test
$OUT/hw_os_test
# Bet 2.2a: the frame-driven virtual clock (hw_time.c + hw_pump.c) over a fake host.
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/hw_clock_test.c $HW/hw_time.c $HW/hw_timer.c $HW/hw_pump.c -o $OUT/hw_clock_test
$OUT/hw_clock_test
# Bet 1.4 gates B and C: overlay data/bss restore over fake linker ranges, and a fake NitroMain run twice through the soft-reset path.
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -pthread -I$HW tools/host-tests/ovl_boot_test.c $HW/hw_ovl.c $HW/hw_boot.c $HW/hw_ctx.c -o $OUT/ovl_boot_test
$OUT/ovl_boot_test
# Bet 1.4 gate A, host half (review 1.4): hw_card.c alone over an in-memory ROM fixture with fault injection; native, one thread, -Werror.
# The SDK half (the real fs + card sources reading the real ROM, on the ARM11) is `make emu-card-fs`. There is no Rosetta/x86_64 leg any more.
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/hw_card_test.c $HW/hw_card.c -o $OUT/hw_card_test
$OUT/hw_card_test
# Bet 2.6b: the backup write-back state machine (hw_save.c): lock/unlock/debounce/cancel/wrap, pure C.
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/hw_save_test.c $HW/hw_save.c -o $OUT/hw_save_test
$OUT/hw_save_test
# Bet 1.5, host half: the ARM7 sound driver's mailbox + cadence (hw_snd.c) over a fake driver and the real loopback ARM7. The real driver over the
# real hardware layer is the private emulator gate `emu-snd-init` (on the ARM11 under Azahar).
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/hw_snd_test.c $HW/hw_snd.c $HW/hw_pxi.c $HW/hw_pxi_arm7.c $HW/hw_rtc.c $HW/hw_input.c -o $OUT/hw_snd_test
$OUT/hw_snd_test
# Bet 2.1a: the GX input layer (hw_gx.c) alone: four paths, program order, the canary, the bounded trace window, the gxtrace config key.
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW -Iport-shim/include tools/host-tests/hw_gx_test.c $HW/hw_gx.c $HW/hw_cfg.c -o $OUT/hw_gx_test
python3 -I tools/host-tests/plat_ref_test.py   # release RF3 B1: the one PLAT_REF rule (absolute / relative .plat_ref, env first, the inside-repo guard)
python3 -I tools/host-tests/header_manifest_test.py   # release RF3: the manifest ignores the path-derived include guards, still fails on a changed line
python3 -I tools/host-tests/port_edits_test.py   # release P1: apply_edits / mkedit round trip over every port-edits .ed, refusals, semantics, fuzz
python3 -I tools/host-tests/check_edit_text_test.py   # release P1: the scan gate on a synthetic tree (planted 45-char line fails, 39 passes, allow / stale)
SCAN=$(python3 -I tools/check_edit_text.py) || { echo "$SCAN"; echo "check_edit_text: FAILED"; exit 1; }   # release P1: the scan gate itself over port-edits/
echo "$SCAN" | tail -1
python3 -I tools/host-tests/gxtrace_test.py   # bet 2.2a: tools/gxtrace.py on hand-built v2 files (structure + matrix-reload rule)
# Bet 2.5a: the polygon-list reader / writer (gpu3d_list.c): round trip, refusals, translucent rule, viewport + depth functions against hand-computed values and, when the
# oracle captures exist, against every vertex of the real --poly-dump files. Writes the synthetic list that the private oracle tests read back with an independent reader.
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/gpu3d_list_test.c $HW/gpu3d_list.c -o $OUT/gpu3d_list_test
$OUT/gpu3d_list_test $OUT build/oracle
# Bet 2.5b: the frame record's 3D half (gpu3d_rec.c): the reader's expanded limit, record isolation after an engine mutation, VRAM changes after the plan, the mirror rules.
# The engine side (gpu3d_rec_fixture.c) sees gpu3d.h, the test sees gpu3d_list.h: two translation units.
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW -Itools/host-tests tools/host-tests/gpu3d_rec_test.c tools/host-tests/gpu3d_rec_fixture.c $HW/gpu3d_rec.c $HW/gpu3d_list.c $HW/gpu3d_tex.c $HW/gpu3d.c $HW/gpu3d_export.c -o $OUT/gpu3d_rec_test
$OUT/gpu3d_rec_test
# Bet 2.5a: DS texture formats to the PICA tiled RGBA8 layout (gpu3d_tex.c)
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/gpu3d_tex_test.c $HW/gpu3d_tex.c -o $OUT/gpu3d_tex_test
$OUT/gpu3d_tex_test
# Bet 2.5c: edge marking, the stencil carrier + same-ID rule (a model of the stencil unit), the Morton de-tile and the depth reconstruction (gpu3d_edge.c)
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/gpu3d_edge_test.c $HW/gpu3d_edge.c $HW/gpu3d_list.c -o $OUT/gpu3d_edge_test
$OUT/gpu3d_edge_test
# Bet 2.7a: the hwcheck frames (gpu3d_selfcheck.c) against a software model of the readback: OK, STALE, LAYOUT
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/gpu3d_selfcheck_test.c $HW/gpu3d_selfcheck.c $HW/gpu3d_edge.c $HW/gpu3d_list.c $HW/gpu3d_tex.c -o $OUT/gpu3d_selfcheck_test
$OUT/gpu3d_selfcheck_test
$OUT/hw_gx_test
# Bet 2.7a: the budget histogram (avg / p95 / max per bucket, hw_budget.c)
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/hw_budget_test.c $HW/hw_budget.c -lm -o $OUT/hw_budget_test
$OUT/hw_budget_test
# Bet 2.7a: the bench.txt parser, the counter, SHA-256 (hw_bench.c) and its file glue over a temp directory (hw_benchio.c)
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/hw_bench_test.c $HW/hw_bench.c -o $OUT/hw_bench_test
$OUT/hw_bench_test
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-result -I$HW tools/host-tests/hw_benchio_test.c $HW/hw_benchio.c $HW/hw_bench.c $HW/hw_cfg.c -o $OUT/hw_benchio_test
$OUT/hw_benchio_test "$PWD/$OUT"
# Bet 2.6a: the console identity in a save (hw_saveid.c) + the per-save config.ini section (hw_cfg.c) on synthetic saves; then (private tree) an independent Python reader against it on synthetic and real saves.
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/hw_saveid_test.c $HW/hw_saveid.c $HW/hw_cfg.c -o $OUT/hw_saveid_test
$OUT/hw_saveid_test
# Bet 2.6c N7: the evaluation-order census over the synced port/src (skips with a notice when port/ is absent); fails on a hazard that is not on tools/evalorder-allow.tsv.
python3 -I tools/evalorder.py --check
# Bet 2.1b: the VRAM bank model (hw_vram.c) with the SDK's own VRAMCNT values (the enums of gx_vramcnt.c and the SHIFT macros of ioreg_GX.h are extracted
# into a generated header) and a real DMA (hw_dma.c) for the dirty bitmap.
mkdir -p $OUT/vram
{ grep -E '^#define REG_GX_VRAMCNT_[A-I]_(E|OFS|MST)_SHIFT ' $SDK/include/nitro/hw/ARM9/ioreg_GX.h
  sed -n '/^typedef enum/,/^} GX_VRAMCNT_I;/p' $SDK/libraries/gx/src/gx_vramcnt.c; } > $OUT/vram/vramcnt_sdk.h
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW -Iport-shim/include -I$OUT/vram tools/host-tests/hw_vram_test.c $HW/hw_vram.c $HW/hw_dma.c -o $OUT/hw_vram_test
$OUT/hw_vram_test
# Bet 2.3a: the per-line 2D record (gpu2d.c) and the pump's HBlank dispatch (hw_pump.c) over a fake host, plus the real VRAM model for the view-follows-VRAMCNT case.
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW -Iport-shim/include tools/host-tests/gpu2d_test.c $HW/gpu2d.c $HW/hw_pump.c $HW/hw_time.c $HW/hw_timer.c $HW/hw_vram.c -o $OUT/gpu2d_test
# Bet 2.3b: the 2D renderer (gpu2d_render) over hand-built frames with hand-computed pixels; no VRAM model, no oracle files.
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/gpu2d_render_test.c $HW/gpu2d.c -o $OUT/gpu2d_render_test
$OUT/gpu2d_render_test
$OUT/gpu2d_test
# Bet 2.4a: the geometry engine on the CPU (gpu3d.c) alone, on hand-built streams with hand-computed 20.12 expectations.
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/gpu3d_test.c $HW/gpu3d.c -o $OUT/gpu3d_test
# Slice 2.4c: the cuts that changed code paths (FPU divide, clip verdict, lighting memo) against what they replaced.
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function -I$HW tools/host-tests/gpu3d_cuts_test.c -o $OUT/gpu3d_cuts_test
$OUT/gpu3d_cuts_test
$OUT/gpu3d_test
# Bet 2.8: the widescreen / stereo formulas and the engine's wide path (DS-matrix substitution, wide planes, guard band, wide map).
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/hw_wide_test.c $HW/gpu3d.c $HW/gpu3d_list.c $HW/hw_wide.c $HW/gpu2d.c $HW/hw_screen.c -o $OUT/hw_wide_test
$OUT/hw_wide_test
# Slice 2.4d (review B1): the clipper never returns more than 10 vertices. The same test twice: plain, and with -fsanitize=bounds,undefined (native clang; never run under Rosetta: a
# sanitizer stop there leaves unkillable processes), whose "runtime error" lines fail the script (recovering mode, so the process exits normally).
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/gpu3d_clip_test.c $HW/gpu3d.c -o $OUT/gpu3d_clip_test
$OUT/gpu3d_clip_test
$CC -O1 -g -fsanitize=bounds,undefined -fsanitize-recover=all -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/gpu3d_clip_test.c $HW/gpu3d.c -o $OUT/gpu3d_clip_test_san
SAN=$($OUT/gpu3d_clip_test_san 2>&1)
echo "$SAN"
if echo "$SAN" | grep -q "runtime error"; then echo "gpu3d_clip_test_san: SANITIZER REPORT"; exit 1; fi
echo "gpu3d_clip_test_san: clean (-fsanitize=bounds,undefined)"
# Bet 2.3c: the screens' pure half (hw_screen.c): present/centre mapping into a host framebuffer, the fbdump file round trip, the frame hand-off (overrun counter), the console owner, the fbdump config key.
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW tools/host-tests/hw_screen_test.c $HW/hw_screen.c $HW/gpu2d.c $HW/hw_cfg.c -o $OUT/hw_screen_test
$OUT/hw_screen_test

# Slice 2.4c: the PLY1 export (gpu3d_export.c) read back through the bet-2.5 reader (gpu3d_list.h / gpu3d_list.c). The reader's header names the same
# types as gpu3d.h, so the test and the fixture are separate translation units.
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW -Itools/host-tests tools/host-tests/gpu3d_export_test.c tools/host-tests/gpu3d_export_fixture.c \
  $HW/gpu3d.c $HW/gpu3d_export.c $HW/gpu3d_list.c -o $OUT/gpu3d_export_test
$OUT/gpu3d_export_test
# Bet 2.1c: the runtime feature census (hw_census.c) alone: every decoded field built by hand from the SDK shifts, the packed G3 stream, frame close.
mkdir -p $OUT/census
NH=$SDK/include/nitro
{ grep -hE '^#define REG_(GX|G2|G3|G3X)_[A-Z0-9_]+_SHIFT +[0-9]+' $NH/hw/ARM9/ioreg_GX.h $NH/hw/ARM9/ioreg_G2.h $NH/hw/ARM9/ioreg_G3.h $NH/hw/ARM9/ioreg_G3X.h
  grep -hE '^#define GX_OAM_ATTR01_[A-Z]+_SHIFT +[0-9]+' $NH/gx/g2_oam.h
  grep -hE '^#define G3OP_[A-Za-z0-9_]+ +(0x[0-9A-Fa-f]+|[0-9]+)$' $NH/gx/g3.h; } > $OUT/census/census_sdk.h
$CC -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$HW -I$OUT/census tools/host-tests/hw_census_test.c $HW/hw_census.c -o $OUT/hw_census_test
$OUT/hw_census_test
