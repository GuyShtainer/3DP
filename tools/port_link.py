#!/usr/bin/env python3
"""tools/port_link.py -- bet 1.6a `make port-link`: the REAL link of 3DP.elf (-> 3DP.3dsx via 3dsxtool, see tools/port.mk).

Inputs: every object of `make port-objects` (port/obj, minus the ARM7 sound sources, os_tick.c and ctrdg_proc.c which hw_os.c / hw_ctrdg.c replace;
port/obj/arm7_snd.o stays), the hardware layer (`make port-hw`: port/hwobj, incl. port_main.o and the generated overlay_sections.o), libctru, libm.
Linker script: port/hwgen/3dsx_port.ld (tools/port_ld.py: devkitARM 3dsx.ld + the overlay map). Through devkitARM's own driver and specs
(3dsx_crt0, --emit-relocs, --gc-sections): NO -nostartfiles, NO --unresolved-symbols=ignore-all, NO --allow-multiple-definition. Any undefined
or multiply defined symbol fails the link and this tool prints it. After a link it audits the section map (restore ranges, see audit()).
usage: port_link.py [--audit-only]       exit 0 = linked and audited
"""
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
DKP = os.environ.get("DEVKITPRO", "/opt/devkitpro")
ARM = os.environ.get("DEVKITARM", DKP + "/devkitARM")
BIN = ARM + "/bin/arm-none-eabi-"
ELF = os.path.join(ROOT, "3DP.elf")
MAP = os.path.join(ROOT, "port/link.map")
RSP = os.path.join(ROOT, "port/link.rsp")
SPECS = os.path.join(ROOT, "port/hwgen/3dsx_port.specs")
EXCLUDE = ("/arm7/", "/os_tick.c.o", "/ctrdg_proc.c.o")
RESTORE_OUT = re.compile(r"^\.(ovl\d+|main)\.(data|bss|sinit)$")


def objects():
    found = []
    for base in ("port/obj", "port/hwobj"):
        for d, _, files in os.walk(os.path.join(ROOT, base)):
            for f in files:
                p = os.path.join(d, f)
                if f.endswith(".o") and not any(x in p for x in EXCLUDE):
                    found.append(os.path.relpath(p, ROOT))
    return sorted(found)


def link(objs):
    open(RSP, "w").write("\n".join(objs) + "\n")
    cmd = [BIN + "gcc", "-specs=" + SPECS, "-g", "-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft",
           "-Wl,--no-enum-size-warning", "-Wl,--wrap=OS_WaitIrq", "-Wl,-Map," + MAP, "-o", ELF, "@" + RSP, "-L" + DKP + "/libctru/lib", "-lcitro3d", "-lctru", "-lm"]
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)


def digest(stderr):
    undef = sorted(set(re.findall(r"undefined reference to [`']([^'`]+)'", stderr)))
    dups = sorted(set(re.findall(r"multiple definition of [`']([^']+)'", stderr)))
    other = [l for l in stderr.splitlines() if l.strip() and "wchar_t" not in l and "undefined reference" not in l and "multiple definition" not in l
             and "first defined here" not in l and "in function" not in l]
    return undef, dups, other


def symbols(elf):
    out = subprocess.run([BIN + "nm", elf], capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    return syms


def writable_sections(objs):
    """{(object path, input section name): size} of every SHF_WRITE+ALLOC input section with a size, per readelf."""
    res = {}
    for o in objs:
        r = subprocess.run([BIN + "readelf", "-S", "-W", o], cwd=ROOT, capture_output=True, text=True).stdout
        for line in r.splitlines():
            m = re.match(r"^\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+\s+([A-Z]*)\s", line)
            if m and "W" in m.group(4) and "A" in m.group(4) and int(m.group(3), 16) > 0:
                res[(o, m.group(1))] = int(m.group(3), 16)
    return res


def map_inputs(mapfile):
    """[(output section, input section, size, object path)] for every input section placed in the final image."""
    rows, out, pend = [], None, None
    for line in open(mapfile):
        m = re.match(r"^(\.\S+)\s*(0x[0-9a-f]+\s+0x[0-9a-f]+)?", line)
        if m and not line.startswith(" "):
            out = m.group(1)
            continue
        m = re.match(r"^ (\.\S+)\s*$", line)
        if m:
            pend = m.group(1)
            continue
        m = re.match(r"^ (\.\S+)\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S+)", line)
        if m:
            sec, size, path = m.group(1), int(m.group(2), 16), m.group(3)
        else:
            m = re.match(r"^\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S+)", line)
            if not (m and pend):
                continue
            sec, size, path = pend, int(m.group(1), 16), m.group(2)
        pend = None
        if size and out:
            rows.append((out, sec, size, path.split("(")[0]))
    return rows


def audit(objs, elf, mapfile):
    """The restore audit on the real link (review 1.4 / bet 1.6). Returns (problems, stats)."""
    import overlay_map
    problems, stats = [], collections.Counter()
    syms = symbols(elf)

    def need(cond, msg):
        if not cond:
            problems.append(msg)

    for a, b in (("__bss_start__", "__main_bss_start"), ("__main_bss_end", "__ovl_pristine_start"), ("__ovl_pristine_end", "__bss_end__")):
        need(a in syms and b in syms and syms[a] <= syms[b], "%s (%s) must be <= %s (%s)" % (a, hex(syms.get(a, -1)), b, hex(syms.get(b, -1))))
    need(syms.get("__main_bss_start", 0) < syms.get("__ovl_pristine_start", 0), ".main.bss must precede the pristine area")
    need("__main_data_start" in syms and syms["__main_data_start"] < syms.get("__bss_start__", 0), ".main.data must lie below __bss_start__")
    lo, hi = syms.get("__main_data_start", 0), syms.get("__ovl_pristine_end", 0)
    stats["restore_span"] = hi - lo
    # every writable input section of a port object is placed in an output section of the restore ranges, the owner's own
    _, overlays, _, _, owner, _, _ = overlay_map.build(os.path.join(ROOT, "port/obj"))
    ids = {o[1]: i for i, o in enumerate(overlays)}
    want = {"port/obj/port/" + rel: ("main" if who == "main" else "ovl%d" % ids[who]) for rel, who in owner.items()}
    want["port/obj/arm7_snd.o"] = "main"
    wr = writable_sections([o for o in objs if o.startswith("port/obj/")])
    placed = {}
    for out, sec, size, path in map_inputs(mapfile):
        placed.setdefault((path, sec), []).append(out)
        m = RESTORE_OUT.match(out)
        if m and not path.startswith("port/obj/"):
            problems.append("%s %s (%d B) is INSIDE the restore range %s: a soft reset would wipe hardware-layer or library state" % (path, sec, size, out))
        if m and path.startswith("port/obj/"):
            stats["restored_" + m.group(2)] += size
        if not m and path.startswith("port/obj/") and (path, sec) in wr:
            problems.append("%s %s (writable, %d B) is in %s, outside every restore range" % (path, sec, size, out))
    for (path, sec), size in sorted(wr.items()):
        outs = placed.get((path, sec))
        if not outs:
            stats["gc_removed_writable"] += 1          # --gc-sections dropped an unreferenced section: nothing to restore
            continue
        exp = want.get(path)
        for out in outs:
            m = RESTORE_OUT.match(out)
            if exp is None or not m or m.group(1) != exp:
                problems.append("%s %s is in %s, expected .%s.*" % (path, sec, out, exp))
    stats["writable_sections_checked"] = len(wr)
    return problems, stats


def sizes(elf):
    out = subprocess.run([BIN + "size", "-A", elf], capture_output=True, text=True).stdout
    keep = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 2 and p[0] in (".text", ".rodata", ".data", ".bss", ".ovl_pristine", ".tdata", ".tbss") or (len(p) >= 2 and re.match(r"^\.(main|ovl\d+)\.(data|bss|sinit)$", p[0])):
            keep[p[0]] = keep.get(p[0], 0) + int(p[1])
    agg = collections.Counter()
    for k, v in keep.items():
        agg[k if not k.startswith((".main", ".ovl")) or k == ".ovl_pristine" else "restore" + k.rsplit(".", 1)[1]] += v
    return agg


def main():
    objs = objects()
    if "--audit-only" not in sys.argv[1:]:
        r = link(objs)
        undef, dups, other = digest(r.stderr)
        for l in other[:30]:
            print("port-link: " + l[:300])
        if r.returncode != 0 or undef or dups:
            print("port-link: FAILED: %d undefined, %d multiply defined" % (len(undef), len(dups)))
            for s in undef:
                print("  undefined: " + s)
            for s in dups:
                print("  duplicate: " + s)
            return 1
    problems, stats = audit(objs, ELF, MAP)
    sz = sizes(ELF)
    print("port-link: %d objects -> 3DP.elf; sizes: %s" % (len(objs), ", ".join("%s %d" % (k, v) for k, v in sorted(sz.items()))))
    print("port-link: restore audit: %d writable port input sections checked, %d gc-removed, restored .data/.sinit %d B, .bss %d B, span %d B; %d problems" %
          (stats["writable_sections_checked"], stats["gc_removed_writable"], stats["restored_data"] + stats["restored_sinit"], stats["restored_bss"],
           stats["restore_span"], len(problems)))
    for p in problems[:30]:
        print("port-link: RESTORE-RANGE: " + p, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
