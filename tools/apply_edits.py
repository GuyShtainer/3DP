#!/usr/bin/env python3
"""apply_edits.py -- apply port-edits/ to a freshly synced port/ tree (tools/sync_port.sh calls it).

usage: python3 -I tools/apply_edits.py [--edits DIR] [--port DIR]

Every port-edits/<pret path>.ed edits the pristine pret file port/<pret path>; every file under port-edits/new/
is copied to port/<same relative path>. The .ed format (port-edits/README has the full text):

    3DP-edit 1
    path <pret path>
    sha1 <SHA-1 of the pristine file>
    pret <pinned pret commit>
    final-newline yes|no          (optional: only when the result's last-line newline differs from the pristine's)
    @N delete K                   remove original lines N .. N+K-1
    @N insert-before K            then K payload lines: inserted before original line N
    @N insert-after K             then K payload lines: inserted after original line N
    @N splice C L                 then 1 payload line: replaces bytes [C, C+L) of original line N (C counts from 0)

A payload line is '|' followed by our text. Every N is an ORIGINAL line number (1-based), so the ops are independent of
each other. The result is the same as applying them bottom-up by line and right-to-left by column. Between two original
lines, the insert-after ops of the upper line come first, then the insert-before ops of the lower one; ops at the same
position keep the order they are written in.

Refuses (exit 1, port/ untouched) on: a malformed .ed, a SHA-1 mismatch, a CR byte, overlapping ops (two deletes
sharing a line, a splice on a deleted line, two splices on one line whose byte ranges overlap or start at the same
column), an op past the end of the file or line, a new/ file that would overwrite a different existing file, a new/ file
and an .ed for the same path, and any file under port-edits/ that is neither port-edits/README, a *.ed outside new/, nor
under new/ (it would be neither applied nor scanned). Each output is written to a temp file and renamed, and a failing
write is reported with "re-run tools/sync_port.sh" (the rsync restores every pristine file) instead of a traceback.

`--check-ref REF` only verifies that REF is a git checkout at the pinned commit with a clean working tree (exit 1 if not).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

PRET_COMMIT = "c248fb3f8cc9934ded800e489567c5c0eeee92eb"
MAGIC = b"3DP-edit 1"
NEW_DIR = "new"
OP_RE = re.compile(rb"^@([0-9]+) (delete|insert-before|insert-after|splice) ([0-9]+)(?: ([0-9]+))?$")
SHA1_RE = re.compile(rb"^[0-9a-f]{40}$")


class EditError(Exception):
    """A refusal: the message names the .ed (or file) and the reason."""


@dataclass(frozen=True)
class Op:
    """One operation. `line` is the original 1-based line; `ed_line` is where it sits in the .ed (for messages)."""
    kind: str
    line: int
    count: int          # delete / insert-*: number of lines; splice: unused (0)
    col: int            # splice only
    length: int         # splice only
    payload: tuple[bytes, ...]
    ed_line: int


@dataclass(frozen=True)
class EditFile:
    path: str
    sha1: str
    final_newline: bool | None
    ops: tuple[Op, ...]


def split_lines(data: bytes) -> tuple[list[bytes], bool]:
    """Split file bytes into lines without their '\\n'; the flag says whether the last line ended with one."""
    if not data:
        return [], False
    ends = data.endswith(b"\n")
    body = data[:-1] if ends else data
    return body.split(b"\n"), ends


def join_lines(lines: list[bytes], final_newline: bool) -> bytes:
    if not lines:
        return b""
    return b"\n".join(lines) + (b"\n" if final_newline else b"")


def _header_value(lines: list[bytes], idx: int, key: bytes, name: str) -> bytes:
    if idx >= len(lines) or not lines[idx].startswith(key + b" "):
        raise EditError(f"{name}:{idx + 1}: expected '{key.decode()} <value>'")
    return lines[idx][len(key) + 1:]


def parse_ed(data: bytes, name: str) -> EditFile:
    """Parse one .ed file. Raises EditError on any deviation from the format."""
    if b"\r" in data:
        raise EditError(f"{name}: CR byte (CRLF line ends?): clone with core.autocrlf=false")
    if not data.endswith(b"\n"):
        raise EditError(f"{name}: does not end with a newline")
    lines = data[:-1].split(b"\n")
    if lines[0] != MAGIC:
        raise EditError(f"{name}:1: not a '{MAGIC.decode()}' file")
    path = _header_value(lines, 1, b"path", name).decode("utf-8")
    sha1 = _header_value(lines, 2, b"sha1", name)
    if not SHA1_RE.match(sha1):
        raise EditError(f"{name}:3: bad sha1")
    pret = _header_value(lines, 3, b"pret", name).decode("ascii")
    if pret != PRET_COMMIT:
        raise EditError(f"{name}:4: pret commit {pret} is not the pinned {PRET_COMMIT}")
    if path.startswith("/") or ".." in path.split("/") or path.startswith(NEW_DIR + "/"):
        raise EditError(f"{name}:2: bad path {path!r}")
    idx = 4
    final_newline = None
    if idx < len(lines) and lines[idx].startswith(b"final-newline "):
        value = lines[idx][len(b"final-newline "):]
        if value not in (b"yes", b"no"):
            raise EditError(f"{name}:{idx + 1}: final-newline must be yes or no")
        final_newline = value == b"yes"
        idx += 1
    ops = []
    while idx < len(lines):
        m = OP_RE.match(lines[idx])
        if not m:
            raise EditError(f"{name}:{idx + 1}: expected an op line")
        line_no, kind, a, b = int(m.group(1)), m.group(2).decode(), int(m.group(3)), m.group(4)
        if (kind == "splice") != (b is not None):
            raise EditError(f"{name}:{idx + 1}: wrong operand count for {kind}")
        n_payload = 1 if kind == "splice" else (a if kind.startswith("insert") else 0)
        if kind != "splice" and a < 1:
            raise EditError(f"{name}:{idx + 1}: {kind} needs a count of at least 1")
        payload = lines[idx + 1: idx + 1 + n_payload]
        if len(payload) != n_payload or any(not p.startswith(b"|") for p in payload):
            raise EditError(f"{name}:{idx + 1}: {kind} needs {n_payload} payload line(s) starting with '|'")
        payload = tuple(p[1:] for p in payload)
        if kind == "splice":
            op = Op(kind, line_no, 0, a, int(b), payload, idx + 1)
            if op.length == 0 and not payload[0]:
                raise EditError(f"{name}:{idx + 1}: empty splice")
        else:
            op = Op(kind, line_no, a, 0, 0, payload, idx + 1)
        ops.append(op)
        idx += 1 + n_payload
    return EditFile(path, sha1.decode("ascii"), final_newline, tuple(ops))


def _check_bounds(op: Op, lines: list[bytes], name: str) -> None:
    total = len(lines)
    if not 1 <= op.line <= total:
        raise EditError(f"{name}:{op.ed_line}: line {op.line} is past the end of the file ({total} lines)")
    if op.kind == "delete" and op.line + op.count - 1 > total:
        raise EditError(f"{name}:{op.ed_line}: delete {op.count} runs past the end of the file ({total} lines)")
    if op.kind == "splice" and op.col + op.length > len(lines[op.line - 1]):
        raise EditError(f"{name}:{op.ed_line}: splice {op.col}+{op.length} is past the end of line {op.line} "
                        f"({len(lines[op.line - 1])} bytes)")


def _deleted_lines(ops: tuple[Op, ...], name: str) -> set[int]:
    deleted: set[int] = set()
    for op in ops:
        if op.kind != "delete":
            continue
        span = set(range(op.line, op.line + op.count))
        if span & deleted:
            raise EditError(f"{name}:{op.ed_line}: delete overlaps another delete")
        deleted |= span
    return deleted


def _splice_line(text: bytes, splices: list[Op], name: str) -> bytes:
    """Apply one line's splices right-to-left by column after refusing overlapping byte ranges."""
    ordered = sorted(splices, key=lambda s: s.col)
    for left, right in zip(ordered, ordered[1:]):
        if right.col == left.col or right.col < left.col + left.length:
            raise EditError(f"{name}:{right.ed_line}: splice overlaps the splice at {name}:{left.ed_line}")
    for op in reversed(ordered):
        text = text[:op.col] + op.payload[0] + text[op.col + op.length:]
    return text


def apply_ed(ed: EditFile, pristine: bytes, name: str) -> bytes:
    """Return the edited bytes of one pristine file. Raises EditError on any refusal."""
    digest = hashlib.sha1(pristine).hexdigest()
    if digest != ed.sha1:
        raise EditError(f"{name}: SHA-1 mismatch for {ed.path}: have {digest}, the .ed wants {ed.sha1} "
                        f"(is the pret checkout at {PRET_COMMIT}?)")
    if b"\r" in pristine:
        raise EditError(f"{name}: CR byte in the pristine {ed.path}")
    lines, ends = split_lines(pristine)
    for op in ed.ops:
        _check_bounds(op, lines, name)
    deleted = _deleted_lines(ed.ops, name)
    before: dict[int, list[bytes]] = {}
    after: dict[int, list[bytes]] = {}
    splices: dict[int, list[Op]] = {}
    for op in ed.ops:
        if op.kind == "insert-before":
            before.setdefault(op.line, []).extend(op.payload)
        elif op.kind == "insert-after":
            after.setdefault(op.line, []).extend(op.payload)
        elif op.kind == "splice":
            if op.line in deleted:
                raise EditError(f"{name}:{op.ed_line}: splice on deleted line {op.line}")
            splices.setdefault(op.line, []).append(op)
    out: list[bytes] = []
    for number, text in enumerate(lines, 1):
        out.extend(before.get(number, ()))
        if number not in deleted:
            out.append(_splice_line(text, splices[number], name) if number in splices else text)
        out.extend(after.get(number, ()))
    return join_lines(out, ends if ed.final_newline is None else ed.final_newline)


def check_ref(ref: Path, pinned: str = PRET_COMMIT) -> None:
    """Raise EditError unless `ref` is the top of a git checkout whose HEAD is `pinned` and whose working tree is clean."""
    def git(*args: str) -> str:
        try:
            done = subprocess.run(["git", "-C", str(ref), *args], capture_output=True, text=True, check=False)
        except OSError as err:
            raise EditError(f"cannot run git to check the pret checkout {ref}: {err}") from err
        if done.returncode != 0:
            raise EditError(f"{ref} is not a git checkout; the pret reference must be a clone of pret/pokeplatinum at commit {pinned} "
                            f"(tools/get_pret.sh makes one)")
        return done.stdout.strip()

    if not ref.is_dir():
        raise EditError(f"pret checkout not found: {ref} (expected pret/pokeplatinum at commit {pinned}; run tools/get_pret.sh)")
    top = git("rev-parse", "--show-toplevel")
    if os.path.realpath(top) != os.path.realpath(ref):
        raise EditError(f"{ref} is not the top of a git checkout (it sits inside {top}); expected a clone at commit {pinned}")
    head = git("rev-parse", "HEAD")
    if head != pinned:
        raise EditError(f"the pret checkout {ref} is at {head}, not the pinned commit {pinned}")
    dirty = git("status", "--porcelain")
    if dirty:
        raise EditError(f"the pret checkout {ref} has local changes (git status --porcelain):\n{dirty}")


def stray_files(edits: Path) -> list[str]:
    """Files under `edits` that are neither README (top level), a *.ed outside new/, nor under new/: not applied, not scanned."""
    out = []
    for path in sorted(p for p in edits.rglob("*") if p.is_file() or p.is_symlink()):
        rel = path.relative_to(edits)
        if rel.parts[0] == NEW_DIR or rel.as_posix() == "README" or (path.name.endswith(".ed") and path.is_file()):
            continue
        out.append(rel.as_posix())
    return out


def ed_files(edits: Path) -> list[Path]:
    return sorted(p for p in edits.rglob("*.ed") if p.relative_to(edits).parts[0] != NEW_DIR)


def new_files(edits: Path) -> list[Path]:
    root = edits / NEW_DIR
    return sorted(p for p in root.rglob("*") if p.is_file()) if root.is_dir() else []


def plan(edits: Path, port: Path) -> tuple[list[tuple[Path, bytes]], int]:
    """Validate everything and compute every output in memory: (target, bytes) pairs and the op count."""
    stray = stray_files(edits)
    if stray:
        raise EditError(f"{edits}: file(s) that are neither README, *.ed nor under new/ (never applied, never scanned): " + ", ".join(stray))
    writes: list[tuple[Path, bytes]] = []
    n_ops = 0
    ed_targets: set[Path] = set()
    for ed_path in ed_files(edits):
        name = str(ed_path)
        ed = parse_ed(ed_path.read_bytes(), name)
        expect = ed_path.relative_to(edits).as_posix()[:-len(".ed")]
        if ed.path != expect:
            raise EditError(f"{name}: header path {ed.path} does not match its location ({expect})")
        target = port / ed.path
        if not target.is_file():
            raise EditError(f"{name}: no pristine file {target} (run the rsync half of tools/sync_port.sh)")
        writes.append((target, apply_ed(ed, target.read_bytes(), name)))
        ed_targets.add(target)
        n_ops += len(ed.ops)
    for src in new_files(edits):
        data = src.read_bytes()
        if b"\r" in data:
            raise EditError(f"{src}: CR byte")
        target = port / src.relative_to(edits / NEW_DIR)
        if target in ed_targets:
            raise EditError(f"{src}: the same path also has a .ed ({target}); a new/ file and an edit cannot both own it")
        if target.exists() and target.read_bytes() != data:
            raise EditError(f"{src}: would overwrite the different existing file {target}")
        writes.append((target, data))
    return writes, n_ops


def write_atomic(target: Path, data: bytes) -> None:
    """Write `data` to a temp file beside `target`, then rename it over `target` (a failure leaves `target` as it was)."""
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_name("." + target.name + ".3dp-tmp")
    try:
        tmp.write_bytes(data)
        if target.exists():
            shutil.copymode(target, tmp)
        os.replace(tmp, target)
    except OSError:
        tmp.unlink(missing_ok=True)
        raise


def main(argv: list[str] | None = None) -> int:
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--edits", type=Path, default=root / "port-edits")
    ap.add_argument("--port", type=Path, default=root / "port")
    ap.add_argument("--check-ref", type=Path, metavar="REF", help="only check that REF is the pinned pret commit, clean")
    args = ap.parse_args(argv)
    if args.check_ref is not None:
        try:
            check_ref(args.check_ref)
        except EditError as err:
            print(f"apply_edits: REFUSED: {err}", file=sys.stderr)
            return 1
        print(f"apply_edits: pret checkout {args.check_ref} is at {PRET_COMMIT}, clean")
        return 0
    try:
        writes, n_ops = plan(args.edits, args.port)
    except EditError as err:
        print(f"apply_edits: REFUSED: {err}", file=sys.stderr)
        return 1
    for target, data in writes:
        try:
            write_atomic(target, data)
        except OSError as err:
            print(f"apply_edits: FAILED writing {target}: {err}; port/ may be half-edited: re-run tools/sync_port.sh "
                  f"(its rsync restores every pristine file)", file=sys.stderr)
            return 1
    n_new = len(new_files(args.edits))
    print(f"apply_edits: {len(writes) - n_new} files edited ({n_ops} ops), {n_new} new files copied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
