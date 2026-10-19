#!/usr/bin/env python3
"""tools/elf_strip_abs_relocs.py <in.elf> <out.elf> -- drop the --emit-relocs entries whose symbol is an out-of-image SHN_ABS constant.

3dsxtool turns the ELF's relocation entries into the 3dsx relocation table and aborts ("Relocation to invalid address!") on a relocation against an
ABSOLUTE symbol. The port has such symbols on purpose: the linker-script constants the SDK reads as numbers (SDK_OVERLAY_<name>_ID: the overlay id
is the symbol's address, FS_OVERLAY_ID; SDK_SYS_STACKSIZE, SDK_IRQ_STACKSIZE, SDK_SECTION_ARENA_*, _binary_*). ld has already resolved every such use
to the constant, so the relocation entry carries no information the loader may act on (adding the load base to a number would corrupt it).
Removing exactly those entries is therefore the correct 3dsx input. Only SHT_REL sections of the ELF are rewritten, in place and compacted
(sh_size shrinks, the file size does not change). Prints the count per category and per symbol.

Review 1.6 (F3): only ABS symbols whose VALUE lies outside the image [__start__, __end__] are constants. An ABS symbol whose value is inside the
image (`__end__`) is an address that 3dsxtool relocates correctly, and dropping it would bake a link-time address into a build that is loaded at
another base (HBL / .cia). Those are kept. Every dropped relocation's symbol must belong to a known category (SDK_OVERLAY_<name>_ID,
SDK_<name>_STACKSIZE, _binary_<name>_size): anything else aborts the build, so a new kind of absolute symbol is a decision, not a silent drop.
"""
import collections
import re
import struct
import sys

SHT_REL, SHT_SYMTAB, SHN_ABS = 9, 2, 0xFFF1
CATEGORIES = [("overlay id", re.compile(r"^SDK_OVERLAY_\w+_ID$")), ("stack size", re.compile(r"^SDK_\w*STACKSIZE$")),
              ("binary size", re.compile(r"^_binary_\w+_size$"))]


def category(name):
    for label, rx in CATEGORIES:
        if rx.match(name):
            return label
    return None


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = bytearray(open(src, "rb").read())
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        sys.exit("not a little-endian ELF32")
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
    secs = []
    for i in range(shnum):
        n, ty, fl, ad, off, sz, lk, inf, al, es = struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize)
        secs.append(dict(i=i, type=ty, off=off, size=sz, link=lk, info=inf, entsize=es, hdr=shoff + i * shentsize))
    removed = collections.Counter()
    kept_abs = collections.Counter()
    total = 0
    lo = hi = None
    for s in secs:
        if s["type"] == SHT_SYMTAB:
            strtab = secs[s["link"]]
            for k in range(s["size"] // 16):
                nameoff, value = struct.unpack_from("<II", d, s["off"] + k * 16)
                e = d.index(b"\0", strtab["off"] + nameoff)
                nm = d[strtab["off"] + nameoff:e]
                if nm == b"__start__":
                    lo = value
                elif nm == b"__end__":
                    hi = value
    if lo is None or hi is None or lo >= hi:
        sys.exit("elf_strip_abs_relocs: __start__ / __end__ not found in the symbol table")
    for s in secs:
        if s["type"] != SHT_REL:
            continue
        sym = secs[s["link"]]
        assert sym["type"] == SHT_SYMTAB
        kept = bytearray()
        for o in range(s["off"], s["off"] + s["size"], 8):
            r_off, r_info = struct.unpack_from("<II", d, o)
            si = r_info >> 8
            shndx = struct.unpack_from("<H", d, sym["off"] + si * 16 + 14)[0] if si else 0
            if shndx == SHN_ABS:
                nameoff, value = struct.unpack_from("<II", d, sym["off"] + si * 16)
                strtab = secs[sym["link"]]
                e = d.index(b"\0", strtab["off"] + nameoff)
                nm = d[strtab["off"] + nameoff:e].decode()
                if lo <= value <= hi:
                    kept_abs[nm] += 1   # an address inside the image: 3dsxtool relocates it
                else:
                    if category(nm) is None:
                        sys.exit("elf_strip_abs_relocs: dropped relocation against ABS symbol %s (value 0x%x) is in no known category" % (nm, value))
                    removed[nm] += 1
                    continue
            kept += d[o:o + 8]
            total += 1
        d[s["off"]:s["off"] + len(kept)] = kept
        struct.pack_into("<I", d, s["hdr"] + 20, len(kept))
    open(dst, "wb").write(d)
    cats = collections.Counter()
    for nm, n in removed.items():
        cats[category(nm)] += n
    print("elf_strip_abs_relocs: %d relocations kept (%d of them against in-image ABS symbols: %s), %d against %d out-of-image ABS constants dropped"
          " [%s] (image 0x%x-0x%x; top: %s)" %
          (total, sum(kept_abs.values()), ", ".join("%s x%d" % kv for kv in kept_abs.items()) or "none", sum(removed.values()), len(removed),
           ", ".join("%s x%d" % kv for kv in sorted(cats.items())), lo, hi, ", ".join("%s x%d" % kv for kv in removed.most_common(3))))


if __name__ == "__main__":
    main()
