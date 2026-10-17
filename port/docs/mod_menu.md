# In-game mod menu

`ps2xRuntime/src/lib/host/mod_menu.cpp`, hooks from `port/tools/patch_modmenu.py`, the "m" plank from
`port/tools/make_mod_button.py`. `PS2X_MODMENU=0` turns it all off.

## Opening it

- **Main menu:** an "m" plank sits right of "options" on the menu sign. RIGHT on "options" gives it the focus
  (highlighted), A opens the menu; LEFT / B / Y gives the focus back, UP / DOWN go back and move the game's
  highlight on. Closing the menu returns the focus to the plank.
- **Anywhere** (also in a match): F2, or BACK + RB on a controller.
- While the plank or the menu has the input, player 1's pad (and the keyboard) reports nothing to the game;
  buttons held through such a change stay masked until released (no stray "Return" on the main menu).

## Settings page

LB / RB switch the panel between MODS and SETTINGS. The settings page changes the host settings live and writes them
back to `ps2x_settings.ini` (`ps2host::setSetting`): master / effects / commentary / movie volume (LEFT / RIGHT in
steps of 10, A steps up and wraps), fullscreen, VSync, the FPS counter, and the frame rate (30 / 60) and widescreen,
which apply on the next start. The settings file is found in the working directory, then next to the executable
and in its `GameFiles` folder; `Mods/` is the folder next to it.

## Main menu state

`Shell::Update` (0x1d7bd0) calls `ps2x_modmenu_shell(rdram, this)` first: Shell + 0x109C is the active menu (0 =
main menu: exhibition, new season, load season, multiplayer, options; item counts per menu at 0x3dd350), +0x109D the
highlighted item, +0x109F the previous one. The game highlights an item by swapping its material's texture
(`set_menuselect_texture`: materials named `Menu<m><i>0` get `*(Shell + 0xF18 + m*56 + i*8)`, the previous item gets
`*(Shell + 0xF14 + ...)` back). While the plank has the focus the native renderer draws "options" with its normal
texture (`nrw::setTextureRemap` in `nodeObject`). The UP / DOWN / button edges of the menu are at Shell + 0x428 /
0x438 / ...; multiplayer (item 3) is skipped while no second pad is connected (`PcplController::Status`).

## The "m" plank

The sign is one 512x512 texture (`Menu` board) with the five planks painted in. The tool builds a sixth plank from
it: the right end of "options" (mirrored for the left end), a text-free strip of its body stretched in between, the
"m" of "multiplayer" with its outline, scaled to 88 % and placed at x 397..489, y 403..471. The highlighted variant
recolours the frame to the game's highlight blue and the letter lavender. The backend applies the variant's
rectangle when it decodes a 512x512 texture whose rows 403..471 / columns 112..391 hash to the sign
(`applyTexturePatches`), and keys 512x512 textures by the variant so the change re-decodes it.

Note: the backend's `fnv1a` seed is 1469598103934665603 (the standard offset basis without its last digit); tools
that compute texture ids must use the same seed.

## Mods

- **Built-in** (the game's cheats): Unlimited Turbo (`Player::muCheatFlags` 0x4A2298 bit 0: `Player::Update` keeps
  the turbo count at 6), Power Hungry (bit 1: `Player::IncPower` x5 for human players), Unlock Everything
  (`WWFOptions::Instance()` = *(0x4A57F8), +0x558: the unlock-everything cheat flag read by the arena, superstar
  and match type choosers). The game clears the cheat flags when a match starts, so they are set every frame.
- **pnach files** in `GameFiles/Mods/`: each `[Section]` is a mod (PCSX2 2.x layout; lines before the first section
  form one named after the file; `description=`, `author=`, `comment=`). `patch=<0|1>,EE,<addr>,<type>,<value>`
  with byte / short / word / double / be* / extended (0/1/2 writes, 4 serial writes, D and E 16-bit conditions).
  patch=0 lines run when the mod is turned on, patch=1 lines every frame. The values a mod's writes replaced are
  kept and written back when it is turned off. Code patches have no effect (the game runs as recompiled code).
- **Live:** `ps2x_mods_frame` runs at the entry of the game's vsync wait (`ps2_wait_for_vsync`, 0x1771a0) on the
  EE thread, once per game frame in menus and matches; the menu (host thread) only flips `enabled` under a mutex.
  `Mods/enabled.txt` keeps the ids that are on (`builtin:<name>` or `<file>|<section>`); the folder is rescanned
  each time the menu opens.

## Panel

Drawn with GDI into a DIB (Impact title, Arial rows; dark steel panel, the game's highlight blue), premultiplied
BGRA, re-rendered only when something changed, and drawn by the host after the frame (and the FPS counter) with a
premultiplied blend, centred, scaled with the window height (1080p = 900 px wide).

## Restored cut content

Built-in mods with hooks from `port/tools/patch_cutcontent.py`.

### Ladder arena

The disc holds a finished arena the game never offers: `BUNDLES\LEVELS\K-Z\LADDER.BND/.BNT` (`levels\ladder`: BSP,
physics, ramp and door props with animations, intro cameras, pickups and weapons, AI road / path maps, `init.lua`),
its sound bank `AUDIO\BANKS\K-Z\LADDER.VBI/.VBK`, the shell's match list entry `MATCH LADDER` (Ladder Match,
`LEVEL=ladder`, `GAMEPLAY=ladder`, all players) and the game code for it (`RefereeLadder`, `BrainLadder`,
`CELadder`, `VICTORY_LADDER`). The shell's asset list puts `ladder` among the finished assets. Only the arena
chooser lacks it: the chooser's names come from the shell script (`LEVELS1..3`, 13 names) matched against
`LevelSelection[14][17]` (0x3DD570), whose 14th slot is empty.

The chooser's tables are sized for 14 arenas (Shell + 0x113E: level per chooser slot, + 0xFBC: tile texture per
slot, + 0x114C: count). With the mod on:

- `LevelSelection[13]` = "ladder" (the name `OnLevelSelection_Tag` / `OnSetGameLevel_Tag` copy into Shell + 0x110C
  and `gLevelInitData`); the level loads by that name (bundle `LADDER`, `levels\ladder\`).
- chooser slot 13 is appended (`ps2x_levelsel_pick`, after the script's three `InitLevelSelect` calls); its tile
  texture is the Survivor Series tile's (with a reference of its own: `Shell::Shutdown` destroys every slot's
  texture). The script wraps `LEVEL_SELECT` at `LEVEL_MAX` = 13, so the hooks keep a virtual selection: a step
  right (+1, or 12 -> 0) or left of the script's value moves it one of 14 slots; `OnSetGameLevel_Tag` with the
  value last shown picks that slot. A new chooser (`OnInitLevelSelect_Tag` with count 0) resets it.
- unlocked: `WWFOptions + 0x24` bit 13 is set for the test in `OnLevelSelection_Tag` (`ps2x_levelsel_unlock`),
  the options themselves are not changed.
- tile: `port/tools/make_ladder_tile.py` builds it from the game's tiles (common frame and logo, a screenshot of
  the arena, the label "Ladder" from the game's own letters); the backend paints it over the Survivor Series
  texture while the chooser shows slot 13 (`ps2x_ladder_tile_state`; 512x256 textures are keyed by it).
- load screen: `InBetween_set_level_texture("ladder")` would load `LOAD_LAD.BNT`, which does not exist; the hook
  gives it the generic one (`gen`, texture "loading").
- match types: the match-type chooser lists matches whose `LEVEL` is the picked level or `ALL`: Ladder Match,
  Free for All, Tag Team and Demolition Derby.

### Faarooq

`BUNDLES\VEHICLES\A-J\FAROOQ.BND` is a complete vehicle (`farooq_car.dff` with `Ron_Simmons` driving, weapons,
effects, tuning, `taunt_face`, `FarooqCommentaryNames` with five Jim Ross name calls that are on the disc in
`IOPDATA\COMM_JR`), but Faarooq has no `EWrestler` entry, no shell wheel / belt bundle and no intro, victory or
taunt videos. With the mod on, `VehicleLoader::LoadVehicle` (all vehicle loads: match start, Royal Rumble /
Battle Royal entrants) gets "farooq" for "bradshaw" (his APA partner; `PS2X_FAAROOQ_FOR=<name>` picks another
superstar for tests), and the superstars' display name "Bradshaw" (0x3EBCF8) reads "Faarooq". The shell's wheel
still shows Bradshaw's car, and his videos play.
