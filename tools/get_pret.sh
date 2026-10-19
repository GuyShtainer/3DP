#!/bin/sh
# Fetch the pret decompilation this port is built on, at the commit our edits are written against, and check that it has been built.
#   usage: tools/get_pret.sh [DIR]        DIR defaults to $PLAT_REF, else the file .plat_ref, else ../pokeplatinum next to this repo
# It clones https://github.com/pret/pokeplatinum (override with PRET_URL) into DIR, checks out the pinned commit, and prints pret's own build
# instructions. It then REFUSES (exit 3) until DIR/build holds the generated headers, i.e. until you have built pret with its default options.
# Run it again after building. It writes nothing of pret's into this repo.
#
# Where the checkout is (ONE rule, shared with tools/sync_port.sh, tools/port.mk, tools/check_edit_text.py and tools/header_manifest.py):
#   1. the argument DIR, then $PLAT_REF if set and non-empty: used as given (a relative value is relative to the current directory);
#   2. else the FIRST line of the file .plat_ref in this repo: an ABSOLUTE path is used as is, a relative one is relative to this repo's root;
#   3. else ../pokeplatinum next to this repo.
# The inside-repo guard compares absolute, normalised paths (symlinks resolved as far as the path exists), so it also works while DIR does not
# exist yet, and it refuses any DIR inside this repo (exit 1). GET_PRET_CHECK_ONLY=1 stops after the guard and prints the resolved DIR (for tests).
set -eu
PIN=c248fb3f8cc9934ded800e489567c5c0eeee92eb
URL="${PRET_URL:-https://github.com/pret/pokeplatinum}"
HERE=$(cd "$(dirname "$0")/.." && pwd -P)
if [ $# -ge 1 ]; then DEST="$1"
elif [ -n "${PLAT_REF:-}" ]; then DEST="$PLAT_REF"
elif [ -s "$HERE/.plat_ref" ] && [ -n "$(head -n 1 "$HERE/.plat_ref" | tr -d '\r[:space:]')" ]; then
  DEST=$(head -n 1 "$HERE/.plat_ref" | tr -d '\r')
  case $DEST in /*) ;; *) DEST="$HERE/$DEST" ;; esac
else DEST="$HERE/../pokeplatinum"; fi
DEST=$(python3 -I -c 'import os, sys; print(os.path.realpath(os.path.abspath(sys.argv[1])))' "$DEST")
case "$DEST" in
  "$HERE"|"$HERE"/*) echo "get_pret: refusing to put pret's tree inside this repo ($DEST); pick a directory outside it" >&2; exit 1 ;;
esac
if [ -n "${GET_PRET_CHECK_ONLY:-}" ]; then echo "$DEST"; exit 0; fi

if [ -d "$DEST/.git" ]; then
  have=$(git -C "$DEST" rev-parse HEAD)
  [ "$have" = "$PIN" ] || { git -C "$DEST" cat-file -e "$PIN^{commit}" 2>/dev/null || git -C "$DEST" fetch origin "$PIN"; git -C "$DEST" checkout --quiet "$PIN"; }
elif [ -e "$DEST" ] && [ -n "$(ls -A "$DEST" 2>/dev/null)" ]; then
  echo "get_pret: $DEST exists and is not a git checkout; pick another directory" >&2; exit 1
else
  git clone "$URL" "$DEST"
  git -C "$DEST" checkout --quiet "$PIN"
fi
[ "$(git -C "$DEST" rev-parse HEAD)" = "$PIN" ] || { echo "get_pret: $DEST is not at the pinned commit $PIN" >&2; exit 1; }
echo "get_pret: pret/pokeplatinum at $PIN in $DEST"

if [ -d "$DEST/build/generated" ] && [ -d "$DEST/build/res" ] && [ -d "$DEST/build/subprojects/NitroSDK-4.2.30001/gen" ]; then
  echo "get_pret: $DEST/build has the generated headers."
  [ "$DEST" = "${PLAT_REF:-}" ] || echo "get_pret: next: export PLAT_REF=\"$DEST\"   (or put the path in a file named .plat_ref here), then tools/sync_port.sh"
  exit 0
fi

cat <<TXT

get_pret: $DEST/build has no generated headers yet. Build pret with its DEFAULT options first (revision 1, no ROM_REVISION=0):
  1. Install pret's build dependencies. Its own instructions are in $DEST/INSTALL.md (section 1 for your OS).
     macOS: xcode-select --install; brew install gcc@14 ninja libpng pkg-config arm-none-eabi-gcc xz; brew install --cask wine-stable
     Debian/Ubuntu: sudo apt install bison flex g++ gcc-arm-none-eabi git make ninja-build pkg-config wget python3 xz-utils nasm libpng-dev
  2. cd "$DEST" && make
     (its section 3; "make rom" skips the checksum. The compiler is Nintendo's, run under wine on macOS/Linux.
      This port only needs the headers and tables the build generates under build/.)
  3. Run tools/get_pret.sh again.
TXT
exit 3
