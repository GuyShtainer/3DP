# 3DP v0.1.0: first public source release

3DP is a native port of Pokémon Platinum to the New Nintendo 3DS, built from the community pret
decompilation. It is not an emulator: the game's own code runs on the 3DS, on top of a thin layer
of our code that stands in for the DS hardware. What it adds:

- a **widescreen top screen** that shows more of the world (400 wide instead of 256, nothing stretched);
- **real stereo 3D** in the overworld, using the 3D slider.

**This release is source code only.** There is no `.cia`, no `.3dsx` and no ROM attached, and there
never will be. You build it yourself, and the game runs from a dump of your own cartridge.

![The first-launch setup screen](https://raw.githubusercontent.com/GuyShtainer/3DP/v0.1.0/media/setup-screen.png)

## Before you start

- **You need your own game.** That means a dump of your own Pokémon Platinum (US) cartridge. 3DP
  does not include one, does not download one, and does not say where to get one. See
  ["Your game file" in the README](https://github.com/GuyShtainer/3DP/blob/v0.1.0/README.md#your-game-file).
- **You need a New 3DS or New 2DS XL** with custom firmware, so you can install a homebrew `.cia`.
  Old 3DS is best effort only.
- **Back up your save first.** 3DP writes only its own copy of your save, in `sdmc:/3ds/3DP/`; the
  save file you import is only read. This is early software: keep your own copy anyway.

## How to use it

1. Build 3DP: follow [BUILDING.md](https://github.com/GuyShtainer/3DP/blob/v0.1.0/BUILDING.md). In short: `tools/get_pret.sh`, build pret,
   `tools/sync_port.sh`, `make cia`.
2. Install `3DP.cia` with FBI or a similar title manager.
3. Copy the dump of your cartridge (a `.nds` file) anywhere on the SD card.
4. Start 3DP. The setup screen opens: pick your game file, then import your save or start a new
   game. Hold SELECT while 3DP starts to open the setup screen again later.
5. **Turn on widescreen and 3D.** They are off by default in this release: the game starts in the
   DS layout. After the first launch, open `sdmc:/3ds/3DP/config.ini` and add these two lines at the
   top of the file:

   ```ini
   wide = 1
   stereo = 1
   ```

   Then start 3DP again and use the 3D slider.

Everything 3DP writes lives in `sdmc:/3ds/3DP/`: the config, the logs and its save.

## What works

Measured in Azahar (a desktop 3DS player we test with): boot, the title screen, Continue, the
overworld in widescreen and stereo 3D, menus drawn over the 3D world, and saving.

On a real New 3DS (one session so far): the 3D overworld works well, the menu sits on top of the
world, and touch works.

## Known problems

This is an early release. From that one hardware session:

- It is **very slow**: it took several minutes to get into the game.
- There is **no sound**.
- Interiors (the Pokémon Center) and the party screen are flat, with no stereo.
- The bottom screen uses only the small DS-sized area.

Also: it does not run at 60 fps yet, there are known pixel differences from the real game, battles
and the full story are untested, and the clock, sleep, microphone, Underground, contests, Battle
Frontier and Pal Park are not done. Wi-Fi Connection, Download Play and the GBA slot are out of
scope. The README lists everything.

## What's next

In this order:

1. **Speed.** Get into the game in seconds rather than minutes, then aim for a steady 60 fps.
2. **Sound.** Music, cries and menu sounds.
3. **Every pixel.** Close the remaining differences from the real game's picture.
4. **System.** Clock and day/night, sleep, microphone, and the full bottom screen.
5. **Every mode.** Battles, the Pokétch, the Underground, contests, the Battle Frontier, Pal Park.
6. **Wide and deep.** Widescreen and stereo for interiors, menus and the 2D effects.

## Legal

- 3DP is a fan project. It is not affiliated with or endorsed by Nintendo, Game Freak, Creatures
  or The Pokémon Company. Pokémon and all related names and trademarks belong to their owners.
- This repository contains no Nintendo or Game Freak code, art, text or audio beyond the function,
  type and file names (and their declarations) that our changes have to match. The game's data
  comes from a dump of your own cartridge.
- No builds are distributed. Please don't share yours; everyone builds their own.
- The software comes with no warranty (GPLv3, sections 15 and 16). You use it at your own risk,
  including the risk to your save.
