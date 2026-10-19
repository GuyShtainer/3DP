#!/usr/bin/env python3
"""check_edit_text.py -- the P1 scan gate: port-edits/ must carry no pret/SDK text.

usage: python3 -I tools/check_edit_text.py [--edits DIR] [--ref DIR] [--allow FILE] [--tree DIR]

Fails (exit 1) when a run of RUN (40) or more characters of any text the edits carry also appears in the pristine pret
tree ($PLAT_REF, see default_ref(): every text file outside build/ and .git/).

THE WINDOW RULE. Two kinds of carried text are compared, each in two forms, against the same two forms of every pret file:
  * line units: every payload line of every .ed and every line of every port-edits/new/ file, on its own;
  * block units: every op's contiguous payload (an insert's K lines) and every new/ file as a whole, as its non-blank
    lines joined with '\\n'. Only a shared run that contains a '\\n' counts for a block (a run inside one line is the line
    unit's job), so a verbatim function body made of short lines cannot hide behind the per-line threshold. Blank lines
    are dropped on both sides.
  * form 1 ("normal"): runs of blanks collapsed to one space, line ends stripped, so a re-indented copy is caught;
    form 2 ("no-blank"): every blank deleted (the '\\n' separators stay), so `foo(a,b,c);` equals `foo(a, b, c);`.
    A run is RUN characters of the form it is found in, i.e. in the no-blank form RUN NON-BLANK characters (plus any
    '\\n'), which is the stricter reading of the 40-character threshold.
A pret tree that is not the pinned commit with a clean working tree, a .ed whose target is missing from it or has another
SHA-1, a new/ file that exists in it, or a file under port-edits/ that is neither README, new/** nor a *.ed, is refused
(exit 2 / 1): the gate must not pass vacuously.

The exceptions are listed in tools/check_edit_text_allow.txt as '<file under port-edits>\\t<sha1>\\t<why>': prototype and
identifier lines that must spell the SDK's own declaration. The sha1 is of the normalised line (line units) or of the
joined block (block units), so an entry cannot be widened by editing the text around it. An entry no longer used is
reported and fails the gate, so the list cannot rot.

THE TREE SCAN (--tree DIR, RF3 G5; run by the private release export on the exported tree). Port-edits are not the only place pret text could land: every other
text file of the exported tree (source/, port-shim/, tools/, the makefiles; not port-edits/, which the scan above owns) is compared with the same
two forms and the same allow list (entries keyed `@tree/<path relative to the tree>`; they are ignored, not stale, when no --tree is given). The same windows would hit thousands of harmless units there
(#include runs, `/* ---- */` rules, a stub that has to repeat an SDK prototype), so the tree scan counts a unit only when it is
  * a line unit: a normalised line of 40+ characters that is not an #include, not a function signature / prototype (class "signature": the SDK fixes
    those, as in the allow list) and is at least 40% letters and digits, or
  * a block unit: a block of 12 consecutive such lines (stride 6) of which MORE THAN HALF of the non-blank characters lie inside shared windows.
Anything it reports is either reworded or put on the allow list with a why.

Method: a shared run of RUN characters always contains one of the pret text's RUN/2-character windows at an offset that
is a multiple of RUN/2, so only those windows are looked up in our text's set of RUN/2-grams; the (at most RUN/2+1)
RUN-windows around each hit are then checked exactly against our RUN-grams.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def default_ref(root: Path) -> Path:
    """The pret checkout. ONE rule, shared with tools/port.mk, tools/get_pret.sh and tools/sync_port.sh:
    $PLAT_REF if set and non-empty (used as given; a relative value is relative to the current directory); else the FIRST line of
    <repo>/.plat_ref (CR and surrounding blanks dropped; an absolute path is used as is, a relative one is relative to the repo root);
    else <repo>/../pokeplatinum (what tools/get_pret.sh makes)."""
    env = os.environ.get("PLAT_REF")
    if env:
        return Path(env)
    f = root / ".plat_ref"
    if f.is_file():
        lines = f.read_text().splitlines()
        first = lines[0].strip() if lines else ""
        if first:
            return root / first        # pathlib: an absolute `first` replaces root
    return root.parent / "pokeplatinum"
_spec = importlib.util.spec_from_file_location("apply_edits", HERE / "apply_edits.py")
AE = importlib.util.module_from_spec(_spec)
sys.modules["apply_edits"] = AE
_spec.loader.exec_module(AE)

RUN = 40
HALF = RUN // 2
SKIP_DIRS = {".git", "build"}
BLANKS = re.compile(rb"[ \t\f\v]+")
SNIFF = 8192


def normalise(line: bytes) -> bytes:
    return BLANKS.sub(b" ", line).strip()


def joined(lines) -> bytes:
    """The block form: the non-blank normalised lines joined with '\\n'."""
    return b"\n".join(n for n in (normalise(x) for x in lines) if n)


def noblank(text: bytes) -> bytes:
    """The second form: every blank deleted (normalised text has only b' ' blanks); '\\n' separators stay."""
    return text.replace(b" ", b"")


@dataclass(frozen=True)
class Unit:
    """One piece of carried text. kind 'line': one normalised line; kind 'block': a joined contiguous payload / whole file."""
    rel: str
    number: int          # line number in that file (a block: its first line)
    kind: str
    text: bytes          # the normal form (the allow-list key is its sha1)
    n_lines: int


def carried_units(edits: Path) -> list[Unit]:
    """Every line unit and every multi-line block unit the edits carry that is long enough to matter."""
    out: list[Unit] = []

    def add(rel: str, first: int, lines: list[bytes]) -> None:
        for k, text in enumerate(lines):
            n = normalise(text)
            if len(n) >= RUN:
                out.append(Unit(rel, first + k, "line", n, 1))
        block = joined(lines)
        if block.count(b"\n") >= 1 and len(block) >= RUN:
            out.append(Unit(rel, first, "block", block, block.count(b"\n") + 1))

    for ed_path in AE.ed_files(edits):
        rel = ed_path.relative_to(edits).as_posix()
        ed = AE.parse_ed(ed_path.read_bytes(), str(ed_path))
        for op in ed.ops:
            add(rel, op.ed_line + 1, list(op.payload))
    for path in AE.new_files(edits):
        add(path.relative_to(edits).as_posix(), 1, path.read_bytes().split(b"\n"))
    return out


TREE_BLOCK = 12
TREE_STRIDE = 6
TREE_SKIP_TOP = {".git", "port-edits", "port", "build"}


SIGNATURE = re.compile(rb"^(?:(?:static|extern|inline|const|volatile|unsigned|signed|struct|enum)\s+)*[A-Za-z_]\w*(?:\s*\*+\s*|\s+)(?:\*+\s*)?[A-Za-z_]\w*\s*\([^;{}]*\)\s*(?:;|\{)?$")


def is_signature(line: bytes) -> bool:
    """A function signature or prototype line (`u32 Foo(const Bar *b, int n)`, with or without `;` / `{`): an interface the SDK fixes, as the allow list
    already treats them (class "signature"). Control statements (`if (...)`, `return f(x);`, `while (...)`) and calls are not signatures."""
    first = line.split(b"(", 1)[0].split()
    if not first or first[0] in (b"if", b"for", b"while", b"switch", b"return", b"else", b"sizeof", b"case", b"do", b"goto"):
        return False
    return SIGNATURE.match(line) is not None and b"=" not in line.split(b"(", 1)[0]


def alnum_share(line: bytes) -> float:
    """The share of letters and digits in a line: a `/* ---- */` rule or a bare brace scores low."""
    return sum(chr(c).isalnum() for c in line) / max(1, len(line))


def tree_units(tree: Path) -> list[Unit]:
    """Line and block units of every text file of an exported tree outside port-edits/ (see THE TREE SCAN)."""
    out: list[Unit] = []
    for dirpath, dirnames, filenames in os.walk(tree):
        if Path(dirpath) == tree:
            dirnames[:] = sorted(d for d in dirnames if d not in TREE_SKIP_TOP and not d.startswith("build"))
        else:
            dirnames[:] = sorted(d for d in dirnames if d != ".git")
        for name in sorted(filenames):
            path = Path(dirpath) / name
            if path.is_symlink() or not path.is_file() or name.startswith(("LICENSE", "COPYING")):
                continue        # licence texts are shared with every project on purpose
            data = path.read_bytes()
            if b"\0" in data[:SNIFF]:
                continue
            rel = "@tree/" + path.relative_to(tree).as_posix()      # the allow list keys tree units as @tree/<path>
            lines = data.split(b"\n")
            kept = [(i + 1, normalise(x)) for i, x in enumerate(lines) if normalise(x) and not normalise(x).startswith(b"#include") and alnum_share(normalise(x)) >= 0.4 and not is_signature(normalise(x))]
            for number, n in kept:
                if len(n) >= RUN:
                    out.append(Unit(rel, number, "line", n, 1))
            for s in range(0, max(1, len(kept) - TREE_BLOCK + TREE_STRIDE), TREE_STRIDE):
                chunk = kept[s:s + TREE_BLOCK]
                if len(chunk) < 2:
                    continue
                block = b"\n".join(n for _, n in chunk)
                if len(block) >= RUN:
                    out.append(Unit(rel, chunk[0][0], "block", block, len(chunk)))
    return out


def pret_text_files(ref: Path):
    for dirpath, dirnames, filenames in os.walk(ref):
        if Path(dirpath) == ref:
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        else:
            dirnames[:] = [d for d in dirnames if d != ".git"]
        for name in filenames:
            path = Path(dirpath) / name
            if path.is_symlink() or not path.is_file():
                continue
            with open(path, "rb") as fh:
                head = fh.read(SNIFF)
                if b"\0" in head:
                    continue
                yield path, head + fh.read()


def find_hits(units: list[Unit], ref: Path) -> tuple[dict[int, set[bytes]], int]:
    """Map index into `units` -> every shared RUN-window found (both forms); plus the number of pret files read."""
    halves: set[bytes] = set()
    runs: dict[bytes, list[int]] = {}
    for idx, unit in enumerate(units):
        for form in (unit.text, noblank(unit.text)):
            for p in range(len(form) - HALF + 1):
                halves.add(form[p:p + HALF])
            for p in range(len(form) - RUN + 1):
                window = form[p:p + RUN]
                if (b"\n" in window) == (unit.kind == "block"):
                    runs.setdefault(window, []).append(idx)
    hits: dict[int, set[bytes]] = {}
    n_files = 0
    for _, data in pret_text_files(ref):
        n_files += 1
        text = joined(data.split(b"\n"))
        for form in (text, noblank(text)):
            for q in range(0, len(form) - HALF + 1, HALF):
                if form[q:q + HALF] not in halves:
                    continue
                for start in range(max(0, q - HALF), min(q, len(form) - RUN) + 1):
                    window = form[start:start + RUN]
                    for idx in runs.get(window, ()):
                        hits.setdefault(idx, set()).add(window)
    return hits, n_files


def coverage(unit: Unit, windows: set[bytes]) -> tuple[int, int]:
    """(characters, of how many) of the unit's no-blank form that lie inside a shared window: how much of it is pret text."""
    flat = noblank(unit.text)
    covered = bytearray(len(flat))
    for form in (unit.text, flat):
        for p in range(len(form) - RUN + 1):
            if form[p:p + RUN] in windows:
                if form is flat:
                    covered[p:p + RUN] = b"\1" * RUN
                else:   # map the normal-form position onto the no-blank form
                    start = len(noblank(form[:p]))
                    covered[start:start + len(noblank(form[p:p + RUN]))] = b"\1" * len(noblank(form[p:p + RUN]))
    return sum(covered), len(flat)


def check_targets(edits: Path, ref: Path) -> None:
    """Every .ed's target must exist in `ref` with the header's SHA-1, and no new/ path may exist there (else the gate could pass vacuously)."""
    for ed_path in AE.ed_files(edits):
        ed = AE.parse_ed(ed_path.read_bytes(), str(ed_path))
        target = ref / ed.path
        if not target.is_file():
            raise AE.EditError(f"{ed_path}: its target {ed.path} is missing from the pret tree {ref}")
        digest = hashlib.sha1(target.read_bytes()).hexdigest()
        if digest != ed.sha1:
            raise AE.EditError(f"{ed_path}: SHA-1 of {ed.path} in the pret tree is {digest}, the .ed wants {ed.sha1}")
    for path in AE.new_files(edits):
        rel = path.relative_to(edits / AE.NEW_DIR)
        if (ref / rel).exists():
            raise AE.EditError(f"{path}: new/ file, but {rel} already exists in the pret tree")


def load_allow(path: Path) -> dict[tuple[str, str], str]:
    allow = {}
    if not path.is_file():
        return allow
    for number, raw in enumerate(path.read_text().split("\n"), 1):
        if not raw.strip() or raw.startswith("#"):
            continue
        fields = raw.split("\t")
        if len(fields) != 3:
            raise SystemExit(f"{path}:{number}: expected '<file>\\t<sha1>\\t<why>'")
        allow[(fields[0], fields[1])] = fields[2]
    return allow


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--edits", type=Path, default=ROOT / "port-edits")
    ap.add_argument("--ref", type=Path, default=default_ref(ROOT))
    ap.add_argument("--allow", type=Path, default=HERE / "check_edit_text_allow.txt")
    ap.add_argument("--tree", type=Path, default=None, help="also scan this exported tree's other text files (THE TREE SCAN); with it the port-edits scan still runs")
    ap.add_argument("--pinned", default=AE.PRET_COMMIT, help="the commit the reference must be at (tests only; default: the pinned pret commit)")
    args = ap.parse_args(argv)
    if not (args.ref / "src").is_dir():
        print(f"check_edit_text: pret tree not found: {args.ref}", file=sys.stderr)
        return 2
    try:
        AE.check_ref(args.ref, args.pinned)
        stray = AE.stray_files(args.edits)
        if stray:
            raise AE.EditError(f"{args.edits}: file(s) that are neither README, *.ed nor under new/ (never applied, never scanned): " + ", ".join(stray))
        check_targets(args.edits, args.ref)
        units = carried_units(args.edits)
        n_ed_units = len(units)
        if args.tree is not None:
            if not args.tree.is_dir():
                raise AE.EditError(f"--tree {args.tree} is not a directory")
            units += tree_units(args.tree)
    except AE.EditError as err:
        print(f"check_edit_text: REFUSED: {err}", file=sys.stderr)
        return 2
    tree_rels = {u.rel for u in units[len(units) - n_tree:]} if (n_tree := (len(units) - n_ed_units)) else set()
    hits, n_files = find_hits(units, args.ref)
    allow = load_allow(args.allow)
    used = set()
    bad = n_allowed = 0
    for idx in sorted(hits):
        unit = units[idx]
        key = (unit.rel, hashlib.sha1(unit.text).hexdigest())
        got, of = coverage(unit, hits[idx])
        if args.tree is not None and unit.rel in tree_rels and got * 2 <= of:
            continue        # the tree scan counts a unit only when more than half of it is shared
        where = f"{unit.rel}:{unit.number}" + (f" (block of {unit.n_lines} lines)" if unit.kind == "block" else "")
        if key in allow:
            used.add(key)
            n_allowed += 1
            print(f"allowed   {where} [{got}/{of} chars shared] ({allow[key]})")
            continue
        bad += 1
        print(f"PRET TEXT {where}: shares {RUN}+ chars with the pret tree "
              f"({got} of {of} non-blank chars): {min(hits[idx]).decode('latin-1')!r}  [allow key {key[1]}]")
    stale = sorted(k for k in set(allow) - used if args.tree is not None or not k[0].startswith("@tree/"))
    for rel, digest in stale:
        print(f"STALE ALLOW {rel} {digest} ({allow[(rel, digest)]}): no longer matches any carried unit")
    n_lines = sum(1 for u in units if u.kind == "line")
    print(f"check_edit_text: {n_lines} carried lines of {RUN}+ chars and {len(units) - n_lines} multi-line blocks vs {n_files} pret text files: "
          f"{bad} with pret text, {n_allowed} allowed ({len(used)} allow entries), {len(stale)} stale allow entries")
    return 1 if bad or stale else 0


if __name__ == "__main__":
    sys.exit(main())
