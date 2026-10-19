# Building 3DP

3DP is built from three inputs that you put together yourself:

1. this repository (our code and our edit scripts),
2. a checkout of the [pret/pokeplatinum](https://github.com/pret/pokeplatinum) decompilation, built once with pret's own tools,
3. a dump of your own Pokemon Platinum cartridge (US), only to *run* the game.

This repository contains no Nintendo or Game Freak code, art, text or audio beyond the function, type and file names (and their
declarations) that our changes have to match. The build copies pret's tree into `port/`, applies our edits
(`port-edits/`) and compiles the result for the 3DS. Nothing of pret's is ever written into the tracked files.

## 1. Tools

| Tool | Version this tree builds with |
|---|---|
| devkitARM (GCC) | 15.2.0 (`devkitarm-gcc 15.2.0-7`, package `devkitARM r67.1-1`) |
| libctru | 2.7.0-1 |
| citro2d | 1.7.0-1 |
| citro3d | 1.7.1-2 |
| picasso, 3dstools (smdhtool, 3dsxtool, bin2s) | picasso 2.7.2-3, 3dstools 1.3.1-3 |
| python3 | 3.14 is what this tree was tested with; a recent 3.x should do |
| rsync, a C compiler for the host (`cc`), git, make | any recent version |
| makerom, bannertool | only for `make cia` (section 5) |

Install devkitPro with its package manager, then:

```sh
sudo dkp-pacman -S 3ds-dev           # devkitARM, libctru, citro2d, citro3d, picasso, 3dstools
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
```

To compare your install with the one this tree was built on, run:

```sh
dkp-pacman -Q devkitARM devkitarm-gcc libctru citro2d citro3d picasso 3dstools
```

and expect `devkitARM r67.1-1`, `devkitarm-gcc 15.2.0-7`, `libctru 2.7.0-1`, `citro2d 1.7.0-1`, `citro3d 1.7.1-2`, `picasso 2.7.2-3`,
`3dstools 1.3.1-3`. Other versions may work; only these are known to.

macOS: install the Xcode command line tools (`xcode-select --install`) for `cc`, `make` and `git`; `rsync` ships with the system.
Linux: install `build-essential`, `rsync`, `git` and `python3` from your distribution. Windows is not tested; use WSL2 and follow the Linux notes.
Everything here, the host tests included, was built and run on macOS (Apple silicon) only. Linux is untested, and the host tests in particular use
Apple-only linker flags today, so expect `make host-tests` to fail to link on Linux.

You also need what pret itself needs (section 2): on macOS that is Homebrew's `gcc@14 ninja libpng pkg-config arm-none-eabi-gcc xz` and Wine.

## 2. Get and build pret

```sh
tools/get_pret.sh                    # clones pret/pokeplatinum next to this repo (../pokeplatinum) at the pinned commit
```

The script checks out commit `c248fb3f8cc9934ded800e489567c5c0eeee92eb`, prints pret's own build instructions, and then refuses to go on until
the pret checkout has the headers its build generates. Follow pret's `INSTALL.md`, then run `make` in its directory **with the default options**
(revision 1; do not pass `ROM_REVISION=0`). Run `tools/get_pret.sh` again to confirm.

pret's first build downloads its helper projects and compiles with Nintendo's compiler under Wine; it took about 11 minutes here. Things that went wrong
on a fresh macOS machine, and what fixed them:

- `exec: wine: not found`: the Wine Stable app is installed but `wine` is not on your `PATH`. Add `/Applications/Wine Stable.app/Contents/Resources/wine/bin`
  to `PATH` (or install Wine so that `wine` resolves).
- `CERTIFICATE_VERIFY_FAILED` while meson downloads a helper project: a Python from python.org has no certificates. Run its `Install Certificates.command`,
  or `export SSL_CERT_FILE=/etc/ssl/cert.pem`.
- `Dependency "libpng" not found`: devkitPro puts its own `pkg-config` first on `PATH`. Build pret in a shell where `/opt/homebrew/bin` comes first
  (or before you export `DEVKITPRO`).
- `die: FormatMessageA` on one file: the pret checkout is in a folder with a very long path, and Wine/Windows tools choke on long paths. Keep the
  checkout under a short path (for example `~/pokeplatinum`). Then point the tools at it with `PLAT_REF` (below).
- After a failed attempt, delete pret's `build/` folder and run `make` again.

If you keep pret somewhere else, tell the tools where:

```sh
export PLAT_REF=/path/to/pokeplatinum        # or write the path on one line into a file named .plat_ref in this directory
```

(`tools/get_pret.sh DIR` also accepts the directory as an argument.)

Both the path in `PLAT_REF` and the one in `.plat_ref` must stay the same for every step below, because `tools/sync_port.sh` and `make` read the same setting.

## 3. Sync and compile

```sh
tools/sync_port.sh                   # copies pret's tree into port/ and applies port-edits/
make port -k -j8                     # (optional) the compile census; every file is attempted
make -j8                             # compiles the port, links 3DP.elf and 3DP.3dsx
```

`tools/sync_port.sh` first compares the generated headers of your pret build with `tools/generated-headers.sha256` and refuses on any difference:
those headers come from pret's own build tools, one of which is pinned by a tag that can move. If it refuses, rebuild pret at the pinned commit with
the default options. It then refuses to apply an edit script to a file that is not byte-for-byte the one the script was written against.

`make` runs the default goal, `port-link`. The outputs are `3DP.elf`, `3DP.smdh` and `3DP.3dsx` in this directory. Two clean builds of the same tree
give the same `3DP.3dsx` (no build date or time is compiled in; `make no-build-stamp` checks that).

Checks that need no ROM and no emulator (run them after `tools/sync_port.sh`; `make host-tests` takes about 2.5 minutes):

```sh
make host-tests                      # the host-side tests of the hardware layer and the edit tools
make check-edit-text                 # the edit scripts carry none of pret's text
```

Without a ROM some of the tests print `SKIP: no ROM` or `no ROM, ... check skipped`, and the 3D tests say `real-dump checks skipped`. Those are
expected; the command still exits 0.

A full build is about 2 minutes (`make -j8`), and two clean builds of the same tree give the same `3DP.3dsx`. You can check yours:
`shasum -a 256 3DP.3dsx` gives `7df0a5a7ed00d696b359da3690f3c23dd6d9beb17be7c478ab6dcd2e848d0180` for this tree with the same devkitPro
packages as above (that is the `.3dsx` the v0.1.0 source builds; no binary is published). Any change to a source file changes it.

## 4. Install and run

The `.3dsx` runs from the Homebrew Launcher, but it cannot reliably claim the second CPU core of the New 3DS, so use the `.cia` (section 5).
Old 3DS models are best effort only.

The first time you start it, a setup screen opens if no game file is set up yet. You pick your game file (it looks through the SD card for `.nds` files) and then import an existing save or start a new one; holding SELECT at launch opens it again. Your original save file is only read. You can
skip the setup screen by hand: copy your dump to `sdmc:/3ds/3DP/platinum.nds`, or write its path in `config.ini` as `rom = sdmc:/path/to/your.nds`.

Everything the game reads and writes lives in `sdmc:/3ds/3DP/` on the SD card:

| File | What |
|---|---|
| `platinum.nds` | a dump of your own cartridge (read-only; the game reads it in place, nothing is copied) |
| `platinum.sav` | the 512 KB save. If it is missing the game starts with an empty save |
| `config.ini` | written with defaults on the first run |
| `hwlog.txt`, `boot.out` | logs of the last run |

Please keep a backup of any save you care about; this is early software.

## 5. The installable .cia

`make cia` needs two small tools that devkitPro does not ship:

- **makerom** from [3DSGuy/Project_CTR](https://github.com/3DSGuy/Project_CTR/releases),
- **bannertool** from a maintained fork such as [Epicpkmn11/bannertool](https://github.com/Epicpkmn11/bannertool/releases).

Put both (executable) into `tools/bin/` in this directory, or anywhere on your `PATH`. `tools/bin/` is searched first, and its contents are git-ignored.

```sh
make cia                             # 3DP.cia, with the New 3DS flags of app.rsf (804 MHz, L2 cache, core 2)
```

Install `3DP.cia` with FBI or a similar title manager. Please build it yourself and do not share it: everyone builds their own.
