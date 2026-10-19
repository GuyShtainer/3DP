# Release P4b: the setup screen in the 3DS build, its host test and its emulator gates. Included from tools/port.mk by ONE line, placed right after the
# `DKP :=` line (after HW_OBJS exists, before the hardware-layer rules and `port-hw:` read it). Its emulator gates (emu-setup-*) are kept
# privately (they drive the Azahar emulator) and are not part of the public tree.
# The ROM-check table comes from tools/setup.mk (P4a): build/generated-setup/setup_fs_table.c, generated from $(PLAT_REF), never committed.
SETUP_TABLE_C := build/generated-setup/setup_fs_table.c
HW_OBJS += $(HWOBJ)/hw_setup.o $(HWOBJ)/hw_setup_core.o $(HWOBJ)/hw_cfgio.o $(HWOBJ)/setup_fs_table.o
$(HWOBJ)/hw_setup.o $(HWOBJ)/hw_setup_core.o $(HWOBJ)/hw_cfgio.o: $(HWOBJ)/%.o: $(HWSRC)/%.c
	@mkdir -p $(HWOBJ); $(PCC) $(P_ARCH) $(P_DEFS) -I$(HWSRC) $(HW_STRICT) -O2 -c $< -o $@
$(HWOBJ)/hw_cfgio.o: $(HWSRC)/hw_cfgio.h $(HWSRC)/hw_ctr.h $(HWSRC)/hw_cfg.h $(HWSRC)/hw_log.h
$(HWOBJ)/hw_setup.o: $(HWSRC)/hw_cfgio.h $(HWSRC)/version.h $(HWSRC)/hw_setup.h $(HWSRC)/hw_setup_core.h $(HWSRC)/hw_ctr.h $(HWSRC)/hw_cfg.h
$(HWOBJ)/setup_fs_table.o: $(SETUP_TABLE_C) $(HWSRC)/hw_setup_core.h
	@mkdir -p $(HWOBJ); $(PCC) $(P_ARCH) $(P_DEFS) -I$(HWSRC) $(HW_STRICT) -O2 -c $< -o $@

# host test of hw_setup.c over a fake seam (plain + ASan/UBSan); P4a's `host-tests-setup` is the core's.
.PHONY: host-tests-setup-flow
host-tests-setup-flow:
	tools/host-tests/hw_setup_test.sh
host-tests-public: host-tests-setup-flow
