#!/usr/bin/env python3
"""List the .c files `make port` compiles, mirroring the meson source lists (ARM9 lists; src/ARM7 excluded
except snd's ARM7 build). usage: port_files.py game|glib|ns|sdk|snd7|extra|asm|unlisted"""
import glob, os, re, sys
R = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "port")
SDK = "subprojects/NitroSDK-4.2.30001/libraries"
NS = "subprojects/NitroSystem-071126.1/libraries"
SDK_LIBS = "fx math std mi os fs card snd gx rtc spi pm ctrdg wm cp pxi".split()
NS_LIBS = "g3d g2d snd fnd gfd".split()

def listed(mb):
    """first `lib*_srcs = files(...)` (or *_c / *_srcs) block that is not an arm7 list"""
    txt = open(mb).read()
    out = []
    for name, body in re.findall(r"(\w+)\s*=\s*files\((.*?)\)", txt, re.S):
        if "arm7" in name.lower(): continue
        out += re.findall(r"'([^']+\.c)'", body)
    return out

def rel(p): return os.path.relpath(p, os.path.dirname(R))

def allc(d): return sorted(glob.glob(os.path.join(d, "**/*.c"), recursive=True))

what = sys.argv[1]
res = []
if what == "game":
    # the game's own src/meson.build list, as the real build does (it leaves out src/unk_02095CD4.c, whose header does not exist)
    mb = f"{R}/src/meson.build"
    lst = {os.path.normpath(f"{R}/src/{f}") for f in re.findall(r"'([^']+\.c)'", open(mb).read())}
    res = [p for p in allc(f"{R}/src") if os.path.normpath(p) in lst]
elif what == "glib":
    for l in ("spl", "crypto", "gds"): res += allc(f"{R}/lib/{l}")
elif what in ("ns", "sdk", "unlisted"):
    for base, libs in ((NS, NS_LIBS), (SDK, SDK_LIBS)):
        if what == "ns" and base != NS: continue
        if what == "sdk" and base != SDK: continue
        for l in libs:
            d = f"{R}/{base}/{l}"
            mb = f"{d}/src/meson.build"
            if not os.path.isdir(d): continue
            lst = {os.path.normpath(f"{d}/src/{f}") for f in listed(mb)} if os.path.exists(mb) else None
            everything = [p for p in allc(d)]
            if what == "unlisted":
                res += [p for p in everything if lst is not None and os.path.normpath(p) not in lst]
            elif lst is None:
                res += [p for p in everything if "/ARM7/" not in p]
            else:
                res += [p for p in sorted(lst) if os.path.exists(p)]
elif what == "snd7":
    d = f"{R}/{SDK}/snd/src"
    res = sorted(glob.glob(f"{d}/ARM7/*.c")) + [f"{d}/{n}.c" for n in
        "snd_util snd_main snd_bank snd_work snd_alarm snd_command snd_data".split()]
elif what == "extra":
    # C files the meson lists leave out but whose symbols the game links (the link trial's "decomp functions in unlisted files"):
    # B64Encode lives in the DWC GameSpy base, libraries/gs/nonport.c.
    res = [f"{R}/subprojects/NitroDWC-2.2.30008/libraries/gs/nonport.c"]
elif what == "asm":
    # gTrainerAITable is the label of the trainer-AI bytecode, a macro-assembler DATA file (8108 lines of byte/short macros, no instructions).
    # The real build assembles it with arm-none-eabi-gcc (meson.build `trainer_ai_script.o`); so does `make port-objects`, with the same
    # flags, so the bytes are the decomp's own and no hand-made C table can drift from them.
    res = [f"{R}/src/battle/trainer_ai/script.s"]
for p in res: print(rel(p))
