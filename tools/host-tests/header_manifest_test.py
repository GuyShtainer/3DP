#!/usr/bin/env python3
"""header_manifest_test.py -- tools/header_manifest.py include-guard masking (release RF3).

usage: python3 -I tools/host-tests/header_manifest_test.py      (part of `make host-tests`; needs nothing from pret)

pret derives a generated header's include guard from the ABSOLUTE path of the checkout. The manifest must match at any path, and must
still fail on a real change. Prints "header_manifest_test: N checks, F failures"; exit 1 on any failure.
"""
from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("header_manifest", HERE.parent / "header_manifest.py")
HM = importlib.util.module_from_spec(spec)
sys.modules["header_manifest"] = HM
spec.loader.exec_module(HM)

BODY = "\n#define ShopMenu_Template_ScrollArrow 0\n#define ShopMenu_Template_Cursor 1\n\n"


def header(guard: str, body: str = BODY, endif: str = "// ") -> bytes:
    return f"#ifndef {guard}\n#define {guard}\n{body}#endif {endif}{guard}\n".encode()


def build_ref(root: Path, name: str, data: bytes) -> Path:
    d = root / "build" / "res" / "graphics" / "sprite_templates"
    d.mkdir(parents=True, exist_ok=True)
    (d / name).write_bytes(data)
    return root


def main() -> int:
    checks = failures = 0

    def check(cond: bool, what: str) -> None:
        nonlocal checks, failures
        checks += 1
        if not cond:
            failures += 1
            print(f"FAIL: {what}")

    with tempfile.TemporaryDirectory() as tmp_s:
        tmp = Path(tmp_s)
        a = build_ref(tmp / "a", "shop_menu.h", header("_USERS_ME_PRET_BUILD_RES_GRAPHICS_SPRITE_TEMPLATES_SHOP_MENU_H"))
        b = build_ref(tmp / "b", "shop_menu.h", header("_PRIVATE_TMP_X_POKEPLATINUM_BUILD_RES_GRAPHICS_SPRITE_TEMPLATES_SHOP_MENU_H"))
        c = build_ref(tmp / "c", "shop_menu.h", header("_G", BODY.replace("Cursor 1", "Cursor 2")))
        d = build_ref(tmp / "d", "shop_menu.h", header("_USERS_ME_PRET_BUILD_RES_GRAPHICS_SPRITE_TEMPLATES_SHOP_MENU_H").replace(b"#endif // ", b"#endif /* ").rstrip(b"\n") + b" */\n")
        e = build_ref(tmp / "e", "shop_menu.h", header("_G").replace(b"#define _G\n", b"#define _OTHER\n"))
        ha, hb, hc, hd, he = (HM.collect(r) for r in (a, b, c, d, e))
        key = "res/graphics/sprite_templates/shop_menu.h"
        check(set(ha) == {key}, f"collect finds the header: {set(ha)}")
        check(ha == hb, "the same header under a different absolute path hashes the same")
        check(ha[key] != hc[key], "a changed body line (Cursor 1 -> 2) changes the hash")
        check(ha == hd, "the /* X */ closing comment form is masked too")
        check(he[key] != ha[key], "an #ifndef/#define pair that disagree is not masked")
        mf = tmp / "m.txt"
        check(HM.main(["write", "--ref", str(a), "--manifest", str(mf)]) == 0, "write")
        check(HM.main(["check", "--ref", str(b), "--manifest", str(mf)]) == 0, "check passes on the same header at another path")
        check(HM.main(["check", "--ref", str(c), "--manifest", str(mf)]) == 1, "check MISMATCHes on a real changed line")
    print(f"header_manifest_test: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
