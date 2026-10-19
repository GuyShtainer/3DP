#!/usr/bin/env python3
"""port_edits_test.py -- tools/apply_edits.py and tools/mkedit.py (release P1).

usage: python3 -I tools/host-tests/port_edits_test.py      (part of `make host-tests`; needs the pret tree, $PLAT_REF)

1. Round trip, every committed edit: pristine pret file + .ed -> edited bytes; those equal port/<path> when port/ is
   synced; mkedit regenerates the committed .ed byte for byte from (pristine, edited); every new/ file equals its port/
   copy.
2. Refusals: SHA-1 mismatch, CR bytes (in the .ed and in the pristine file), overlaps (two deletes, a splice on a deleted
   line, overlapping splices, two splices at one column), ops past the end (line, delete count, splice range), malformed
   files, a header path that does not match the location, a new/ file over a different file; and a refused plan writes
   nothing.
3. Semantics on hand-built files: insert order between two lines, ops at one position, right-to-left splices,
   final-newline.
4. A seeded fuzz: random edits of random files must survive mkedit -> apply_edits unchanged.
Prints "port_edits_test: N checks, F failures" and exits 1 on any failure.
"""
from __future__ import annotations

import hashlib
import importlib.util
import os
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def default_ref(root: Path) -> Path:
    """The pret checkout: $PLAT_REF, else the one line in <repo>/.plat_ref, else ../pokeplatinum (what tools/get_pret.sh makes)."""
    env = os.environ.get("PLAT_REF")
    if env:
        return Path(env)
    f = root / ".plat_ref"
    if f.is_file() and f.read_text().strip():
        return root / f.read_text().strip()
    return root.parent / "pokeplatinum"

TOOLS = ROOT / "tools"


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


AE = _load("apply_edits")
MK = _load("mkedit")

checks = 0
failures = 0


def check(cond: bool, what: str) -> None:
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}")


def refused(fn, needle: str, what: str) -> None:
    """fn() must raise EditError whose message contains needle."""
    try:
        fn()
    except AE.EditError as err:
        check(needle in str(err), f"{what}: refused, but the message lacks {needle!r}: {err}")
        return
    check(False, f"{what}: not refused")


def header(pristine: bytes, path: str = "src/x.c", extra: str = "") -> str:
    return (f"3DP-edit 1\npath {path}\nsha1 {hashlib.sha1(pristine).hexdigest()}\npret {AE.PRET_COMMIT}\n" + extra)


def apply_text(pristine: bytes, body: str, path: str = "src/x.c") -> bytes:
    text = (header(pristine, path) + body).encode("latin-1")
    return AE.apply_ed(AE.parse_ed(text, "t.ed"), pristine, "t.ed")


# ---- 1. round trip over the committed edits ---------------------------------------------------------------------
def round_trip(edits: Path, ref: Path, port: Path) -> None:
    n_ed = n_new = n_port = 0
    for ed_path in AE.ed_files(edits):
        rel = ed_path.relative_to(edits).as_posix()[:-len(".ed")]
        data = ed_path.read_bytes()
        ed = AE.parse_ed(data, str(ed_path))
        check(ed.path == rel, f"{rel}: header path {ed.path}")
        pristine = (ref / rel).read_bytes()
        edited = AE.apply_ed(ed, pristine, str(ed_path))
        check(edited != pristine, f"{rel}: the edit changes nothing")
        check(MK.make_ed(pristine, edited, rel) == data, f"{rel}: mkedit does not regenerate the committed .ed")
        if (port / rel).is_file():
            n_port += 1
            check((port / rel).read_bytes() == edited, f"{rel}: port/ differs from pristine + .ed (re-run tools/sync_port.sh)")
        n_ed += 1
    for src in AE.new_files(edits):
        rel = src.relative_to(edits / AE.NEW_DIR).as_posix()
        check(not (ref / rel).exists(), f"new/{rel}: a pret file of that name exists (ship an .ed instead)")
        if (port / rel).is_file():
            check((port / rel).read_bytes() == src.read_bytes(), f"new/{rel}: port/ copy differs")
        n_new += 1
    check(n_ed > 0, "no .ed files found")
    print(f"port_edits_test: round trip over {n_ed} .ed files ({n_port} compared with port/) and {n_new} new files")


# ---- 2. refusals ----------------------------------------------------------------------------------------------------
P = b"line one\nline two\nline three\nline four\n"


def refusals() -> None:
    good = header(P) + "@2 splice 5 3\n|2\n"
    check(apply_text(P, "@2 splice 5 3\n|2\n") == b"line one\nline 2\nline three\nline four\n", "baseline splice")
    # SHA-1 mismatch: a different pristine file, and the edited file itself (a second apply).
    ed = AE.parse_ed(good.encode(), "t.ed")
    refused(lambda: AE.apply_ed(ed, P + b"x\n", "t.ed"), "SHA-1 mismatch", "sha mismatch")
    refused(lambda: AE.apply_ed(ed, AE.apply_ed(ed, P, "t.ed"), "t.ed"), "SHA-1 mismatch", "double apply")
    # CR bytes.
    refused(lambda: AE.parse_ed(good.replace("\n", "\r\n").encode(), "t.ed"), "CR byte", "CRLF .ed")
    refused(lambda: AE.parse_ed((good + "@1 insert-before 1\n|a\rb\n").encode(), "t.ed"), "CR byte", "CR in a payload")
    crlf = P.replace(b"\n", b"\r\n")
    refused(lambda: AE.apply_ed(AE.parse_ed((header(crlf) + "@1 delete 1\n").encode(), "t.ed"), crlf, "t.ed"),
            "CR byte in the pristine", "CRLF pristine")
    # Overlaps.
    refused(lambda: apply_text(P, "@1 delete 2\n@2 delete 1\n"), "overlaps another delete", "two deletes share a line")
    refused(lambda: apply_text(P, "@1 delete 3\n@3 delete 2\n"), "overlaps another delete", "delete ranges overlap at the end")
    refused(lambda: apply_text(P, "@2 delete 2\n@3 splice 0 1\n|x\n"), "splice on deleted line", "splice on a deleted line")
    refused(lambda: apply_text(P, "@1 splice 0 4\n|x\n@1 splice 3 2\n|y\n"), "overlaps the splice", "overlapping splices")
    refused(lambda: apply_text(P, "@1 splice 2 0\n|x\n@1 splice 2 0\n|y\n"), "overlaps the splice", "two inserts at one column")
    refused(lambda: apply_text(P, "@1 splice 2 1\n|x\n@1 splice 2 0\n|y\n"), "overlaps the splice", "two splices at one column")
    check(apply_text(P, "@1 splice 0 4\n|L\n@1 splice 4 1\n|_\n") == b"L_one\nline two\nline three\nline four\n",
          "adjacent splices (touching, not overlapping) are fine")
    # Past the end.
    refused(lambda: apply_text(P, "@5 delete 1\n"), "past the end of the file", "line past EOF")
    refused(lambda: apply_text(P, "@0 delete 1\n"), "past the end of the file", "line 0")
    refused(lambda: apply_text(P, "@4 delete 2\n"), "runs past the end", "delete count past EOF")
    refused(lambda: apply_text(P, "@5 insert-before 1\n|x\n"), "past the end of the file", "insert past EOF")
    refused(lambda: apply_text(P, "@1 splice 6 3\n|x\n"), "past the end of line", "splice range past the line")
    refused(lambda: apply_text(P, "@1 splice 9 0\n|x\n"), "past the end of line", "splice column past the line")
    check(apply_text(P, "@1 splice 8 0\n|!\n").startswith(b"line one!\n"), "a splice at the end of a line is fine")
    # Malformed.
    bad = [
        ("3DP-edit 2\n", "not a '3DP-edit 1' file"),
        (good.replace("path src/x.c", "path ../x.c"), "bad path"),
        (good.replace("path src/x.c", "path new/x.c"), "bad path"),
        (good.replace(AE.PRET_COMMIT, "0" * 40), "is not the pinned"),
        (good.replace("sha1 ", "sha1 Z"), "bad sha1"),
        (header(P) + "@1 delete\n", "expected an op line"),
        (header(P) + "@1 splice 0\n|x\n", "wrong operand count"),
        (header(P) + "@1 delete 1 2\n", "wrong operand count"),
        (header(P) + "@1 insert-before 2\n|x\n", "payload line"),
        (header(P) + "@1 insert-before 1\nx\n", "payload line"),
        (header(P) + "@1 delete 0\n", "at least 1"),
        (header(P) + "@1 splice 0 0\n|\n", "empty splice"),
        (header(P) + "final-newline maybe\n", "yes or no"),
        (header(P) + "@1 replace 1\n", "expected an op line"),
        (good[:-1], "does not end with a newline"),
    ]
    for text, needle in bad:
        refused(lambda t=text: AE.parse_ed(t.encode(), "t.ed"), needle, f"malformed ({needle})")


def plan_refusals(tmp: Path) -> None:
    edits, port = tmp / "edits", tmp / "port"
    (edits / "src").mkdir(parents=True)
    (port / "src").mkdir(parents=True)
    (port / "src" / "x.c").write_bytes(P)
    (port / "src" / "y.c").write_bytes(P)
    (edits / "src" / "x.c.ed").write_text(header(P, "src/x.c") + "@1 delete 1\n")
    writes, n_ops = AE.plan(edits, port)
    check(len(writes) == 1 and n_ops == 1, "plan: one edit, one op")
    # A header path that does not match the location.
    (edits / "src" / "y.c.ed").write_text(header(P, "src/x.c") + "@1 delete 1\n")
    refused(lambda: AE.plan(edits, port), "does not match its location", "header path vs location")
    (edits / "src" / "y.c.ed").unlink()
    # No pristine file.
    (edits / "src" / "z.c.ed").write_text(header(P, "src/z.c") + "@1 delete 1\n")
    refused(lambda: AE.plan(edits, port), "no pristine file", "missing pristine file")
    (edits / "src" / "z.c.ed").unlink()
    # A new/ file over a different existing file; the same bytes are fine (a second sync).
    (edits / "new" / "src").mkdir(parents=True)
    (edits / "new" / "src" / "n.c").write_bytes(b"ours\n")
    check(len(AE.plan(edits, port)[0]) == 2, "plan: a new file is copied")
    (port / "src" / "n.c").write_bytes(b"ours\n")
    check(len(AE.plan(edits, port)[0]) == 2, "plan: a new file over its own copy is fine")
    (port / "src" / "n.c").write_bytes(b"theirs\n")
    refused(lambda: AE.plan(edits, port), "would overwrite", "new file over a different file")
    (port / "src" / "n.c").write_bytes(b"ours\n")
    # Atomic: one bad .ed among good ones -> exit 1 and port/ untouched.
    (edits / "src" / "y.c.ed").write_text(header(P + b"x\n", "src/y.c") + "@1 delete 1\n")
    before = {p: p.read_bytes() for p in port.rglob("*") if p.is_file()}
    proc = subprocess.run([sys.executable, "-I", str(TOOLS / "apply_edits.py"), "--edits", str(edits), "--port", str(port)],
                          capture_output=True, text=True)
    check(proc.returncode == 1 and "REFUSED" in proc.stderr, f"cli refusal: rc {proc.returncode}, {proc.stderr!r}")
    after = {p: p.read_bytes() for p in port.rglob("*") if p.is_file()}
    check(before == after, "a refused plan wrote nothing")
    (edits / "src" / "y.c.ed").unlink()
    proc = subprocess.run([sys.executable, "-I", str(TOOLS / "apply_edits.py"), "--edits", str(edits), "--port", str(port)],
                          capture_output=True, text=True)
    check(proc.returncode == 0 and "1 files edited (1 ops), 1 new files copied" in proc.stdout, f"cli apply: {proc.stdout!r}")
    check((port / "src" / "x.c").read_bytes() == b"line two\nline three\nline four\n", "cli apply result")
    proc = subprocess.run([sys.executable, "-I", str(TOOLS / "apply_edits.py"), "--edits", str(edits), "--port", str(port)],
                          capture_output=True, text=True)
    check(proc.returncode == 1 and "SHA-1 mismatch" in proc.stderr, "cli: a second apply without a fresh rsync is refused")


def git_repo(path: Path, files: dict[str, str]) -> str:
    """Make a one-commit git repo at `path` holding `files`; returns its HEAD."""
    path.mkdir(parents=True, exist_ok=True)
    for name, text in files.items():
        (path / name).parent.mkdir(parents=True, exist_ok=True)
        (path / name).write_text(text)
    ident = ["-c", "user.name=t", "-c", "user.email=293649481+GuyShtainer@users.noreply.github.com", "-c", "commit.gpgsign=false"]
    for args in (["init", "-q"], ["add", "-A"], [*ident, "commit", "-q", "-m", "x"]):
        subprocess.run(["git", "-C", str(path), *args], check=True, capture_output=True)
    return subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()


def more_plan_refusals(tmp: Path) -> None:
    """Release review F2/F3/F6: the reference tree, stray files, new/ vs .ed, atomic writes."""
    edits, port = tmp / "e2", tmp / "p2"
    (edits / "src").mkdir(parents=True)
    (port / "src").mkdir(parents=True)
    (port / "src" / "x.c").write_bytes(P)
    (edits / "src" / "x.c.ed").write_text(header(P, "src/x.c") + "@1 delete 1\n")
    (edits / "README").write_text("the one allowed top-level file\n")
    check(len(AE.plan(edits, port)[0]) == 1, "plan: README is not stray")
    for stray in ("src/x.c.orig", "src/x.c.ed~", "extra/x"):
        (edits / stray).parent.mkdir(parents=True, exist_ok=True)
        (edits / stray).write_text("a copy of a whole pret file\n")
        refused(lambda: AE.plan(edits, port), "neither README", f"stray file {stray}")
        check(AE.stray_files(edits) == [stray], f"stray_files lists {stray}: {AE.stray_files(edits)}")
        (edits / stray).unlink()
    (edits / "src" / "sub").mkdir()
    check(AE.stray_files(edits) == ["README"] or AE.stray_files(edits) == [], "an empty directory is not a stray file")
    (edits / "new" / "src").mkdir(parents=True)
    (edits / "new" / "src" / "x.c").write_bytes(P)
    refused(lambda: AE.plan(edits, port), "also has a .ed", "new/ file and .ed for one path")
    (edits / "new" / "src" / "x.c").unlink()
    # A write that fails: clean message, exit 1, the target untouched, no temp file left behind.
    os.chmod(port / "src", 0o555)
    try:
        proc = subprocess.run([sys.executable, "-I", str(TOOLS / "apply_edits.py"), "--edits", str(edits), "--port", str(port)],
                              capture_output=True, text=True)
    finally:
        os.chmod(port / "src", 0o755)
    if os.geteuid() != 0:
        check(proc.returncode == 1 and "re-run tools/sync_port.sh" in proc.stderr and "Traceback" not in proc.stderr,
              f"unwritable target: rc {proc.returncode}, {proc.stderr!r}")
        check((port / "src" / "x.c").read_bytes() == P and sorted(q.name for q in (port / "src").iterdir()) == ["x.c"],
              "unwritable target: the file is as it was and no temp file is left")
    proc = subprocess.run([sys.executable, "-I", str(TOOLS / "apply_edits.py"), "--edits", str(edits), "--port", str(port)],
                          capture_output=True, text=True)
    check(proc.returncode == 0 and sorted(q.name for q in (port / "src").iterdir()) == ["x.c"], f"atomic write: {proc.stdout!r}")
    # check_ref: not a git checkout, wrong commit, dirty tree, a subdirectory of a repo; and the good case.
    plain = tmp / "plain"
    (plain / "src").mkdir(parents=True)
    refused(lambda: AE.check_ref(plain), AE.PRET_COMMIT, "ref that is not a git checkout names the expected commit")
    refused(lambda: AE.check_ref(tmp / "missing"), AE.PRET_COMMIT, "missing ref names the expected commit")
    repo = tmp / "repo"
    head = git_repo(repo, {"src/a.c": "int a;\n"})
    AE.check_ref(repo, head)
    check(True, "check_ref: a clean checkout at the pinned commit passes")
    refused(lambda: AE.check_ref(repo), "not the pinned commit", "ref at another commit")
    refused(lambda: AE.check_ref(repo / "src", head), "not the top of a git checkout", "ref below a repo top")
    (repo / "src" / "a.c").write_text("int b;\n")
    refused(lambda: AE.check_ref(repo, head), "local changes", "ref with a modified file")
    (repo / "src" / "a.c").write_text("int a;\n")
    (repo / "src" / "untracked.c").write_text("x\n")
    refused(lambda: AE.check_ref(repo, head), "local changes", "ref with an untracked file")
    proc = subprocess.run([sys.executable, "-I", str(TOOLS / "apply_edits.py"), "--check-ref", str(plain)], capture_output=True, text=True)
    check(proc.returncode == 1 and AE.PRET_COMMIT in proc.stderr, f"cli --check-ref on a plain dir: rc {proc.returncode} {proc.stderr!r}")
    # tools/sync_port.sh refuses the same ref before it copies anything.
    out = tmp / "sync_out"
    env = {**os.environ, "PLAT_REF": str(plain)}
    proc = subprocess.run(["sh", str(TOOLS / "sync_port.sh")], capture_output=True, text=True, env=env)
    check(proc.returncode != 0 and AE.PRET_COMMIT in proc.stderr, f"sync_port.sh on a non-git ref: rc {proc.returncode} {proc.stderr!r}")
    check(not out.exists(), "sync_port.sh wrote nothing for a refused ref")


# ---- 3. semantics ---------------------------------------------------------------------------------------------------
def semantics() -> None:
    # Between lines 1 and 2: after(1) ops in written order, then before(2) ops in written order.
    out = apply_text(P, "@2 insert-before 1\n|B1\n@1 insert-after 1\n|A1\n@2 insert-before 1\n|B2\n@1 insert-after 1\n|A2\n")
    check(out == b"line one\nA1\nA2\nB1\nB2\nline two\nline three\nline four\n", f"insert order: {out!r}")
    # Insert around a deleted line, and a splice next to inserts.
    out = apply_text(P, "@2 delete 1\n@2 insert-before 1\n|in\n@2 insert-after 1\n|out\n@3 splice 0 4\n|LINE\n")
    check(out == b"line one\nin\nout\nLINE three\nline four\n", f"inserts around a deleted line: {out!r}")
    # Splices apply right-to-left by column whatever order they are written in.
    out = apply_text(P, "@3 splice 5 5\n|3\n@3 splice 0 4\n|Line\n")
    check(out.split(b"\n")[2] == b"Line 3", f"splice order: {out!r}")
    # final-newline both ways, and unchanged when absent.
    nonl = b"a\nb"
    check(apply_text(nonl, "@2 splice 0 1\n|c\n") == b"a\nc", "no final newline is kept")
    text = (header(nonl) + "final-newline yes\n@2 splice 0 1\n|c\n").encode()
    check(AE.apply_ed(AE.parse_ed(text, "t"), nonl, "t") == b"a\nc\n", "final-newline yes")
    text = (header(P) + "final-newline no\n@4 delete 1\n").encode()
    check(AE.apply_ed(AE.parse_ed(text, "t"), P, "t") == b"line one\nline two\nline three", "final-newline no")
    check(MK.make_ed(nonl, b"a\nc\n", "src/x.c").count(b"final-newline yes") == 1, "mkedit writes final-newline yes")
    check(b"final-newline" not in MK.make_ed(P, P.replace(b"two", b"2"), "src/x.c"), "mkedit omits final-newline when unchanged")
    # Deleting everything leaves an empty file.
    check(apply_text(P, "@1 delete 4\n") == b"", "delete all")
    # mkedit prefers a splice for a near-copy: the .ed carries the changed token, not the line.
    ed = MK.make_ed(b"static int foo(int a, int b);\n", b"static int foo(int a, long b);\n", "src/x.c").decode()
    check("@1 splice" in ed and "|long" in ed and "static" not in ed.split("pret ")[1], f"near-copy splice: {ed!r}")
    # An insert at the end of the file uses insert-after on the last line.
    ed = MK.make_ed(P, P + b"tail\n", "src/x.c").decode()
    check("@4 insert-after 1\n|tail\n" in ed, f"append: {ed!r}")


# ---- 4. fuzz ----------------------------------------------------------------------------------------------------------
WORDS = [b"int", b"u32", b"x", b"foo", b"(", b")", b";", b" ", b"  ", b"\t", b"{", b"}", b"0x10", b"/*", b"*/", b"\xe9", b"#if"]


def rand_line(rng: random.Random) -> bytes:
    return b"".join(rng.choice(WORDS) for _ in range(rng.randrange(0, 9)))


def fuzz(rounds: int = 400) -> None:
    rng = random.Random(1760486400)
    for r in range(rounds):
        old = [rand_line(rng) for _ in range(rng.randrange(1, 25))]
        new = list(old)
        for _ in range(rng.randrange(1, 6)):
            k = rng.randrange(0, len(new) + 1)
            choice = rng.randrange(4)
            if choice == 0 and new:
                del new[min(k, len(new) - 1)]
            elif choice == 1:
                new.insert(k, rand_line(rng))
            elif new:
                k = min(k, len(new) - 1)
                toks = MK.tokens(new[k])
                if toks:
                    toks[rng.randrange(len(toks))] = rng.choice(WORDS)
                new[k] = b"".join(toks) + (rand_line(rng) if choice == 3 else b"")
        pristine = b"\n".join(old) + (b"\n" if rng.random() < 0.8 else b"")
        edited = b"\n".join(new) + (b"\n" if rng.random() < 0.8 else b"") if new else b""
        if pristine == edited:
            continue
        try:
            text = MK.make_ed(pristine, edited, "src/f.c")
            out = AE.apply_ed(AE.parse_ed(text, "f.ed"), pristine, "f.ed")
            check(out == edited, f"fuzz round {r}: apply(mkedit) != edited")
        except SystemExit as err:
            check(False, f"fuzz round {r}: mkedit refused: {err}")
        except AE.EditError as err:
            check(False, f"fuzz round {r}: apply refused: {err}")


def main() -> int:
    ref = default_ref(ROOT)
    if not (ref / "src").is_dir():
        print(f"port_edits_test: pret tree not found: {ref}")
        return 2
    round_trip(ROOT / "port-edits", ref, ROOT / "port")
    refusals()
    tmp = Path(tempfile.mkdtemp(prefix="port_edits_test."))
    try:
        plan_refusals(tmp)
        more_plan_refusals(tmp)
    finally:
        shutil.rmtree(tmp)
    semantics()
    fuzz()
    print(f"port_edits_test: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
