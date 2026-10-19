# 3DP

3DP is a native port of Pokémon Platinum to the New Nintendo 3DS. It is not an emulator: the game's
own code, built from the community decompilation, runs directly on the 3DS, with a thin layer of
our code under it that stands in for the DS hardware.

Two things make it different from playing the game on a DS:

- **Widescreen that shows more world.** The top screen is 400 wide instead of 256. The overworld
  camera uses the extra width to show more of the map. Nothing is stretched.
- **Real stereo 3D.** The overworld is true 3D geometry, so the 3DS 3D slider gives it real depth.

Version: **v0.1.0**. This is an early, rough release of the source.

## What this repository contains

This repository contains no Nintendo or Game Freak code, art, text or audio beyond the function, type and
file names (and their declarations) that our changes have to match. The build applies our changes to the
pret decompilation you fetch yourself, and the game reads its data from a dump of your own cartridge.

The hardware layer is written from public hardware documentation (GBATEK) and documented reverse
engineering; the edits were written against the pret decompilation.

You bring two things: the pret decompilation (which you fetch and build yourself) and a dump of
your own Pokémon Platinum (US) cartridge. There are no downloads here: no ROM, no prebuilt `.cia`,
no release builds.

3DP is not affiliated with or endorsed by Nintendo, Game Freak, Creatures or The Pokémon Company.
Pokémon and all related names and trademarks belong to their owners.

## What works

Measured in Azahar (a desktop 3DS player we test with), on the build from this source:

- boot, then the title screen, then Continue;
- the overworld, in widescreen and in stereo 3D;
- the menus, drawn over the 3D world;
- save write-back (the game saves to the card image and the port writes it to your SD).

On a real New 3DS, one session so far: the 3D overworld works well, the menu sits on top of the
world, and touch works. (Touch was seen only there, not measured in Azahar.)

## Not tested, not done

Be warned: this is early. Here is everything we know is missing or unproven.

What the one real-hardware session showed, in the words of the person who ran it:

- "VERY VERY VERY VERY SLOW, took several minutes to be able to enter the game."
- "no sound at all."
- Inside the Pokémon Center the picture is flat, with no stereo. The party screen is flat too.
- The bottom screen uses only the small DS-sized area, not the whole 320x240.

Also:

- It is **one real-hardware session so far**. Nothing here is proven on hardware beyond those notes.
- It does not run at 60 fps yet.
- There are known pixel gaps against the real game's picture ("Every pixel" in the plans below).
- The real-time clock, sleep and the microphone are not done ("System" below).
- The Underground, contests, the Battle Frontier and Pal Park are not done ("Every mode" below).
- Wide versions of the 2D effects (weather, cave darkness and so on) are not done ("Wide and deep" below).
- Battles and the full story are untested.
- Old 3DS is best effort only. The target is the New 3DS and New 2DS XL.
- The `.3dsx` cannot claim the second CPU core; use the `.cia`.
- Wi-Fi Connection, Download Play and the GBA slot are out of scope.
- **Keep a backup of your save.** The port writes your save file. Copy it somewhere safe first.

## Plans

In plain words, in the order we want to work:

1. **Speed first.** Get into the game much faster than today, then aim for a steady 60 fps.
2. **Sound.** Music, cries and menu sounds.
3. **Every pixel.** Close the remaining differences between our picture and the real game's.
4. **System.** Clock and day/night, sleep, microphone, and better use of the bottom screen.
5. **Every mode.** Battles, the Pokétch, the Underground, contests, the Battle Frontier, Pal Park.
6. **Wide and deep.** Widescreen and stereo for the 2D effects, interiors and menus too.

## Build and use

Building is described in [BUILDING.md](BUILDING.md). In short: fetch and build the pret
decompilation, run `tools/get_pret.sh` (it fetches pret at the right commit), then `tools/sync_port.sh`
(it copies pret's tree into `port/` and applies our changes), then `make` (a `.3dsx`) or `make cia`
(an installable `.cia`).

- **Install the `.cia`.** The `.3dsx` from the Homebrew Launcher cannot reliably claim the second
  core, which the port needs. The `.cia` is the real target.
- **Target:** New 3DS and New 2DS XL. Old 3DS is best effort.
- **Files live in `sdmc:/3ds/3DP/`** on the SD card: the config, the logs and your save.
- **First launch.** If no game is set up yet, a setup screen opens. Pick your game file (a dump of
  your own Pokémon Platinum (US) cartridge), then import an existing save or start a new one. Your
  original save file is only read, never written. Holding SELECT at launch opens the setup screen
  again.

## Sharing

Please don't share builds; everyone builds their own. This is a request, not a licence term.

## Licence

The files in this repository are under the GNU General Public License v3 (see [LICENSE](LICENSE)).
That covers only the files in this repository. It does not cover the pret decompilation or
anything in it, and it does not cover your dump.

## Credits

- [pret](https://github.com/pret/pokeplatinum), whose Pokémon Platinum decompilation this builds on.
- GBATEK (Martin Korth), the public DS hardware documentation the hardware layer is written from.
- [devkitPro](https://devkitpro.org/), libctru, citro2d and citro3d, the 3DS toolchain and libraries.
- makerom (Project_CTR) and bannertool, which package the `.cia`.

Enjoy :)
