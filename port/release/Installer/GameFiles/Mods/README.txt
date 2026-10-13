Mods folder
===========

The in-game mod menu lists the built-in mods and every cheat file in this folder.
Open it from the main menu: highlight "options", press RIGHT to the "m" plank, press A.
Anywhere else (also in a match): F2, or BACK + RB on a controller.
A or LEFT/RIGHT turns the highlighted mod on or off, B (or Y / START) closes the menu.
Changes are live - no restart. The mods you turn on are remembered in enabled.txt.

Built-in mods (the game's own cheats)
  Unlimited Turbo     your turbo stays full
  Power Hungry        your special power charges five times faster
  Unlock Everything   every superstar, arena and match type

Adding mods: PCSX2 cheat files (.pnach)
  Put .pnach files here. Each [Section] in a file is one mod in the menu
  (lines before the first section form a mod named after the file).

    gametitle=WWE Crush Hour [SLUS-20385]

    [Infinite something]
    author=you
    description=What it does (shown in the menu).
    patch=1,EE,204A2298,extended,00000001

  patch=0 lines are written once when the mod is turned on, patch=1 lines every frame.
  Types: byte, short, word, double, extended (0/1/2 = 8/16/32-bit write, 4 = serial
  write, D/E = 16-bit conditions). Turning a mod off puts back the values it replaced.

  This port runs the game as recompiled PC code, so cheats that change the game's
  MIPS instructions do nothing; cheats that change values in memory (most of them) work.
