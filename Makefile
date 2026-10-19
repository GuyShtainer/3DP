# 3DP -- native Pokemon Platinum port to the New 3DS. See BUILDING.md.
#   make           the port: tools/port.mk (compile the pret tree you synced, link 3DP.elf, 3DP.3dsx)
#   make cia       the installable 3DP.cia (needs makerom + bannertool: tools/bin/ or PATH)
#   make host-tests, make check-edit-text, make no-build-stamp, make clean
# A private tree may add tools/port-dev.mk (the emulator, oracle and kit targets); it is included only when present.
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)

APP_TITLE       := 3DP
APP_DESCRIPTION := Gen-4 overworld, widescreen + stereo 3D
APP_AUTHOR      := Guy Shtainer

#---------------------------------------------------------------------------------
# CIA packaging. makerom + bannertool are found in this repo's tools/bin/ first, then on PATH
# (see BUILDING.md). DEV_MAKEROM / DEV_BANNERTOOL are extra candidates a private tools/port-dev.mk may set.
# The New 3DS flags (804MHz / L2 / core 2 access) live in $(RSF).
#---------------------------------------------------------------------------------
MAKEROM          ?= $(firstword $(wildcard $(CURDIR)/tools/bin/makerom) $(DEV_MAKEROM) makerom)
BANNERTOOL       ?= $(firstword $(wildcard $(CURDIR)/tools/bin/bannertool) $(DEV_BANNERTOOL) bannertool)

RSF              := app.rsf
BANNER_IMAGE     := banner.png
BANNER_AUDIO     := banner.wav

APP_PRODUCT_CODE    := CTR-P-3DPA
APP_UNIQUE_ID       := 0xF3D4E
APP_SYSTEM_MODE     := 64MB
APP_SYSTEM_MODE_EXT := 124MB
APP_SAVE_SIZE       := 0K

.DEFAULT_GOAL := port-link

# `make cia` packages the port (3DP.elf + 3DP.smdh from `make` / port-link) with the New 3DS exheader of $(RSF).
# No gate runs it: the .cia is built only when it is about to be installed.
PORT_OUT := 3DP
# N8: the title version comes from the committed source/hw/version.h (one place), passed to makerom.
APP_VER_FILE := source/hw/version.h
APP_VER_MAJOR := $(shell sed -n 's/^\#define APP_VERSION_MAJOR[[:space:]]*\([0-9]*\).*/\1/p' $(APP_VER_FILE))
APP_VER_MINOR := $(shell sed -n 's/^\#define APP_VERSION_MINOR[[:space:]]*\([0-9]*\).*/\1/p' $(APP_VER_FILE))
APP_VER_MICRO := $(shell sed -n 's/^\#define APP_VERSION_MICRO[[:space:]]*\([0-9]*\).*/\1/p' $(APP_VER_FILE))
CIA_VER = -major $(APP_VER_MAJOR) -minor $(APP_VER_MINOR) -micro $(APP_VER_MICRO)
CIA_DEFS = -DAPP_TITLE="$(APP_TITLE)" \
		-DAPP_PRODUCT_CODE="$(APP_PRODUCT_CODE)" \
		-DAPP_UNIQUE_ID=$(APP_UNIQUE_ID) \
		-DAPP_SYSTEM_MODE=$(APP_SYSTEM_MODE) \
		-DAPP_SYSTEM_MODE_EXT=$(APP_SYSTEM_MODE_EXT) \
		-DAPP_SAVE_SIZE=$(APP_SAVE_SIZE)

.PHONY: cia clean

cia: port-link banner.bnr
	@$(MAKEROM) -f cia -o $(PORT_OUT).cia -target t -exefslogo \
		-elf $(PORT_OUT).elf -rsf $(RSF) -icon $(PORT_OUT).smdh -banner banner.bnr $(CIA_VER) $(CIA_DEFS)
	@echo built ... $(PORT_OUT).cia

banner.bnr: $(BANNER_IMAGE) $(BANNER_AUDIO)
	@$(BANNERTOOL) makebanner -i $(BANNER_IMAGE) -a $(BANNER_AUDIO) -o banner.bnr

# clean: the port's outputs and its compiled objects. It keeps the synced pret tree (port/ itself) so a rebuild needs no re-sync.
clean:
	@echo clean ...
	@rm -fr $(PORT_OUT).elf $(PORT_OUT).3dsx $(PORT_OUT).smdh $(PORT_OUT).cia banner.bnr port/obj port/hwobj port/hwgen port/census port/arm7snd port/host-tests port/3dsx_input.elf

include $(CURDIR)/tools/port.mk
-include $(CURDIR)/tools/port-dev.mk
