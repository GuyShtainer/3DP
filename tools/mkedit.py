#!/usr/bin/env python3
"""mkedit.py -- generate a port-edits .ed file from a pristine pret file and our edited copy of it.

usage: python3 -I tools/mkedit.py PRISTINE EDITED [--path PRET_PATH] > x.ed
       python3 -I tools/mkedit.py --write PRET_PATH [PRET_PATH ...]

The first form prints the .ed (the pret path defaults to the part of EDITED after its last 'port/').
--write regenerates port-edits/<pret path>.ed from $PLAT_REF/<pret path> (default_ref()) and
port/<pret path>; a file with no pristine copy is copied to port-edits/new/ instead, and an unchanged file has its .ed
removed. Either way the result is applied back in memory and must reproduce EDITED byte for byte, or nothing is written.

How the ops are chosen (the format is in tools/apply_edits.py and port-edits/README):
- a line diff (difflib, no junk heuristic) gives kept, deleted and inserted runs;
- inside a replaced run, each removed line is paired with at most one added line, in order, maximising the summed
  token similarity (pairs below MIN_PAIR_RATIO stay unpaired);
- a paired line becomes one splice per token-level difference, so the .ed carries only the changed bytes and never the
  original text around them; unpaired removed lines become deletes and unpaired added lines inserts.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import importlib.util
import os
import re
import sys
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
sys.modules["apply_edits"] = AE     # dataclasses look their module up here
_spec.loader.exec_module(AE)

MIN_PAIR_RATIO = 0.5
MAX_PAIR_CELLS = 250_000          # a replaced run bigger than this (removed x added lines) is not paired at all
TOKEN_RE = re.compile(rb"\w+|\s+|[^\w\s]")


def tokens(line: bytes) -> list[bytes]:
    return TOKEN_RE.findall(line)


def similarity(a: bytes, b: bytes) -> float:
    return difflib.SequenceMatcher(None, tokens(a), tokens(b), autojunk=False).ratio()


def pair_lines(removed: list[bytes], added: list[bytes]) -> list[tuple[int, int]]:
    """Order-preserving pairing of removed with added lines maximising the summed similarity (ties: earliest pairs)."""
    n, m = len(removed), len(added)
    if n * m > MAX_PAIR_CELLS:
        return []
    score = [[0.0] * (m + 1) for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        for j in range(m - 1, -1, -1):
            best = max(score[i + 1][j], score[i][j + 1])
            ratio = similarity(removed[i], added[j])
            if ratio >= MIN_PAIR_RATIO:
                best = max(best, ratio + score[i + 1][j + 1])
            score[i][j] = best
    pairs = []
    i = j = 0
    while i < n and j < m:
        ratio = similarity(removed[i], added[j])
        if ratio >= MIN_PAIR_RATIO and score[i][j] == ratio + score[i + 1][j + 1]:
            pairs.append((i, j))
            i, j = i + 1, j + 1
        elif score[i][j] == score[i + 1][j]:
            i += 1
        else:
            j += 1
    return pairs


def splice_ops(line_no: int, old: bytes, new: bytes) -> list[str]:
    """One splice per token-level difference between old and new (ascending columns)."""
    old_t, new_t = tokens(old), tokens(new)
    assert b"".join(old_t) == old and b"".join(new_t) == new, "tokenizer must cover every byte"
    out = []
    matcher = difflib.SequenceMatcher(None, old_t, new_t, autojunk=False)
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "equal":
            continue
        col = len(b"".join(old_t[:i1]))
        length = len(b"".join(old_t[i1:i2]))
        out.append(f"@{line_no} splice {col} {length}")
        out.append("|" + b"".join(new_t[j1:j2]).decode("latin-1"))
    return out


class OpWriter:
    """Collects ops in file order; consecutive inserts at one position merge into one op."""

    def __init__(self, total: int):
        self.total = total
        self.lines: list[str] = []
        self.pending: list[bytes] = []
        self.pending_at = 0

    def insert(self, before_index: int, text: bytes) -> None:
        """Queue an inserted line before original 0-based index `before_index` (== total: at the end)."""
        if self.pending and self.pending_at != before_index:
            self.flush()
        self.pending_at = before_index
        self.pending.append(text)

    def flush(self) -> None:
        if not self.pending:
            return
        if self.pending_at < self.total:
            head = f"@{self.pending_at + 1} insert-before {len(self.pending)}"
        else:
            if self.total == 0:
                raise SystemExit("mkedit: the pristine file is empty: ship it under port-edits/new/ instead")
            head = f"@{self.total} insert-after {len(self.pending)}"
        self.lines.append(head)
        self.lines.extend("|" + t.decode("latin-1") for t in self.pending)
        self.pending = []

    def emit(self, op_lines: list[str]) -> None:
        self.flush()
        self.lines.extend(op_lines)


def _replace_block(w: OpWriter, old: list[bytes], new: list[bytes], i1: int, j1: int, i2: int, j2: int) -> None:
    pairs = pair_lines(old[i1:i2], new[j1:j2])
    i, j = i1, j1
    for pi, pj in [(p + i1, q + j1) for p, q in pairs] + [(i2, j2)]:
        if pi > i:
            w.emit([f"@{i + 1} delete {pi - i}"])
        for k in range(j, pj):
            w.insert(pi, new[k])
        if pi < i2:
            w.emit(splice_ops(pi + 1, old[pi], new[pj]))
        i, j = pi + 1, pj + 1


def make_ops(pristine: bytes, edited: bytes) -> list[str]:
    old, _ = AE.split_lines(pristine)
    new, _ = AE.split_lines(edited)
    w = OpWriter(len(old))
    matcher = difflib.SequenceMatcher(None, old, new, autojunk=False)
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "delete":
            w.emit([f"@{i1 + 1} delete {i2 - i1}"])
        elif tag == "insert":
            for k in range(j1, j2):
                w.insert(i1, new[k])
        elif tag == "replace":
            _replace_block(w, old, new, i1, j1, i2, j2)
    w.flush()
    return w.lines


def make_ed(pristine: bytes, edited: bytes, pret_path: str) -> bytes:
    """Return the .ed bytes for one file, verified by applying it back."""
    for label, data in (("pristine", pristine), ("edited", edited)):
        if b"\r" in data:
            raise SystemExit(f"mkedit: CR byte in the {label} file of {pret_path}")
    header = [AE.MAGIC.decode(), f"path {pret_path}", f"sha1 {hashlib.sha1(pristine).hexdigest()}",
              f"pret {AE.PRET_COMMIT}"]
    _, old_ends = AE.split_lines(pristine)
    new_lines, new_ends = AE.split_lines(edited)
    if new_lines and new_ends != old_ends:
        header.append("final-newline " + ("yes" if new_ends else "no"))
    text = ("\n".join(header + make_ops(pristine, edited)) + "\n").encode("latin-1")
    check = AE.apply_ed(AE.parse_ed(text, pret_path + ".ed"), pristine, pret_path + ".ed")
    if check != edited:
        raise SystemExit(f"mkedit: internal error: the generated .ed does not reproduce {pret_path}")
    return text


def write_one(pret_path: str, ref: Path, port: Path, edits: Path) -> str:
    src, dst = ref / pret_path, port / pret_path
    if not dst.is_file():
        raise SystemExit(f"mkedit: no edited file {dst}")
    ed_out = edits / (pret_path + ".ed")
    if not src.is_file():
        new_out = edits / AE.NEW_DIR / pret_path
        new_out.parent.mkdir(parents=True, exist_ok=True)
        new_out.write_bytes(dst.read_bytes())
        return f"new  {new_out}"
    pristine, edited = src.read_bytes(), dst.read_bytes()
    if pristine == edited:
        if ed_out.exists():
            ed_out.unlink()
        return f"same {pret_path} (no .ed)"
    data = make_ed(pristine, edited, pret_path)
    ed_out.parent.mkdir(parents=True, exist_ok=True)
    ed_out.write_bytes(data)
    n_ops = data.count(b"\n@")
    return f"ed   {ed_out} ({n_ops} ops)"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("files", nargs="+", help="PRISTINE EDITED, or with --write one or more pret paths")
    ap.add_argument("--path", help="the pret path for the header (default: EDITED after its last 'port/')")
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args(argv)
    if args.write:
        ref = default_ref(ROOT)
        for pret_path in args.files:
            print(write_one(pret_path, ref, ROOT / "port", ROOT / "port-edits"))
        return 0
    if len(args.files) != 2:
        ap.error("give PRISTINE and EDITED")
    pristine_path, edited_path = (Path(f) for f in args.files)
    pret_path = args.path
    if pret_path is None:
        parts = edited_path.as_posix().rsplit("port/", 1)
        if len(parts) != 2:
            ap.error("EDITED is not under a port/ directory: pass --path")
        pret_path = parts[1]
    sys.stdout.buffer.write(make_ed(pristine_path.read_bytes(), edited_path.read_bytes(), pret_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
