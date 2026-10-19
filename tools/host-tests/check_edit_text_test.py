#!/usr/bin/env python3
"""check_edit_text_test.py -- the P1 scan gate (tools/check_edit_text.py) on a synthetic pret tree and edits.

usage: python3 -I tools/host-tests/check_edit_text_test.py      (part of `make host-tests`)

A planted 45-character pret line must FAIL the gate, in an .ed payload and in a new/ file, re-indented or not, and as a
40-character run inside a longer line; a 39-character run must pass; an allow-listed line passes; a stale allow entry
fails; a binary pret file and the pret build/ directory are not read.
Release review F1-F4: a verbatim multi-line block of short lines (each < 40 chars) FAILS as a payload block and as a new/ file
(a short joined block passes; a run inside one line is the line unit's job); `foo(a,b,c);` against the pret `foo(a, b, c);`
FAILS (40 non-blank characters), 39 pass; the reference tree must be a git checkout at the pinned commit (here: the synthetic
repo's own HEAD, via --pinned), clean, holding every .ed target with the header's SHA-1 and none of the new/ paths; a file
under port-edits/ that is neither README, *.ed nor new/** is refused.
Prints "check_edit_text_test: N checks, F failures".
"""
from __future__ import annotations

import hashlib
import importlib.util
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "check_edit_text.py"
_spec = importlib.util.spec_from_file_location("check_edit_text", TOOL)
CT = importlib.util.module_from_spec(_spec)
sys.modules["check_edit_text"] = CT
_spec.loader.exec_module(CT)
AE = sys.modules["apply_edits"]

HEADS: dict[Path, str] = {}
SHORT_FN = ["void Foo(void)", "{", "    SND_UpdateExChannel();", "    SND_CommandProc();", "    SND_SeqMain(doPeriodicProc);",
            "    SND_ExChannelMain(doPeriodicProc);", "}"]          # 7 lines, every one under 40 characters
SPACED = "void Foo(void) { SomeLongFunctionName(argumentNumberOne, argumentNumberTwo, argumentNumberThree); }"
PRET_LINE = "    MemoryRegionInitialiserForTheSdk(ctx, 0x400);"          # 45 characters after normalising
assert len(CT.normalise(PRET_LINE.encode())) == 45
LONG_PRET = "u32 a = TheQuickBrownFoxJumpsOverTheLazyDog_And_Then_Some_More(1);"

checks = 0
failures = 0


def check(cond: bool, what: str) -> None:
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}")


PRISTINE = b"x\n"          # what src/a.c holds in the synthetic pret tree: the .ed headers carry its SHA-1


def commit_all(ref: Path) -> str:
    ident = ["-c", "user.name=t", "-c", "user.email=293649481+GuyShtainer@users.noreply.github.com", "-c", "commit.gpgsign=false"]
    for args in (["init", "-q"], ["add", "-A"], [*ident, "commit", "-q", "--allow-empty", "-m", "x"]):
        subprocess.run(["git", "-C", str(ref), *args], check=True, capture_output=True)
    return subprocess.run(["git", "-C", str(ref), "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()


def make_tree(tmp: Path) -> tuple[Path, Path]:
    ref, edits = tmp / "ref", tmp / "edits"
    (ref / "src").mkdir(parents=True)
    (ref / "src" / "a.c").write_bytes(PRISTINE)
    (ref / "src" / "p.c").write_text(f"int a;\n{PRET_LINE}\n{LONG_PRET}\n\n" + "\n\n".join(SHORT_FN) + f"\n{SPACED}\n")
    (ref / ".gitignore").write_text("build/\n")
    (ref / "build").mkdir()
    (ref / "build" / "gen.c").write_text("this line only exists in the generated build dir, never read\n")
    (ref / "src" / "blob.bin").write_bytes(b"\0" + b"binary data that also says ThisIsBinaryAndMustNotBeScanned!!\n")
    (edits / "src").mkdir(parents=True)
    HEADS[ref] = commit_all(ref)
    return ref, edits


def write_ed(edits: Path, payload: list[str], name: str = "src/a.c") -> None:
    pristine = PRISTINE
    body = f"@1 insert-before {len(payload)}\n" + "".join(f"|{p}\n" for p in payload)
    text = (f"3DP-edit 1\npath {name}\nsha1 {hashlib.sha1(pristine).hexdigest()}\npret {AE.PRET_COMMIT}\n" + body)
    (edits / (name + ".ed")).write_text(text)


def run(ref: Path, edits: Path, allow: Path, extra: list[str] = ()) -> tuple[int, str]:
    pinned = ["--pinned", HEADS[ref]] if ref in HEADS else []
    proc = subprocess.run([sys.executable, "-I", str(TOOL), "--ref", str(ref), "--edits", str(edits), "--allow", str(allow), *pinned, *extra],
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


def case(tmp: Path, label: str, payload: list[str], expect_rc: int, needle: str = "", allow_lines: list[str] = (),
         new_file: str | None = None) -> None:
    sub = tmp / label.replace(" ", "_")
    ref, edits = make_tree(sub)
    write_ed(edits, payload)
    if new_file is not None:
        (edits / "new" / "lib").mkdir(parents=True)
        (edits / "new" / "lib" / "ours.c").write_text(new_file)
    allow = sub / "allow.txt"
    allow.write_text("# test allow list\n" + "".join(line + "\n" for line in allow_lines))
    rc, out = run(ref, edits, allow)
    check(rc == expect_rc, f"{label}: rc {rc}, want {expect_rc}\n{out}")
    if needle:
        check(needle in out, f"{label}: output lacks {needle!r}\n{out}")


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="check_edit_text_test."))
    try:
        case(tmp, "clean", ["int ours_only_and_long_enough_to_be_scanned_here = 1;"], 0, "0 with pret text")
        case(tmp, "planted 45", [PRET_LINE], 1, "PRET TEXT src/a.c.ed")
        case(tmp, "planted reindented", ["\t\t" + PRET_LINE.strip().replace(" ", "   ")], 1, "PRET TEXT")
        case(tmp, "40 run in a longer line", ["x:" + LONG_PRET[8:48] + ":1;"], 1, "PRET TEXT")
        case(tmp, "39 run passes", ["x:" + LONG_PRET[8:47] + "!!" + LONG_PRET[47:60]], 0, "0 with pret text")
        case(tmp, "planted in new file", ["int fine;"], 1, "PRET TEXT new/lib/ours.c", new_file=f"int ours;\n{PRET_LINE}\n")
        digest = hashlib.sha1(CT.normalise(PRET_LINE.encode())).hexdigest()
        case(tmp, "allowed", [PRET_LINE], 0, "1 allowed", [f"src/a.c.ed\t{digest}\tprototype"])
        case(tmp, "allowed in another file only", [PRET_LINE], 1, "PRET TEXT", [f"src/b.c.ed\t{digest}\tprototype"])
        case(tmp, "stale allow", ["int fine;"], 1, "STALE ALLOW", [f"src/a.c.ed\t{digest}\tprototype"])
        case(tmp, "build dir not read", ["this line only exists in the generated build dir, never read"], 0, "0 with pret text")
        case(tmp, "binary not read", ["binary data that also says ThisIsBinaryAndMustNotBeScanned!!"], 0, "0 with pret text")
        # F1: a verbatim block of short lines.
        case(tmp, "short line function", SHORT_FN, 1, "PRET TEXT src/a.c.ed:6 (block of 7 lines)")
        case(tmp, "short line function reindented, blank lines", ["\t" + l.strip() if i % 2 else "" for i, l in enumerate(SHORT_FN)]
             + [""] + SHORT_FN, 1, "block of")
        case(tmp, "short block passes", SHORT_FN[:3], 0, "0 with pret text")
        case(tmp, "short function in a new file", ["int fine;"], 1, "PRET TEXT new/lib/ours.c:1 (block of", new_file="\n".join(SHORT_FN) + "\n")
        # F4: re-spacing is not a way around the gate.
        nos = CT.noblank(CT.normalise(SPACED.encode())).decode()
        case(tmp, "spaces removed", [nos], 1, "PRET TEXT")
        case(tmp, "no-blank 40 run", ["Q" + nos[4:44] + "!;"], 1, "PRET TEXT")
        case(tmp, "no-blank 39 run passes", ["Q" + nos[4:43] + "!;"], 0, "0 with pret text")
        # F2: the reference tree.
        two = tmp / "refcases"
        ref, edits = make_tree(two / "plain")
        write_ed(edits, ["int fine;"])
        allow = two / "allow.txt"
        allow.write_text("")
        shutil.rmtree(ref / ".git")
        HEADS.pop(ref)
        rc, out = run(ref, edits, allow)
        check(rc == 2 and CT.AE.PRET_COMMIT in out and "not a git checkout" in out, f"ref without .git: rc {rc}\n{out}")
        ref, edits = make_tree(two / "wrongpin")
        write_ed(edits, ["int fine;"])
        HEADS[ref] = "0" * 40
        rc, out = run(ref, edits, allow)
        check(rc == 2 and "not the pinned commit" in out, f"ref at another commit: rc {rc}\n{out}")
        ref, edits = make_tree(two / "dirty")
        write_ed(edits, ["int fine;"])
        (ref / "src" / "p.c").write_text("changed\n")
        rc, out = run(ref, edits, allow)
        check(rc == 2 and "local changes" in out, f"dirty ref: rc {rc}\n{out}")
        ref, edits = make_tree(two / "empty")
        write_ed(edits, ["int fine;"], name="src/gone.c")
        rc, out = run(ref, edits, allow)
        check(rc == 2 and "missing from the pret tree" in out, f"ed target absent in the ref: rc {rc}\n{out}")
        ref, edits = make_tree(two / "badsha")
        write_ed(edits, ["int fine;"])
        (ref / "src" / "a.c").write_bytes(b"y\n")
        HEADS[ref] = commit_all(ref)
        rc, out = run(ref, edits, allow)
        check(rc == 2 and "SHA-1" in out, f"ed target with another SHA-1: rc {rc}\n{out}")
        ref, edits = make_tree(two / "newexists")
        write_ed(edits, ["int fine;"])
        (edits / "new" / "src").mkdir(parents=True)
        (edits / "new" / "src" / "p.c").write_text("int ours;\n")
        rc, out = run(ref, edits, allow)
        check(rc == 2 and "already exists in the pret tree" in out, f"new/ path present in the ref: rc {rc}\n{out}")
        ref, edits = make_tree(two / "noed")
        (edits / "src" / "a.c.ed").unlink(missing_ok=True)
        rc, out = run(ref, edits, allow)
        check(rc == 0, f"a tree with no .ed at all is fine when the ref is sound: rc {rc}\n{out}")
        # F3: stray files.
        for stray in ("src/a.c.orig", "src/a.c.ed~", "extra/x", "b.txt"):
            ref, edits = make_tree(two / ("stray_" + stray.replace("/", "_")))
            write_ed(edits, ["int fine;"])
            (edits / stray).parent.mkdir(parents=True, exist_ok=True)
            (edits / stray).write_text(PRET_LINE + "\n")
            rc, out = run(ref, edits, allow)
            check(rc == 2 and "neither README" in out and stray in out, f"stray {stray}: rc {rc}\n{out}")
        ref, edits = make_tree(two / "readme")
        write_ed(edits, ["int fine;"])
        (edits / "README").write_text("notes\n")
        rc, out = run(ref, edits, allow)
        check(rc == 0, f"port-edits/README is allowed: rc {rc}\n{out}")
        bad = tmp / "malformed"
        ref, edits = make_tree(bad)
        write_ed(edits, ["int fine;"])
        (bad / "allow.txt").write_text("only two\tfields\n")
        rc, out = run(ref, edits, bad / "allow.txt")
        check(rc != 0 and "expected" in out, f"malformed allow list: rc {rc}\n{out}")
        rc, out = run(tmp / "missing", edits, bad / "allow.txt")
        check(rc == 2 and "pret tree not found" in out, f"missing pret tree: rc {rc}\n{out}")
        # RF3 G5: the tree scan (--tree) reads the exported tree's other text files
        sub = tmp / "tree"
        ref, edits = make_tree(sub)
        write_ed(edits, ["int fine;"])
        (sub / "allow.txt").write_text("")
        for label, body, want in (("a clean tree passes", "int ours = 1;\n", 0),
                                  ("a verbatim pret line in source/ fails", f"int ours;\n{LONG_PRET}\n", 1),
                                  ("a re-indented pret line in tools/ fails", f"    {LONG_PRET}\n", 1)):
            tree = sub / ("t_" + str(want) + str(len(body)))
            (tree / "source").mkdir(parents=True)
            (tree / "source" / "x.c").write_text(body)
            rc, out = run(ref, edits, sub / "allow.txt", ["--tree", str(tree)])
            check(rc == want and (want == 0 or "@tree/source/x.c" in out), f"tree scan, {label}: rc {rc}\n{out}")
        rc, out = run(ref, edits, sub / "allow.txt")
        check(rc == 0, f"without --tree the plant is not read: rc {rc}\n{out}")
        key = hashlib.sha1(CT.normalise(LONG_PRET.encode())).hexdigest()
        (sub / "allow2.txt").write_text(f"@tree/source/x.c\t{key}\tour test line\n")
        tree = sub / "t_allow"
        (tree / "source").mkdir(parents=True)
        (tree / "source" / "x.c").write_text(f"{LONG_PRET}\n")
        rc, out = run(ref, edits, sub / "allow2.txt", ["--tree", str(tree)])
        check(rc == 0, f"tree scan, an allow entry passes it: rc {rc}\n{out}")
        rc, out = run(ref, edits, sub / "allow2.txt")
        check(rc == 0, f"a tree allow entry is not stale without --tree: rc {rc}\n{out}")
    finally:
        shutil.rmtree(tmp)
    print(f"check_edit_text_test: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
