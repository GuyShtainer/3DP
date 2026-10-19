#!/usr/bin/env python3
"""tools/overlay_map.py -- bet 1.4: the overlay map of the port, generated from the decomp's linker spec (port/platinum.us/main.lsf).

The DS loads 122 overlays into one RAM window; on the 3DS everything is linked once and an overlay "load" restores that overlay's
.data from a pristine copy and zeroes its .bss. This script turns main.lsf into the three things that make that work:

  port/hwgen/overlay_map.ld       a linker-script fragment (`ld -T`, INSERT BEFORE .data / .bss):
                                    SDK_OVERLAY_<name>_ID = <index>;           (the ids FS_LoadOverlay is called with)
                                    .ovl<N>.data / .ovl<N>.bss / .ovl<N>.sinit   one output section per overlay, gathering exactly the
                                                                                 objects main.lsf puts in that overlay
                                    .main.data / .main.bss / .main.sinit        every other game+SDK object (Static main)
                                    __ovl<N>_{data,bss,sinit}_{start,end}, __main_{data,bss,sinit}_{start,end}
                                    .ovl_pristine (NOLOAD)                      room for one pristine copy of every .data above
                                    _binary_* and SDK_SECTION_ARENA_* / SDK_SYS_STACKSIZE   the linker-supplied symbols
                                  Objects of the hardware layer (port/hwobj), libctru and newlib match no pattern, so they stay in the
                                  ordinary .data/.bss OUTSIDE every restore range: a soft reset restores the game, never the hw layer.
  port/hwgen/overlay_sections.c   g_hw_ovl_syms[122] (the section symbols above) + the id/name table; compiled into port/hwobj.
  stdout                          the mapping report and the exceptions.

Cross-check (fails with exit 1 on a surprise): every port object of the game is listed in main.lsf or in EXCEPTIONS below, every object
main.lsf names exists in the port or in MISSING_OK, no object is listed twice, and the overlays whose ROM sinit list is non-empty are
exactly the overlays that contain an object with a .sinit table (the 34 that carry NitroStaticInit, see the private design notes).

usage: overlay_map.py [--check] [--objs port/obj] [--out port/hwgen]      (--check: map + checks only, writes nothing)
"""
import collections
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LSF = os.path.join(ROOT, "port/platinum.us/main.lsf")
ROM = os.path.join(ROOT, "roms/platinum-us.nds")
OBJ = os.path.join(ROOT, "port/obj")
OUT = os.path.join(ROOT, "port/hwgen")

# (port object path under port/obj/port/, overlay name, why): port objects the lsf does not name 1:1.
EXCEPTIONS = {
    "lib/crypto/src/rc4_encrypt.c.o": ("main_menu", "C stand-in for asm/rc4.s (libcrypto_ov97.a, overlay main_menu) and asm/rc4_fast.s (static): stateless, so it rides with the main_menu crypto"),
    "subprojects/NitroDWC-2.2.30008/libraries/gs/nonport.c.o": ("nintendo_wfc", "B64Encode, in libdwcgscommon.a (nintendo_wfc); added by `port_files.py extra`"),
}
# lsf objects with no port object: (reason)
MISSING_OK = {
    "src_unk_02095CD4.c.o": "the decomp's own meson build leaves src/unk_02095CD4.c out (its header does not exist)",
    "trainer_ai_script.o": "assembled data, port/obj/port/src/battle/trainer_ai/script.s.o (matched by name below)",
}
# libs of main.lsf -> the directories of port objects that belong to them (everything else of a library is not in the port)
LIB_DIRS = {
    "libspl.a": ["lib/spl/src/"],
    "libgds.a": ["lib/gds/src/"],
    "libcrypto_static.a": ["lib/crypto/src/rc4_init.c.o"],
    "libcrypto_ov97.a": ["lib/crypto/src/sign.c.o", "lib/crypto/src/bignum.c.o", "lib/crypto/src/util.c.o"],
}
SDK_PREFIXES = ("subprojects/NitroSDK-", "subprojects/NitroSystem-")
DWC_PREFIXES = ("subprojects/NitroDWC-",)
# NitroSDK/NitroSystem libraries that main.lsf names inside an OVERLAY block (the rest of the SDK is Static main): lib -> the directory prefixes of their
# port objects. Such an object belongs to that overlay (its statics must be reloaded with it), never to Static main. libmb.a is in wfc_settings
# (main.lsf:737); the port compiles none of it today, and the moment it does these objects follow the overlay.
SDK_OVERLAY_LIBS = {
    "libmb.a": ["subprojects/NitroSDK-4.2.30001/libraries/mb/"],
}
# Libraries named in overlay blocks that are not NitroSDK/NitroSystem and have no port objects (NitroDWC / NitroWiFi / the game's own): the port
# stubs the whole WFC stack (hw_dwc.c, hw_wfc_stubs.c). A port object that does appear under NitroDWC must have an EXCEPTIONS entry.
NO_PORT_OBJECT_LIBS_PREFIX = ("libdwc", "libcps", "libsoc", "libssl", "libvct", "libwcm", "libstubsmd5")


def parse_lsf(path):
    """-> list of (kind, name, objects[], libraries[]) in file order; kind is 'Static' or 'Overlay'."""
    blocks, cur = [], None
    for raw in open(path):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.match(r"^(Static|Overlay)\s+(\S+)$", line)
        if m:
            cur = (m.group(1), m.group(2), [], [])
            continue
        if line == "{":
            continue
        if line == "}":
            if cur:
                blocks.append(cur)
            cur = None
            continue
        m = re.match(r"^Object\s+(\S+)", line)
        if m and cur:
            cur[2].append(m.group(1))
            continue
        m = re.match(r"^Library\s+(\S+)\s*(\(.*\))?", line)
        if m and cur and not m.group(2):
            cur[3].append(m.group(1))
    return blocks


def port_objects(objdir):
    """-> {path relative to port/obj/port/ : full path} of every compiled port object (ARM9)."""
    base = os.path.join(objdir, "port")
    found = {}
    for d, _, files in os.walk(base):
        for f in files:
            if f.endswith(".o"):
                p = os.path.join(d, f)
                found[os.path.relpath(p, base)] = p
    return found


def obj_key(lsf_obj):
    """main.nef.p/src_overlay005_fieldmap.c.o -> 'src_overlay005_fieldmap.c.o'"""
    return lsf_obj.split("/", 1)[1] if "/" in lsf_obj else lsf_obj


def has_sinit(path):
    r = subprocess.run(["arm-none-eabi-objdump", "-h", path], capture_output=True, text=True)
    return ".sinit" in r.stdout


def rom_sinit_overlays(rom):
    """ids of the overlays whose ROM table entry has a non-empty sinit list (ARM9 overlay table, 32 bytes/entry)."""
    f = open(rom, "rb")
    hdr = f.read(0x60)
    off, size = struct.unpack_from("<II", hdr, 0x50)
    f.seek(off)
    tab = f.read(size)
    out = []
    for i in range(size // 32):
        id_, ram, rsz, bss, sinit_s, sinit_e, fid = struct.unpack_from("<7I", tab, i * 32)
        if sinit_e - sinit_s > 4:   # 4 bytes = the lone 0 terminator; 8 = [fn, 0]
            out.append(id_)
    return sorted(out)


def build(objdir):
    blocks = parse_lsf(LSF)
    overlays = [b for b in blocks if b[0] == "Overlay"]
    static = [b for b in blocks if b[0] == "Static"][0]
    objs = port_objects(objdir)
    # lsf object name -> port path; the port path is src/<dir>/<name>.c.o with '/' written '_' in the lsf name
    by_key = {}
    for rel in objs:
        if rel.startswith("src/"):
            by_key["src_" + rel[4:].replace("/", "_")] = rel
    errors, notes = [], []
    owner = {}     # port rel path -> overlay name ('main' for static)

    def assign(rel, who, how):
        if rel in owner and owner[rel] != who:
            errors.append("%s is claimed by both %s and %s" % (rel, owner[rel], who))
        owner[rel] = who

    for kind, name, olist, libs in [static] + overlays:
        who = "main" if kind == "Static" else name
        for o in olist:
            key = obj_key(o)
            if key in by_key:
                assign(by_key[key], who, "object")
            elif key == "trainer_ai_script.o":
                assign("src/battle/trainer_ai/script.s.o", who, "object")
            elif key in MISSING_OK:
                notes.append("lsf object without a port object (%s): %s [%s]" % (who, key, MISSING_OK[key]))
            else:
                errors.append("lsf object %s (%s) has no port object" % (o, who))
        for lib in libs:
            if kind == "Overlay" and lib not in LIB_DIRS and lib not in SDK_OVERLAY_LIBS and not lib.startswith(NO_PORT_OBJECT_LIBS_PREFIX):
                errors.append("overlay %s names library %s, which overlay_map.py has no rule for (add it to LIB_DIRS, SDK_OVERLAY_LIBS or the no-object list)" % (who, lib))
            for pat in SDK_OVERLAY_LIBS.get(lib, []) if kind == "Overlay" else []:
                hit = [r for r in objs if r.startswith(pat)]
                for r in hit:
                    assign(r, who, "library " + lib)
                notes.append("library %s (overlay %s): %d port objects" % (lib, who, len(hit)))
            for pat in LIB_DIRS.get(lib, []):
                hit = [r for r in objs if r.startswith(pat) or r == pat]
                if not hit:
                    errors.append("library %s (%s): nothing under %s" % (lib, who, pat))
                for r in hit:
                    assign(r, who, "library " + lib)
    known = {o[1] for o in overlays}
    for rel, (who, why) in EXCEPTIONS.items():
        if who not in known:
            errors.append("EXCEPTIONS entry %s names an unknown overlay %s" % (rel, who))
        if rel not in objs:
            errors.append("EXCEPTIONS entry %s is not a port object any more" % rel)
        else:
            assign(rel, who, "exception")
            notes.append("exception: %s -> %s (%s)" % (rel, who, why))
    # everything else under src/ must not exist: a game object that no overlay and the static block both ignore is a mapping hole
    for rel in sorted(objs):
        if rel in owner:
            continue
        if rel.startswith("src/"):
            errors.append("game object %s is in no main.lsf block" % rel)
        elif rel.startswith(DWC_PREFIXES):
            errors.append("NitroDWC object %s is compiled but main.lsf puts the DWC libraries in overlays: add an EXCEPTIONS entry naming its overlay" % rel)
        elif rel.startswith(SDK_PREFIXES):
            owner[rel] = "main"   # NitroSDK / NitroSystem libraries: libos.a ... are Static main libraries in main.lsf
        else:
            errors.append("port object %s is in no main.lsf block" % rel)
    return blocks, overlays, static, objs, owner, errors, notes


def ident(n):
    return re.sub(r"[^A-Za-z0-9_]", "_", n)


def emit_ld(overlays, objs, owner):
    per = collections.defaultdict(list)
    for rel, who in sorted(owner.items()):
        per[who].append(rel)
    L = ["/* GENERATED by tools/overlay_map.py from main.lsf -- do not edit. Used as `ld -T overlay_map.ld` next to the default script. */", ""]
    for i, o in enumerate(overlays):
        L.append("SDK_OVERLAY_%s_ID = %d;" % (o[1], i))
    L += ["",
          "/* linker-supplied symbols the SDK expects (os_arena.c, os_thread.c, os_china.c). The 3DS has no DS memory map: the values only need",
          "   to be distinct and aligned; hw_sys.c sets the real arenas up. */",
          "SDK_SYS_STACKSIZE = 0;",
          "/* SDK_SECTION_ARENA_{DTCM,ITCM,EX}_START and SDK_IRQ_STACKSIZE are defined in source/hw/hw_sys.c: the arena starts are WRITTEN through (the launcher",
          "   thread's stack-limit words), so they must lie inside the host arrays, not at the DS addresses */"]
    for b in ("notes_forChina_chrData", "notes_forChina_scrData", "digit_chrData", "logo_forChina_chrData", "logo_forChina_scrData", "logo_forChina_palData"):
        L.append("_binary_%s_bin = 0; _binary_%s_bin_end = 0;" % (b, b))

    def fragment(kinds, where):
        out = ["", "SECTIONS", "{"]
        for i, o in enumerate(overlays):
            for kind in kinds:
                out.append("    .ovl%d.%s : ALIGN(16) {" % (i, kind))
                out.append("        __ovl%d_%s_start = .;" % (i, kind))
                for r in per[o[1]]:
                    out.append("        " + INPUT[kind] % ("*port/obj/port/" + r))
                out += ["        . = ALIGN(16);", "        __ovl%d_%s_end = .;" % (i, kind), "    }"]
        # Static main: every other object under port/obj (the hardware layer, libctru and newlib do not match, so they stay outside)
        for kind in kinds:
            out.append("    .main.%s : ALIGN(16) {" % kind)
            out.append("        __main_%s_start = .;" % kind)
            out.append("        " + INPUT[kind] % "*port/obj/*")
            out += ["        . = ALIGN(16);", "        __main_%s_end = .;" % kind, "    }"]
        out += ["}", "INSERT BEFORE %s;" % where]
        return out

    L += fragment(["data", "sinit"], ".data")
    L += fragment(["bss"], ".bss")
    total = " + ".join(["SIZEOF(.main.data)"] + ["SIZEOF(.ovl%d.data)" % i for i in range(len(overlays))])
    L += ["", "/* one pristine copy of every .data above, filled once at init (hw_ovl_init) */", "SECTIONS", "{",
          "    .ovl_pristine (NOLOAD) : ALIGN(16) {", "        __ovl_pristine_start = .;", "        . = . + (%s);" % total,
          "        . = ALIGN(16);", "        __ovl_pristine_end = .;", "    }", "}", "INSERT AFTER .bss;", ""]
    return "\n".join(L)


INPUT = {"data": "%s(.data .data.*)", "bss": "%s(.bss .bss.* COMMON)", "sinit": "KEEP(%s(.sinit))"}


def emit_c(overlays):
    n = len(overlays)
    C = ["/* GENERATED by tools/overlay_map.py from main.lsf -- do not edit. The overlay section symbols the linker script defines. */",
         '#include "hw_ovl.h"', ""]
    for i in range(n):
        for k in ("data", "bss", "sinit"):
            C.append("extern char __ovl%d_%s_start[], __ovl%d_%s_end[];" % (i, k, i, k))
    for k in ("data", "bss", "sinit"):
        C.append("extern char __main_%s_start[], __main_%s_end[];" % (k, k))
    C += ["extern char __ovl_pristine_start[], __ovl_pristine_end[];", "",
          "const HwOvlSyms g_hw_ovl_syms[HW_OVL_COUNT] = {"]
    for i, o in enumerate(overlays):
        C.append("    { __ovl%d_data_start, __ovl%d_data_end, __ovl%d_bss_start, __ovl%d_bss_end, (void (**)(void))__ovl%d_sinit_start, (void (**)(void))__ovl%d_sinit_end, \"%s\" }," % ((i,) * 6 + (o[1],)))
    C += ["};", "",
          "const HwOvlSyms g_hw_main_syms = { __main_data_start, __main_data_end, __main_bss_start, __main_bss_end,",
          "    (void (**)(void))__main_sinit_start, (void (**)(void))__main_sinit_end, \"main\" };",
          "char *const g_hw_ovl_pristine_start = __ovl_pristine_start;", "char *const g_hw_ovl_pristine_end = __ovl_pristine_end;", ""]
    return "\n".join(C)


def main():
    check_only = "--check" in sys.argv
    objdir = OBJ
    out = OUT
    if "--objs" in sys.argv:
        objdir = sys.argv[sys.argv.index("--objs") + 1]
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]
    blocks, overlays, static, objs, owner, errors, notes = build(objdir)
    if len(overlays) != 122:
        errors.append("main.lsf has %d overlays, expected 122" % len(overlays))
    # the sinit cross-check: overlays that contain a .sinit object vs the ROM's non-empty lists
    sinit_ov = collections.defaultdict(int)
    for rel, who in owner.items():
        if rel.startswith("src/") and has_sinit(objs[rel]):
            sinit_ov[who] += 1
    ids = {o[1]: i for i, o in enumerate(overlays)}
    mine = sorted(ids[w] for w in sinit_ov if w in ids)
    if "main" in sinit_ov:
        errors.append("Static main contains %d object(s) with a .sinit table (unsupported: nothing runs them at boot)" % sinit_ov["main"])
    if os.path.exists(ROM):
        rom = rom_sinit_overlays(ROM)
        if rom != mine:
            errors.append("sinit overlays differ: objects say %s, the ROM table says %s" % (mine, rom))
        else:
            notes.append("sinit: %d overlays carry a NitroStaticInit table, identical to the ROM overlay table's non-empty sinit lists" % len(rom))
    else:
        notes.append("sinit: ROM absent, %d overlays carry a NitroStaticInit table (not cross-checked against the ROM table)" % len(mine))
    per = collections.Counter(owner.values())
    print("overlay_map: %d overlays, %d port objects mapped (%d in Static main, %d in overlays)" %
          (len(overlays), len(owner), per["main"], len(owner) - per["main"]))
    for n in notes:
        print("overlay_map: " + n)
    for e in errors:
        print("overlay_map: ERROR " + e, file=sys.stderr)
    if errors:
        return 1
    if not check_only:
        os.makedirs(out, exist_ok=True)
        open(os.path.join(out, "overlay_map.ld"), "w").write(emit_ld(overlays, objs, owner))
        open(os.path.join(out, "overlay_sections.c"), "w").write(emit_c(overlays))
        print("overlay_map: wrote %s/overlay_map.ld and overlay_sections.c" % os.path.relpath(out, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
