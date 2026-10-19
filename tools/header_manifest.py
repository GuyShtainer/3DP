#!/usr/bin/env python3
"""header_manifest.py -- the hash manifest of the generated headers tools/sync_port.sh copies out of your pret build (release F3).

usage: python3 -I tools/header_manifest.py check [--ref DIR] [--manifest FILE]
       python3 -I tools/header_manifest.py write [--ref DIR] [--manifest FILE]

The headers (.h, .naix, .inc under build/generated, build/res and build/subprojects/NitroSDK-4.2.30001/gen of the pret checkout) are
made by pret's own build tools, one of them (metang) pinned by a tag that can move. `check` refuses (exit 1) when any file differs from the
committed manifest, is missing, or is extra; sync_port.sh runs it before it copies anything. The manifest holds a SHA-256 and a path per
line, never any content. The include-guard lines are masked before hashing (pret derives them from the checkout's absolute path), so the
manifest is the same for a checkout at any path. `write` regenerates the manifest from a pret build you trust (the maintainer's, after a pin change).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DIRS = ("generated", "res", "subprojects/NitroSDK-4.2.30001/gen")
SUFFIXES = (".h", ".naix", ".inc")


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


GUARD_MASK = "@GUARD@"


def normalise_guard(data: bytes) -> bytes:
    """The header bytes with its include guard masked. pret's build derives the guard from the ABSOLUTE path of the checkout
    (`#ifndef _USERS_ME_POKEPLATINUM_BUILD_RES_..._H`), so the same header hashes differently on every machine. When the first two
    non-blank lines are `#ifndef X` / `#define X`, every `#ifndef X`, `#define X` and `#endif // X` (or `/* X */`) line becomes X = @GUARD@.
    Every other byte is kept, so a real change to the header still changes the hash."""
    lines = data.split(b"\n")
    idx = [i for i, ln in enumerate(lines) if ln.strip()][:2]
    if len(idx) < 2:
        return data
    a, b = lines[idx[0]].split(), lines[idx[1]].split()
    if len(a) != 2 or len(b) != 2 or a[0] != b"#ifndef" or b[0] != b"#define" or a[1] != b[1]:
        return data
    name = a[1]
    keep = {b"#ifndef " + name, b"#define " + name}
    mask = GUARD_MASK.encode()
    for i, ln in enumerate(lines):
        s = ln.rstrip(b"\r").strip()
        tail = ln[len(ln.rstrip(b"\r")):]
        if s in keep:
            lines[i] = s.replace(name, mask) + tail
        elif s in (b"#endif // " + name, b"#endif /* " + name + b" */", b"#endif //" + name):
            lines[i] = b"#endif // " + mask + tail
    return b"\n".join(lines)


def collect(ref: Path) -> dict[str, str]:
    build = ref / "build"
    found: dict[str, str] = {}
    for d in DIRS:
        base = build / d
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if p.is_file() and p.suffix in SUFFIXES:
                found[p.relative_to(build).as_posix()] = hashlib.sha256(normalise_guard(p.read_bytes())).hexdigest()
    return found


def read_manifest(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for n, line in enumerate(path.read_text().splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        parts = line.split("  ", 1)
        if len(parts) != 2 or len(parts[0]) != 64:
            raise SystemExit(f"{path}:{n}: expected '<sha256>  <path>'")
        out[parts[1]] = parts[0]
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("mode", choices=("check", "write"))
    ap.add_argument("--ref", type=Path, default=default_ref(ROOT))
    ap.add_argument("--manifest", type=Path, default=HERE / "generated-headers.sha256")
    args = ap.parse_args(argv)
    found = collect(args.ref)
    if not found:
        print(f"header_manifest: no generated headers under {args.ref}/build (build pret first: tools/get_pret.sh)", file=sys.stderr)
        return 2
    if args.mode == "write":
        lines = ["# SHA-256 of the generated headers copied out of the pret build (tools/header_manifest.py write). Hashes and paths only."]
        lines += [f"{h}  {p}" for p, h in sorted(found.items())]
        args.manifest.write_text("\n".join(lines) + "\n")
        print(f"header_manifest: wrote {len(found)} entries to {args.manifest}")
        return 0
    want = read_manifest(args.manifest)
    missing = sorted(set(want) - set(found))
    extra = sorted(set(found) - set(want))
    changed = sorted(p for p in set(want) & set(found) if want[p] != found[p])
    if missing or extra or changed:
        print(f"header_manifest: MISMATCH against {args.manifest}: {len(changed)} changed, {len(missing)} missing, {len(extra)} extra "
              f"(of {len(want)} listed, {len(found)} found)", file=sys.stderr)
        for tag, names in (("changed", changed), ("missing", missing), ("extra", extra)):
            for p in names[:5]:
                print(f"  {tag}: {p}", file=sys.stderr)
        print("The generated headers must come from pret's own build at the pinned commit (tools/get_pret.sh), with the default options.", file=sys.stderr)
        return 1
    print(f"header_manifest: ok ({len(found)} generated headers match tools/generated-headers.sha256)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
