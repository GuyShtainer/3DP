# The public port build (compile the synced pret tree, the hardware layer, link, 3dsx) and the host checks that need only exported files.
# The private half (emulator, oracle, kit, spike) is tools/port-dev.mk, included by the Makefile only when it exists.
#
# `make port [-j8]` : phase-1 bet 1.1 compile census. -fsyntax-only on every game/SDK/NS .c file, one
# target per file, stderr -> port/census/<path>.diag, exit status -> .rc. Never stops at a failure.
# `make port-census-clean` wipes the logs (header edits are not tracked, so wipe before a timed run).
# Run tools/sync_port.sh first.

PCC     := $(or $(DEVKITARM),/opt/devkitpro/devkitARM)/bin/arm-none-eabi-gcc
PORT    := port
PSDK    := $(PORT)/subprojects/NitroSDK-4.2.30001
PNS     := $(PORT)/subprojects/NitroSystem-071126.1
PCEN    := $(PORT)/census

P_ARCH  := -march=armv6k -mtune=mpcore -mfloat-abi=hard -std=gnu99 -fsigned-char -fno-short-enums -fshort-wchar \
           -fno-strict-aliasing -fms-extensions \
           -fwrapv -fno-aggressive-loop-optimizations -fno-delete-null-pointer-checks   # UB-tolerant codegen, see phase-1-toolchain-policy.md. -fno-strict-aliasing: slice 1.2b C bodies pun u16/u32 like the asm did (see phase-1-toolchain-policy.md). mwcc accepts tagged anonymous struct members inside unions (poffin.h)
P_WARN  := -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable
# mirrors meson (-D list of the game's main.nef / NitroSDK nitrosdk_args) minus SDK_CW; plus SDK_GCC, revision 0
P_DEFS  := -DSDK_TS -DSDK_4M -DSDK_ARM9 -DSDK_FINALROM -DSDK_CODE_ARM -DNNS_FINALROM \
           -DSDK_CW_FORCE_EXPORT_SUPPORT -DSP1P3_BUG_FOR_CONDITIONAL_ASM_INSTRUCTIONS \
           -DPOKEPLATINUM_GENERATED_ENUM -D_MSL_RESTRICT= -DSDK_GCC=1 -DPOKEPLATINUM_REVISION=0
P_GAMEDEFS := -D_NITRO -DLINK_PPWLOBBY -DPM_KEEP_ASSERTS -DGAME_VERSION=VERSION_PLATINUM -DGAME_LANGUAGE=LANGUAGE_ENGLISH
P_ARM7DEFS := -USDK_ARM9 -DSDK_ARM7 -DSDK_WIRELESS_IN_VRAM -DSDK_NOINIT
P_RESDIRS := $(addprefix $(PORT)/build/,$(shell cat $(PORT)/res_incdirs.txt 2>/dev/null))
P_INCS  := -I$(PORT)/build -I$(PORT) -I$(PORT)/include -I$(PORT)/asm \
           -I$(PORT)/lib/crypto/include -I$(PORT)/lib/gds/include -I$(PORT)/lib/spl/include -I$(PORT)/lib/ppwlobby/include \
           -I$(PORT)/build/generated \
           -I$(PORT)/build/subprojects/NitroSDK-4.2.30001/gen -I$(PSDK)/gen -I$(PSDK)/include \
           -I$(PORT)/build/subprojects/NitroSDK-4.2.30001/gen/nitro/fx \
           -I$(PNS)/include -I$(PORT)/subprojects/NitroWiFi-2.1.30003/include \
           -I$(PORT)/subprojects/NitroDWC-2.2.30008/include -I$(PORT)/subprojects/NitroDWC-2.2.30008/include/gs \
           -I$(PORT)/subprojects/NitroDWC-2.2.30008/include/base -I$(PORT)/subprojects/libvct-1.3.1/include \
           -Iport-shim/include
P_BASE  := $(P_ARCH) $(P_WARN) $(P_DEFS) $(P_INCS)

# file sets: tools/port_files.py mirrors the meson source lists (ARM9 lists; src/ARM7 and unlisted files excluded)
P_GAME  := $(shell python3 tools/port_files.py game)
P_GLIB  := $(shell python3 tools/port_files.py glib)
P_NS    := $(shell python3 tools/port_files.py ns)
P_SDK   := $(shell python3 tools/port_files.py sdk)
P_SND7  := $(shell python3 tools/port_files.py snd7)
P_EXTRA := $(shell python3 tools/port_files.py extra)
P_ASM   := $(shell python3 tools/port_files.py asm)

P_D_GAME := $(addprefix $(PCEN)/,$(addsuffix .diag,$(P_GAME)))
P_D_GLIB := $(addprefix $(PCEN)/,$(addsuffix .diag,$(P_GLIB)))
P_D_NS   := $(addprefix $(PCEN)/,$(addsuffix .diag,$(P_NS)))
P_D_SDK  := $(addprefix $(PCEN)/,$(addsuffix .diag,$(P_SDK)))
P_D_SND7 := $(addprefix $(PCEN)/arm7/,$(addsuffix .diag,$(P_SND7)))

# One flag set per file group. $(call P_FLAGRULES,<out dir>) instantiates it for a result tree: PCEN (-fsyntax-only census)
# and POBJ (-O2 -c object gate, `make port-objects`). Both trees hold <path>.diag + <path>.rc (+ <path>.o in POBJ).
POBJ    := $(PORT)/obj
define P_FLAGRULES
D_GAME_$(1) := $$(addprefix $(1)/,$$(addsuffix .diag,$$(P_GAME)))
D_GLIB_$(1) := $$(addprefix $(1)/,$$(addsuffix .diag,$$(P_GLIB)))
D_NS_$(1)   := $$(addprefix $(1)/,$$(addsuffix .diag,$$(P_NS)))
D_SDK_$(1)  := $$(addprefix $(1)/,$$(addsuffix .diag,$$(P_SDK)))
D_SND7_$(1) := $$(addprefix $(1)/arm7/,$$(addsuffix .diag,$$(P_SND7)))
D_EXTRA_$(1) := $$(addprefix $(1)/,$$(addsuffix .diag,$$(P_EXTRA) $$(P_ASM)))
$$(D_GAME_$(1)): P_FLAGS = $$(P_BASE) $$(addprefix -I,$$(P_RESDIRS)) $$(P_GAMEDEFS) -include $$(PORT)/include/pch/global_pch.h
$$(D_GLIB_$(1)): P_FLAGS = $$(P_BASE) -D_NITRO -DLINK_PPWLOBBY   # gds links nitrodwc_dep, which passes both
$$(D_NS_$(1)):   P_FLAGS = $$(P_BASE)
$$(addprefix $(1)/,$$(addsuffix .diag,$$(P_EXTRA))): P_FLAGS = $$(P_BASE) -D_NITRO
$$(addprefix $(1)/,$$(addsuffix .diag,$$(P_ASM))): P_FLAGS = $$(P_ARCH) -I$(PORT)/include -I$(PORT)/asm -I$(PORT)/build -x assembler-with-cpp   # exactly the decomp's meson.build include path
$$(D_SDK_$(1)):  P_FLAGS = $$(P_BASE) -include $$(PSDK)/include/pch/nitro_pch.h
# per-library own dirs, as meson adds them (libraries/<lib> and libraries/<lib>/include)
$$(foreach l,fx math std mi os fs card snd gx rtc spi ctrdg wm cp pxi,$$(eval $$(filter $(1)/$$(PSDK)/libraries/$$(l)/%,$$(D_SDK_$(1)) $$(D_SND7_$(1))): P_FLAGS += -I$$(PSDK)/libraries/$$(l) -I$$(PSDK)/libraries/$$(l)/include))
$$(foreach l,g3d g2d snd fnd gfd,$$(eval $$(filter $(1)/$$(PNS)/libraries/$$(l)/%,$$(D_NS_$(1))): P_FLAGS += -I$$(PNS)/libraries/$$(l) -I$$(PNS)/libraries/$$(l)/include))
$$(D_SND7_$(1)): P_FLAGS = $$(P_BASE) $$(P_ARM7DEFS) -include $$(PSDK)/include/pch/nitro_pch.h -I$$(PSDK)/libraries/snd -I$$(PSDK)/libraries/snd/include
ALL_$(1) := $$(D_GAME_$(1)) $$(D_GLIB_$(1)) $$(D_NS_$(1)) $$(D_SDK_$(1)) $$(D_SND7_$(1)) $$(D_EXTRA_$(1))
endef
$(eval $(call P_FLAGRULES,$(PCEN)))
$(eval $(call P_FLAGRULES,$(POBJ)))

$(PCEN)/arm7/%.diag: %
	@mkdir -p $(dir $@); rc=0; $(PCC) $(P_FLAGS) -fsyntax-only -c $< 2> $@ || rc=$$?; echo $$rc > $(@:.diag=.rc)
$(PCEN)/%.diag: %
	@mkdir -p $(dir $@); rc=0; $(PCC) $(P_FLAGS) -fsyntax-only -c $< 2> $@ || rc=$$?; echo $$rc > $(@:.diag=.rc)
$(POBJ)/arm7/%.diag: %
	@mkdir -p $(dir $@); rc=0; $(PCC) $(P_FLAGS) -O2 -MD -MF $(@:.diag=.d) -c $< -o $(@:.diag=.o) 2> $@ || rc=$$?; echo $$rc > $(@:.diag=.rc)
$(POBJ)/%.diag: %
	@mkdir -p $(dir $@); rc=0; $(PCC) $(P_FLAGS) -O2 -MD -MF $(@:.diag=.d) -c $< -o $(@:.diag=.o) 2> $@ || rc=$$?; echo $$rc > $(@:.diag=.rc)

.PHONY: port port-census-clean port-objects port-objects-clean
port: $(ALL_$(PCEN))
	@echo "port: $(words $(ALL_$(PCEN))) files attempted"
port-census-clean:
	@rm -rf $(PCEN)

# Gate (review 1.2): the whole port set compiled for real, -O2 -c, into port/obj/<path>.o (+ .diag/.rc). -fsyntax-only proves
# parsing only; this proves code generation and surfaces the optimiser-only warnings. Never stops at a failure; the target
# then fails if any file's rc != 0 or any error: line exists. Classification of the warnings is done by a private census tool.
# Header edits are not tracked: run port-objects-clean first for a trustworthy run.
port-objects: $(ALL_$(POBJ))
	@bad=$$(grep -L '^0$$' $$(find $(POBJ) -name '*.rc') | wc -l | tr -d ' '); \
	 echo "port-objects: $(words $(ALL_$(POBJ))) files compiled -O2 -c; $$bad with rc != 0"; \
	 [ "$$bad" = 0 ]
port-objects-clean:
	@rm -rf $(POBJ)

# PLAT_REF: where your pret/pokeplatinum checkout is (tools/get_pret.sh makes one). ONE rule, shared with tools/get_pret.sh, tools/sync_port.sh,
# tools/check_edit_text.py and tools/header_manifest.py: the environment; else the FIRST line of the file .plat_ref (git-ignored, yours; an absolute
# path is used as is, a relative one is relative to this directory, where make runs); else ../pokeplatinum. Exported, so every script sees it.
PLAT_REF ?= $(or $(strip $(shell head -n 1 .plat_ref 2>/dev/null | tr -d '\r')),../pokeplatinum)
export PLAT_REF

# Slice 1.2b: host tests (see tools/host-tests/build.sh): the translated SDK functions and the hardware layer, on the build machine's cc.
# host-tests-public needs only exported files. A private tree's tools/port-dev.mk adds host-tests-dev (the dev-only tests) behind it.
.PHONY: host-tests host-tests-public
host-tests: host-tests-public
host-tests-public: no-build-stamp check-art
	tools/host-tests/build.sh

# RF3 G2: the shipped art (icon.png, banner.png, banner.wav) must equal what tools/art/ generates (decoded content, exact chunk lists, no trailing
# bytes), and the check must still detect its planted faults. Also run by host-tests.
.PHONY: check-art
check-art:
	python3 tools/art/check_art.py
	python3 tools/art/check_art.py --self-test

# B9 (release plan): no build date or time in any compiled source, so two clean builds of the same tree are byte-identical. Fails on any hit.
# RF1 F5: also port-edits/ (what the .ed files carry) and, when synced, the pret tree under port/ (src, lib, subprojects); a missing source
# directory or a grep error (exit 2) is a failure, not a pass.
.PHONY: no-build-stamp
no-build-stamp:
	@dirs="source include port-shim port-edits"; \
	 for d in $$dirs; do [ -d "$$d" ] || { echo "no-build-stamp: FAIL (directory $$d is missing)"; exit 1; }; done; \
	 for d in port/src port/lib port/subprojects; do [ -d "$$d" ] && dirs="$$dirs $$d"; done; \
	 grep -rnE '__(DATE|TIME|TIMESTAMP)__' $$dirs; rc=$$?; \
	 if [ $$rc = 0 ]; then echo "no-build-stamp: FAIL (a build date/time macro in the sources)"; exit 1; fi; \
	 if [ $$rc != 1 ]; then echo "no-build-stamp: FAIL (grep failed, exit $$rc)"; exit 1; fi; \
	 echo "no-build-stamp: ok (no __DATE__/__TIME__/__TIMESTAMP__ in $$dirs)"

# Release P1: the scan gate. No 40+ character run of any text port-edits/ carries may appear in the pristine pret tree ($PLAT_REF),
# except the prototype / identifier lines on tools/check_edit_text_allow.txt. Also run by host-tests.
# check-edit-text-mutant is its planted fault: a copy of port-edits/ whose src/main.c.ed also inserts the first 45 characters of a pret
# line (read from $PLAT_REF/src/main.c at run time, so no pret text is tracked) must FAIL the gate.
.PHONY: check-edit-text check-edit-text-mutant
check-edit-text:
	python3 -I tools/check_edit_text.py
check-edit-text-mutant:
	@t=$$(mktemp -d); trap 'rm -rf "$$t"' EXIT; cp -R port-edits "$$t/edits"; \
	 line=$$(sed -e 's/[[:space:]][[:space:]]*/ /g' -e 's/^ //' "$(PLAT_REF)/src/main.c" | awk 'length($$0) >= 45 { print substr($$0, 1, 45); exit }'); \
	 [ $${#line} = 45 ] || { echo "check-edit-text-mutant: no 45-character line in $(PLAT_REF)/src/main.c"; exit 2; }; \
	 printf '@1 insert-before 1\n|%s\n' "$$line" >> "$$t/edits/src/main.c.ed"; \
	 python3 -I tools/check_edit_text.py --edits "$$t/edits" > "$$t/out" 2>&1; rc=$$?; grep -E '^(PRET TEXT|check_edit_text:)' "$$t/out"; \
	 if [ $$rc = 1 ] && grep -q '^PRET TEXT src/main.c.ed' "$$t/out"; then echo "check-edit-text-mutant: the planted 45-character pret line FAILS the gate (rc 1), as it must"; \
	 else echo "check-edit-text-mutant: BROKEN GATE: rc $$rc with a planted pret line"; exit 1; fi

# Bet 2.6c (review 2.6 N7): the evaluation-order census as a host check. Scans every compiled C file of the port (needs port/src: tools/sync_port.sh) and FAILS on any hazard
# that is not on tools/evalorder-allow.tsv (the triaged harmless sites). Also run by host-tests.
.PHONY: host-census-evalorder
host-census-evalorder:
	python3 -I tools/evalorder.py --check

# Bet 1.3: the hardware layer source/hw/*.c, compiled for real with -Werror (golden rule 10). Three flag sets:
#  - SDK-world files (they include NitroSDK/DWC headers): the port flags + the SDK prefix header;
#  - pure files (no SDK, no libctru; also host-tested): the port flags, nothing else;
#  - hw_ctr.c, the only libctru file: devkitPro's include path and NO port-shim/include (the shim's stddef.h would shadow the system one).
# Objects go to port/hwobj/. `make port-hw` fails on any warning or error.
HWSRC     := source/hw
HWOBJ     := $(PORT)/hwobj
HW_SDK    := hw_os hw_sys hw_ctrdg hw_dwc hw_dgt_abi hw_ppwlobby hw_devs hw_world hw_snd7 hw_wm7 hw_wfc_stubs
HW_PURE   := hw_wmq hw_rtc hw_input hw_card hw_ovl hw_boot hw_cfg hw_saveid hw_save hw_savewb hw_dgt hw_log hw_time hw_timer hw_pump hw_ctx hw_cp hw_gx hw_vram hw_census hw_dma hw_pxi hw_pxi_arm7 hw_blz hw_snd gpu2d gpu3d gpu3d_list gpu3d_tex gpu3d_rec hw_screen gpu3d_export gpu3d_edge hw_budget gpu3d_selfcheck hw_bench hw_benchio hw_wide
HW_CTR    := hw_ctr
# bet 2.5b: gpu3d_pica.o (the citro3d file) and its vertex shader object are part of the link now (port_link.py adds -lcitro3d before -lctru)
HW_OBJS   := $(addprefix $(HWOBJ)/,$(addsuffix .o,$(HW_SDK) $(HW_PURE) $(HW_CTR))) $(HWOBJ)/port_main.o $(HWOBJ)/gpu3d_pica.o $(HWOBJ)/gpu3d_pica_shbin.o
HW_STRICT := -Wall -Wextra -Wno-unused-parameter -Werror $(HW_PROF)
# the DS-era SDK headers are not -Wall clean (mwcc pragmas, unparenthesised masks): they come in as system headers, our own files do not
HW_SYSINC := $(subst -I,-isystem ,$(filter-out -Iport-shim/include,$(P_INCS))) -Iport-shim/include
DKP       := $(or $(DEVKITPRO),/opt/devkitpro)
# Release P4b: the setup screen objects (build half; its emulator gates are private).
include tools/setup-gates.mk
$(addprefix $(HWOBJ)/,$(addsuffix .o,$(HW_SDK))): $(HWOBJ)/%.o: $(HWSRC)/%.c
	@mkdir -p $(HWOBJ); $(PCC) $(P_ARCH) $(P_DEFS) $(HW_SYSINC) -D_NITRO -DLINK_PPWLOBBY -include $(PSDK)/include/pch/nitro_pch.h -I$(HWSRC) \
	  -I$(PSDK)/libraries/os/include -I$(PSDK)/libraries/ctrdg/include $(HW_STRICT) -O2 -c $< -o $@
$(addprefix $(HWOBJ)/,$(addsuffix .o,$(HW_PURE))): $(HWOBJ)/%.o: $(HWSRC)/%.c
	@mkdir -p $(HWOBJ); $(PCC) $(P_ARCH) $(P_DEFS) -I$(HWSRC) $(HW_STRICT) -O2 -c $< -o $@
# N8: the version header reaches exactly two objects; the pattern rules above carry no header dependencies, so say it here (a version bump rebuilds hw_devs.o).
$(HWOBJ)/hw_devs.o: $(HWSRC)/version.h
$(HWOBJ)/hw_ctr.o: $(HWSRC)/hw_ctr.c
	@mkdir -p $(HWOBJ); $(PCC) -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -std=gnu11 -D__3DS__ -I$(HWSRC) \
	  -I$(DKP)/libctru/include $(HW_STRICT) -O2 -c $< -o $@
# bet 1.6a: source/port_main.c, main() of the real link: a libctru file (no port-shim include) that sees the hardware layer through its pure headers.
$(HWOBJ)/port_main.o: source/port_main.c $(wildcard $(HWSRC)/*.h)
	@mkdir -p $(HWOBJ); $(PCC) -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -std=gnu11 -D__3DS__ -I$(HWSRC) \
	  -I$(DKP)/libctru/include $(HW_STRICT) -O2 -g -c $< -o $@
# bet 2.5a/2.5b: gpu3d_pica.c is the citro3d file (libctru + citro3d headers), compiled -Werror. Since 2.5b it is IN the link (port/hwobj, with the picasso-assembled vertex
# shader as a data object, symbols gpu3d_pica_shbin / _end / _size); the pica_replay gate app builds the same two files for itself.
$(HWOBJ)/gpu3d_pica.o: $(HWSRC)/gpu3d_pica.c $(HWSRC)/gpu3d_pica.h $(HWSRC)/gpu3d_rec.h $(HWSRC)/gpu3d_list.h $(HWSRC)/gpu3d_tex.h $(HWSRC)/gpu3d_edge.h
	@mkdir -p $(HWOBJ); $(PCC) -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -std=gnu11 -D__3DS__ -I$(HWSRC) \
	  -I$(DKP)/libctru/include $(HW_STRICT) -O2 -c $< -o $@
$(HWOBJ)/gpu3d_pica.shbin: $(HWSRC)/gpu3d_pica.v.pica
	@mkdir -p $(HWOBJ); $(DKP)/tools/bin/picasso -o $@ $<
$(HWOBJ)/gpu3d_pica_shbin.o: $(HWOBJ)/gpu3d_pica.shbin
	@cd $(HWOBJ) && $(DKP)/tools/bin/bin2s gpu3d_pica.shbin > gpu3d_pica_shbin.s && $(PCC) -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -x assembler-with-cpp -c gpu3d_pica_shbin.s -o gpu3d_pica_shbin.o
.PHONY: port-hw port-hw-clean
port-hw: $(HW_OBJS)
	@echo "port-hw: $(words $(HW_OBJS)) hardware-layer files compiled -O2 -Werror (incl. gpu3d_pica.c, the citro3d file, and its shader object)"
port-hw-clean:
	@rm -rf $(HWOBJ)

# Bet 1.4: the overlay map. tools/overlay_map.py reads platinum.us/main.lsf and the compiled objects (`make port-objects`) and writes
# port/hwgen/overlay_map.ld (ids, per-overlay .data/.bss/.sinit output sections, linker symbols) and overlay_sections.c (compiled into port/hwobj).
HWGEN := $(PORT)/hwgen
.PHONY: port-overlay-map
port-overlay-map:
	PATH="$(DEVKITARM)/bin:$$PATH" python3 tools/overlay_map.py
	@mkdir -p $(HWOBJ); $(PCC) $(P_ARCH) $(P_DEFS) -I$(HWSRC) $(HW_STRICT) -O2 -c $(HWGEN)/overlay_sections.c -o $(HWOBJ)/overlay_sections.o

# Bet 1.5: the ARM7 sound driver as one prefixed relocatable object (port/obj/arm7_snd.o). Needs `make port-objects` (it builds the 13 ARM7-flag
# objects in port/obj/arm7/). Mechanism and the symbol audit: tools/arm7_snd.py.
.PHONY: port-arm7-snd
port-arm7-snd:
	python3 tools/arm7_snd.py

# Bet 1.6a: the real link. 3DP.elf from every port object + the hardware layer + libctru through devkitARM's 3dsx specs and a GENERATED
# full linker script (tools/port_ld.py: 3dsx.ld + the overlay map), strict (no
# --unresolved-symbols=ignore-all, no --allow-multiple-definition), then the restore audit (tools/port_link.py) and 3dsxtool/smdhtool.
# `make` (the default) runs this; `make spike` is the Phase-0 spike.
.PHONY: port-link port-ld
port-ld: port-overlay-map
	python3 tools/port_ld.py
port-link: port-objects port-hw port-arm7-snd port-ld
	python3 tools/port_link.py
	$(DKP)/tools/bin/smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" icon.png 3DP.smdh
	python3 tools/elf_strip_abs_relocs.py 3DP.elf port/3dsx_input.elf
	$(DKP)/tools/bin/3dsxtool port/3dsx_input.elf 3DP.3dsx --smdh=3DP.smdh
	@ls -l 3DP.elf 3DP.3dsx | awk '{print "port-link: " $$5 " B " $$9}'

# Release P4a: the setup screen's ROM-check table (generated from $(PLAT_REF), never committed) + its host tests.
include tools/setup.mk
