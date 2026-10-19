# Release P4a: the setup screen's ROM-check table + host tests. Included at the end of tools/port.mk, which sets PLAT_REF.
# The table is generated from the user's own pret checkout; it lands under build/ (git-ignored) and is never committed.
SETUP_GEN := build/generated-setup

$(SETUP_GEN)/setup_fs_table.c: tools/gen_filesys_table.py $(PLAT_REF)/platinum.us/filesys.sha1 $(PLAT_REF)/platinum.us/filesys.csv
	python3 -I tools/gen_filesys_table.py --ref "$(PLAT_REF)" --out $@

.PHONY: setup-table host-tests-setup
setup-table: $(SETUP_GEN)/setup_fs_table.c

host-tests-setup:
	tools/host-tests/setup_core_test.sh
host-tests-public: host-tests-setup
