#!/bin/sh
# Refresh port/ from your pret/pokeplatinum checkout (read-only: tools/get_pret.sh makes one), then apply port-edits/ (tools/apply_edits.py). Idempotent.
# PLAT_REF names the checkout. ONE rule, shared with tools/get_pret.sh, tools/port.mk, tools/check_edit_text.py and tools/header_manifest.py:
#   1. $PLAT_REF if set and non-empty: used as given (a relative value is relative to the current directory);
#   2. else the FIRST line of the file .plat_ref in this repo: an ABSOLUTE path is used as is, a relative one is relative to this repo's root;
#   3. else ../pokeplatinum next to this repo.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${PLAT_REF:-}" ]; then
  if [ -s "$HERE/.plat_ref" ] && [ -n "$(head -n 1 "$HERE/.plat_ref" | tr -d '\r[:space:]')" ]; then
    PLAT_REF=$(head -n 1 "$HERE/.plat_ref" | tr -d '\r')
    case $PLAT_REF in /*) ;; *) PLAT_REF="$HERE/$PLAT_REF" ;; esac
  else PLAT_REF="$HERE/../pokeplatinum"; fi
fi
REF="$PLAT_REF"
P="$HERE/port"
[ -d "$REF/src" ] || { echo "pret checkout not found: $REF (run tools/get_pret.sh, or set PLAT_REF)" >&2; exit 1; }
# F2 (release review): the reference must be the pinned pret commit with a clean tree (a partial clone or another revision would sync silently).
python3 -I "$HERE/tools/apply_edits.py" --check-ref "$REF" || exit 1
# F3: the generated headers come from pret's own build tools, one of them pinned by a movable tag. Refuse before copying anything if they differ
# from the committed manifest (tools/generated-headers.sha256: hashes only).
python3 -I "$HERE/tools/header_manifest.py" check --ref "$REF" || exit 1
mkdir -p "$P"
R="rsync -a --delete --exclude=*.a --exclude=*.o --exclude=*.nef --exclude=.git"
for d in src include asm platinum.us lib/spl lib/crypto lib/gds lib/ppwlobby/include; do
  mkdir -p "$P/$(dirname "$d")"; $R "$REF/$d/" "$P/$d/"
done
S=subprojects
for sub in NitroSDK-4.2.30001 NitroSystem-071126.1; do
  mkdir -p "$P/$S/$sub"
  $R "$REF/$S/$sub/include/" "$P/$S/$sub/include/"
  $R "$REF/$S/$sub/libraries/" "$P/$S/$sub/libraries/"
done
mkdir -p "$P/$S/NitroSDK-4.2.30001/gen"; $R "$REF/$S/NitroSDK-4.2.30001/gen/" "$P/$S/NitroSDK-4.2.30001/gen/"
for sub in NitroDWC-2.2.30008 NitroWiFi-2.1.30003 libvct-1.3.1; do   # include only
  mkdir -p "$P/$S/$sub"; $R "$REF/$S/$sub/include/" "$P/$S/$sub/include/"
done
# plus the one DWC C file the link trial needs (B64Encode, GameSpy's base64): libraries/gs/nonport.c
mkdir -p "$P/$S/NitroDWC-2.2.30008/libraries/gs"; cp "$REF/$S/NitroDWC-2.2.30008/libraries/gs/nonport.c" "$P/$S/NitroDWC-2.2.30008/libraries/gs/nonport.c"
# generated headers from Guy's matching build (.h/.naix/.inc only; meson wires these as -Igenerated -Ires/...)
mkdir -p "$P/build"
for d in generated res $S/NitroSDK-4.2.30001/gen; do
  mkdir -p "$P/build/$d"
  rsync -a --delete --include='*/' --include='*.h' --include='*.naix' --include='*.inc' --exclude='*' "$REF/build/$d/" "$P/build/$d/"
done
# the -Ires/... directories meson passes to the game's main.nef (taken from the matching build's compile_commands.json)
python3 - "$REF/build/compile_commands.json" > "$P/res_incdirs.txt" <<'PY' || { rm -f "$P/res_incdirs.txt"; exit 1; }
import json, sys
for e in json.load(open(sys.argv[1])):
    if e["file"] == "../src/main.c" and "mwccarm" in e["command"]:
        dirs = [t[2:] for t in e["command"].split() if t.startswith("-Ires/")]
        if dirs:
            print("\n".join(dirs))
            sys.exit(0)
sys.exit("sync_port: no '../src/main.c' entry with mwccarm and -Ires/ include directories in " + sys.argv[1]
         + " (was the pret build made with its own mwccarm wrapper?)")
PY
[ -s "$P/res_incdirs.txt" ] || { echo "sync_port: $P/res_incdirs.txt is empty" >&2; rm -f "$P/res_incdirs.txt"; exit 1; }
# our edits (port-edits/: .ed files against the pristine copies just synced, plus port-edits/new/ files; port-edits/README).
# The rsync above restores every pristine file first, so this is idempotent; apply_edits refuses (and writes nothing) on a mismatch.
python3 -I "$HERE/tools/apply_edits.py" --edits "$HERE/port-edits" --port "$P"
echo "port/ synced from $REF"
