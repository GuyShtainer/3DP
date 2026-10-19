#!/usr/bin/env python3
"""tools/arm7_snd.py -- `make port-arm7-snd`: the ARM7 sound driver as ONE relocatable object, port/obj/arm7_snd.o.

  1. `ld -r` every ARM7-flag object of port/obj/arm7/ (the 7 shared snd_*.c built with SDK_ARM7 plus the 6 src/ARM7/*.c, i.e. what
     `tools/port_files.py snd7` lists; `make port-objects` builds them) into port/arm7snd/arm7_snd_raw.o.
  2. Take the symbols the merged object DEFINES (global, weak) and write an objcopy --redefine-syms file mapping each X to arm7_X. A symbol the
     object only REFERENCES is untouched, so its calls into the SDK (OS_*, MI_*, g_hwregs ...) still resolve to the ARM9-side implementations:
     --prefix-symbols would also have renamed those into dangling arm7_OS_* names.
  3. More entries of the same file redirect references the ARM7 makes into the ARM9-side PXI / BIOS / PMIC / alarm system to the hardware layer
     (source/hw/hw_snd7.c): the ARM7's PXI_SetFifoRecvCallback would overwrite the ARM9's own callback table entry and its PXI_SendWordByFifo is
     the reply path, so they become hwsnd_Arm7SetFifoRecvCallback / hwsnd_Arm7SendWord; SVC_{Set,Reset}SoundBias and PMi_{Set,Reset}Control
     (ARM7-only services nothing in the ARM9 set defines) become hwsnd_Svc*/hwsnd_Pm*; OS_{Create,Set,SetPeriodic,Cancel}Alarm and OS_GetTick become
     the ARM7's private alarm list (hwsnd_Arm7*Alarm, hwsnd_Arm7GetTick: review 1.5 F2; the ARM9 alarm queue is not the ARM7's).
  4. objcopy --redefine-syms, then audit: no defined symbol without the prefix, no remaining undefined reference of the object that is also
     DEFINED by the object itself, the objects match tools/port_files.py snd7 BY NAME, and every undefined reference is in the classified
     allowlist UNDEF_OK below (anything new fails: it would otherwise link silently against an ARM9 symbol of the same name).
Usage: arm7_snd.py [--arm DIR]   (DEVKITARM from the environment otherwise). Exit 1 on an audit failure.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREFIX = "arm7_"
REDIRECT = {
    "PXI_SetFifoRecvCallback": "hwsnd_Arm7SetFifoRecvCallback",
    "PXI_SendWordByFifo": "hwsnd_Arm7SendWord",
    "SVC_SetSoundBias": "hwsnd_SvcSetSoundBias",
    "SVC_ResetSoundBias": "hwsnd_SvcResetSoundBias",
    "PMi_SetControl": "hwsnd_PmSetControl",
    "PMi_ResetControl": "hwsnd_PmResetControl",
    "OS_CreateAlarm": "hwsnd_Arm7CreateAlarm",
    "OS_SetAlarm": "hwsnd_Arm7SetAlarm",
    "OS_SetPeriodicAlarm": "hwsnd_Arm7SetPeriodicAlarm",
    "OS_CancelAlarm": "hwsnd_Arm7CancelAlarm",
    "OS_GetTick": "hwsnd_Arm7GetTick",
}
# Every undefined reference of the finished object, with its class. A name not listed here fails the audit.
#  shared  = resolves to the ARM9-side SDK function on purpose, safe because it is pure data/IRQ-shadow work (the ARM7 slice runs with ARM9 IRQs held off
#            by HWOS_Arm7Enter, so OS_Disable/RestoreInterrupts cannot dispatch);
#  hw      = defined by source/hw/hw_snd7.c (the redirect targets);
#  forbidden = the ARM7 build's OSThread/OSContext layout differs from the ARM9's (164/72 vs 192/100 bytes): any reference corrupts memory, so it is
#            an ERROR if one remains (review 1.5 F3; snd_main.c compiles the thread entry points out under SDK_GCC);
#  libgcc / state as named.
UNDEF_OK = {
    "OS_DisableInterrupts": "shared", "OS_RestoreInterrupts": "shared", "OS_InitMessageQueue": "shared", "OS_SendMessage": "shared",
    "OS_ReceiveMessage": "shared", "OS_SpinWait": "shared", "MIi_CpuCopy32": "shared",
    "OS_Terminate": "shared",   # OS_Panic in the FINALROM build: the three compiled-out thread entry points
    "__aeabi_idiv": "libgcc", "__aeabi_ldivmod": "libgcc",
    "g_hwregs": "state",
}
for _new in REDIRECT.values():
    UNDEF_OK[_new] = "hw"
FORBIDDEN = {"OS_CreateThread", "OS_SetThreadPriority", "OS_WakeupThreadDirect"}


def run(cmd):
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        sys.exit("arm7_snd: failed: " + " ".join(cmd[:3]))
    return r.stdout


def nm(arm, obj, *flags):
    out = []
    for line in run([arm + "/bin/arm-none-eabi-nm", *flags, obj]).splitlines():
        p = line.split()
        if len(p) >= 2:
            out.append((p[-2], p[-1]))
    return out


def main():
    arm = os.environ.get("DEVKITARM", "/opt/devkitpro/devkitARM")
    if "--arm" in sys.argv:
        arm = sys.argv[sys.argv.index("--arm") + 1]
    objs = sorted(os.path.join(d, f) for d, _, fs in os.walk(os.path.join(ROOT, "port/obj/arm7")) for f in fs if f.endswith(".o"))
    want = [f for f in run([sys.executable, "-I", "tools/port_files.py", "snd7"]).split()]
    have_names = sorted(os.path.basename(o)[:-2].removesuffix(".c") for o in objs)   # snd_main.c.o -> snd_main
    want_names = sorted(os.path.basename(w).removesuffix(".c") for w in want)
    if have_names != want_names:   # by name, not by count: a stale or renamed object must not pass
        sys.exit("arm7_snd: objects in port/obj/arm7 differ from tools/port_files.py snd7 (run make port-objects): extra %s, missing %s" %
                 (sorted(set(have_names) - set(want_names)), sorted(set(want_names) - set(have_names))))
    os.makedirs(os.path.join(ROOT, "port/arm7snd"), exist_ok=True)   # scratch outside port/obj: the link trial links every .o under port/obj
    raw = os.path.join(ROOT, "port/arm7snd/arm7_snd_raw.o")
    out = os.path.join(ROOT, "port/obj/arm7_snd.o")
    syms = os.path.join(ROOT, "port/arm7snd/arm7_snd.redefine")
    run([arm + "/bin/arm-none-eabi-ld", "-r", "-o", raw] + objs)
    defined = sorted({n for t, n in nm(arm, raw, "-g", "--defined-only") if t not in ("U",)})
    undefined = sorted({n for t, n in nm(arm, raw, "-g", "--undefined-only")})
    with open(syms, "w") as f:
        for n in defined:
            f.write("%s %s%s\n" % (n, PREFIX, n))
        for old, new in sorted(REDIRECT.items()):
            if old in undefined:
                f.write("%s %s\n" % (old, new))
    run([arm + "/bin/arm-none-eabi-objcopy", "--redefine-syms=" + syms, raw, out])
    d2 = sorted({n for t, n in nm(arm, out, "-g", "--defined-only")})
    u2 = sorted({n for t, n in nm(arm, out, "-g", "--undefined-only")})
    bad = [n for n in d2 if not n.startswith(PREFIX)]
    clash = sorted(set(u2) & set(d2))
    dangling = [n for n in u2 if n.startswith(PREFIX)]
    unredirected = sorted(set(REDIRECT) & set(u2))
    forbidden = sorted(set(u2) & FORBIDDEN)
    unknown = sorted(n for n in u2 if n not in UNDEF_OK and n not in FORBIDDEN and not n.startswith(PREFIX))
    print("arm7_snd: %d objects -> %s; %d defined symbols prefixed %s; %d redirected references; %d undefined references remain" %
          (len(objs), os.path.relpath(out, ROOT), len(d2), PREFIX, len([o for o in REDIRECT if o in undefined]), len(u2)))
    for cls in ("shared", "hw", "libgcc", "state"):
        print("arm7_snd:   %-6s %s" % (cls, " ".join(n for n in u2 if UNDEF_OK.get(n) == cls)))
    problems = []
    if bad:
        problems.append("defined without prefix: " + " ".join(bad))
    if clash:
        problems.append("defined and referenced: " + " ".join(clash))
    if dangling:
        problems.append("dangling prefixed references: " + " ".join(dangling))
    if unredirected:
        problems.append("still referenced (redirect failed): " + " ".join(unredirected))
    if forbidden:
        problems.append("forbidden references (ARM7 OSThread layout differs from the ARM9's): " + " ".join(forbidden))
    if unknown:
        problems.append("undefined references not in the UNDEF_OK allowlist (classify them in tools/arm7_snd.py): " + " ".join(unknown))
    for p in problems:
        print("arm7_snd: AUDIT FAIL: " + p, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
