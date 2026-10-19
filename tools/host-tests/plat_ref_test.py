#!/usr/bin/env python3
"""plat_ref_test.py -- the one PLAT_REF resolution rule (release RF3 B1).

usage: python3 -I tools/host-tests/plat_ref_test.py      (part of `make host-tests`; needs nothing from pret)

The rule (tools/get_pret.sh, tools/sync_port.sh, tools/port.mk, tools/check_edit_text.py, tools/header_manifest.py, tools/mkedit.py):
$PLAT_REF first (as given), else the first line of <repo>/.plat_ref (absolute as is, relative to the repo root), else <repo>/../pokeplatinum.
Checks, in a scratch copy of the scripts: an absolute and a relative .plat_ref; env PLAT_REF taking priority; CR line ends and a second line;
get_pret refusing a DEST inside the repo even when it does not exist yet; sync_port naming the resolved path; the Python default_ref agreeing;
`make -n`-free check of port.mk's text. Prints "plat_ref_test: N checks, F failures"; exit 1 on any failure.
"""
from __future__ import annotations

import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
checks = 0
failures = 0


def check(cond: bool, what: str) -> None:
    """Count one check; print it when it fails."""
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}")


def run(cmd: list[str], env_extra: dict[str, str] | None = None, cwd: Path | None = None) -> tuple[int, str]:
    """Run a command with PLAT_REF cleared (then env_extra added); return (rc, stdout+stderr)."""
    env = {k: v for k, v in os.environ.items() if k not in ("PLAT_REF", "GET_PRET_CHECK_ONLY")}
    env.update(env_extra or {})
    p = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr).strip()


def load_default_ref(py: Path):
    spec = importlib.util.spec_from_file_location("m_" + py.stem, py)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod.default_ref


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp_s:
        tmp = Path(tmp_s).resolve()
        repo = tmp / "work" / "repo"
        (repo / "tools").mkdir(parents=True)
        for name in ("get_pret.sh", "sync_port.sh"):
            shutil.copy(ROOT / "tools" / name, repo / "tools" / name)
        get = ["sh", str(repo / "tools" / "get_pret.sh")]
        sync = ["sh", str(repo / "tools" / "sync_port.sh")]
        only = {"GET_PRET_CHECK_ONLY": "1"}
        outside = tmp / "elsewhere" / "pokeplatinum"          # does not exist yet, on purpose
        plat = repo / ".plat_ref"

        # default: ../pokeplatinum next to the repo
        rc, out = run(get, only)
        check(rc == 0 and out == str(repo.parent / "pokeplatinum"), f"default DEST: rc {rc} {out!r}")

        # absolute .plat_ref (the form BUILDING.md invites), DEST not created yet
        plat.write_text(f"{outside}\n")
        rc, out = run(get, only)
        check(rc == 0 and out == str(outside), f"absolute .plat_ref: rc {rc} {out!r}")
        rc, out = run(sync)
        check(rc == 1 and f"pret checkout not found: {outside} " in out, f"sync_port absolute .plat_ref: rc {rc} {out!r}")

        # relative .plat_ref is relative to the repo root, whatever the current directory is
        plat.write_text("../rel_pret\n")
        rc, out = run(get, only, cwd=tmp)
        check(rc == 0 and out == str(repo.parent / "rel_pret"), f"relative .plat_ref: rc {rc} {out!r}")
        rc, out = run(sync, cwd=tmp)
        check(rc == 1 and f"pret checkout not found: {repo}/../rel_pret " in out, f"sync_port relative .plat_ref: rc {rc} {out!r}")

        # CR line ends and a second line are ignored
        plat.write_bytes(f"{outside}\r\nsecond line\r\n".encode())
        rc, out = run(get, only)
        check(rc == 0 and out == str(outside), f"CRLF + second line: rc {rc} {out!r}")
        rc, out = run(sync)
        check(f"pret checkout not found: {outside} " in out, f"sync_port CRLF: {out!r}")

        # a blank file falls back to the default
        plat.write_text("\n")
        rc, out = run(get, only)
        check(rc == 0 and out == str(repo.parent / "pokeplatinum"), f"blank .plat_ref: rc {rc} {out!r}")

        # env PLAT_REF wins over the file, and the argument wins over both
        plat.write_text(f"{outside}\n")
        envdir = tmp / "from_env"
        rc, out = run(get, {**only, "PLAT_REF": str(envdir)})
        check(rc == 0 and out == str(envdir), f"env over file: rc {rc} {out!r}")
        rc, out = run(sync, {"PLAT_REF": str(envdir)})
        check(f"pret checkout not found: {envdir} " in out, f"sync_port env over file: {out!r}")
        argdir = tmp / "from_arg"
        rc, out = run(get + [str(argdir)], {**only, "PLAT_REF": str(envdir)})
        check(rc == 0 and out == str(argdir), f"argument over env: rc {rc} {out!r}")

        # the inside-repo guard: refuses, also for a DEST that does not exist yet, however it is spelled
        for label, spelled in (("absolute .plat_ref inside", None), ("env inside", str(repo / "pokeplatinum")),
                               ("env inside via ..", str(repo / "x" / ".." / "pret")), ("env the repo itself", str(repo))):
            if spelled is None:
                plat.write_text(f"{repo}/pret_here\n")
                rc, out = run(get, only)
            else:
                rc, out = run(get, {**only, "PLAT_REF": spelled})
            check(rc == 1 and "refusing to put pret's tree inside this repo" in out, f"guard, {label}: rc {rc} {out!r}")
        plat.write_text("pret_here\n")                       # relative, lands inside the repo
        rc, out = run(get, only)
        check(rc == 1 and "refusing" in out, f"guard, relative .plat_ref inside: rc {rc} {out!r}")
        # a sibling whose name merely starts with the repo's name is outside
        rc, out = run(get, {**only, "PLAT_REF": str(repo) + "2/pret"})
        check(rc == 0, f"guard, sibling named repo2: rc {rc} {out!r}")
        # a symlink that points into the repo is inside
        link = tmp / "link_to_repo"
        link.symlink_to(repo)
        rc, out = run(get, {**only, "PLAT_REF": str(link / "pret")})
        check(rc == 1 and "refusing" in out, f"guard, symlink into the repo: rc {rc} {out!r}")

        # the Python resolvers agree with the shell ones
        for py in ("check_edit_text", "header_manifest", "mkedit"):
            default_ref = load_default_ref(ROOT / "tools" / f"{py}.py")
            saved = os.environ.pop("PLAT_REF", None)
            try:
                plat.write_text(f"{outside}\n")
                check(default_ref(repo) == outside, f"{py}.default_ref absolute")
                plat.write_text("../rel_pret\n")
                check(default_ref(repo) == repo / "../rel_pret", f"{py}.default_ref relative")
                plat.write_bytes(f"{outside}\r\nsecond\r\n".encode())
                check(default_ref(repo) == outside, f"{py}.default_ref CRLF + second line")
                plat.write_text("\n")
                check(default_ref(repo) == repo.parent / "pokeplatinum", f"{py}.default_ref blank")
                plat.write_text(f"{outside}\n")
                os.environ["PLAT_REF"] = str(envdir)
                check(default_ref(repo) == envdir, f"{py}.default_ref env first")
            finally:
                os.environ.pop("PLAT_REF", None)
                if saved is not None:
                    os.environ["PLAT_REF"] = saved

    # port.mk reads the first line only
    mk = (ROOT / "tools" / "port.mk").read_text()
    check(re.search(r"^PLAT_REF \?= .*head -n 1 \.plat_ref", mk, re.M) is not None, "port.mk reads the first line of .plat_ref")
    print(f"plat_ref_test: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
