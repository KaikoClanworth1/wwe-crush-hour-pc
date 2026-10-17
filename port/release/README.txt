WWE Crush Hour - native PC port   alpha 0.0.1
=============================================

This is an early alpha. It contains NO game files: you need your own copy of
WWE Crush Hour for PlayStation 2 (USA, SLUS-20385) as an .iso image or disc.

Install:  double-click "Install WWE Crush Hour.bat" and pick your .iso (or cancel and pick the
          disc drive / a folder with the disc's files). The game data (about 1.1 GB) is copied
          into the GameFiles folder. Windows 10/11 mounts the .iso by itself.
Play:     double-click "Play WWE Crush Hour.bat"

Status
------
- Boots through the intro movies to the main menu. All five options (Exhibition, New Season,
  Load Season, Multiplayer, Options) show on the sign like on a PS2; Up/Down moves the highlight.
- Exhibition matches play: arena, your vehicle, opponents, HUD (portraits, turbo/special/health),
  countdown and explosions all render correctly. 2-player split-screen matches (Multiplayer) work too.
- 60 fps: menus and matches run at a steady 60 game fps. Game data (asset bundles, sounds) is read into
  memory at startup, so nothing is loaded from the disk mid-match (cold reads from a hard disk caused
  stutters). The arena screens and menu videos no longer drop the game to 30 fps while they play.
- No console window: the game runs as a normal Windows program.
- FPS counter in the top-left corner: FPS = frames shown per second, GAME = frames the game
  itself produced (orange when below 55). F1 hides/shows it (ShowFPS=0 in the settings turns it off).
- Everything runs as native PC code: the game's program and its PS2 vector-unit programs are compiled
  ahead of time to x64 (nothing is interpreted). Rendering is DirectX 11, drawn directly from the game's
  own data: the arenas (including the live video screens), cars, reflections, wrestlers/drivers, the HUD
  and menus, 2D screens, particles (smoke, fire, sparks, explosions) and the crowd in the stands. In the
  menus every object is drawn this way; in a match all but ~0.5% (objects showing a live camera picture,
  which still let the game place that picture first, then are drawn natively too). What still goes
  through the layer that turns PS2 drawing commands into DirectX: screen clears and a few rare
  untextured particle effects.
- Collision/physics fixed (cars no longer get stuck or fall through the arena at spawn), all arenas
  run at full speed, car reflections no longer show green garbage.
- Tested: Exhibition (RAW, SmackDown, Hell in a Cell, Hardcore), New Season, 2-player split screen,
  the pause menu and Options. Cars no longer drop through the floor in Hell in a Cell / Hardcore.
- Mod menu: on the main menu press RIGHT on "options" to reach the "m" plank and press A (or F2 /
  BACK + RB anywhere, also in a match). Mods turn on and off live: the game's own cheats (Unlimited
  Turbo, Power Hungry, Unlock Everything), "Faarooq" (the finished but unused Faarooq car, driven
  when you pick Bradshaw - experimental) and PCSX2 cheat files (.pnach) put in GameFiles\Mods
  (see the README there).
- If the picture ever stops moving, close and restart.
- The first time you boot, the memory card screen asks for a button press (A / X key).
  Skip the intro movies with Start / Enter.

Controls
--------
XInput controllers (Xbox pads) work directly, and the game's button prompts and the Controller
Options screen show Xbox buttons (A, B, X, Y, LB, RB, LT, RT, RS - the glyphs come from WWE
SmackDown vs Raw 2011). Multiplayer is 2-player split screen (the game only supports two
controllers): the first pad is player 1, the second pad is player 2.
Keyboard (acts as player 1):
  D-pad         Arrow keys          Left stick   W A S D
  A             X or Space          B            C or Backspace
  X             Z                   Y            V
  LB / RB       Q / E               LT / RT      1 / 3
  Start         Enter               Back         Tab or Right Shift
  Alt+Enter     toggle fullscreen   F1           FPS counter on/off
  F2            mod menu (BACK + RB on a controller)

Settings  (GameFiles\ps2x_settings.ini, or in game: mod menu, LB / RB to the SETTINGS page)
--------
  Width / Height   window size (default 1920x1080; up to 3840x2160)
  Fullscreen=1     start in borderless fullscreen at the monitor's resolution (4K on a 4K screen)
  VSync            1 = on
  TargetFPS=60     60 fps; 30 = the original pacing; 120 / 144 / 165 / 240 for high refresh displays;
                   0 = the monitor's refresh rate. VSync=0 + a G-Sync / FreeSync display: no tearing
  KeepAspect=1     undistorted picture (4:3 with side bars)
  Widescreen=1     true 16:9 (default): wider 3D view (more of the arena at the sides); HUD, text and menu
                   panels keep their proportions, name tags stay over the cars; movies and loading
                   screens are shown 4:3 with side bars. 0 = original 4:3
  ShowFPS=1        FPS counter on at start (F1 toggles)
  InternalScale=0  render resolution: 0 = automatic from the window / screen size (1080p -> 3x,
                   1440p -> 4x, 4K -> 6x the PS2's 640x448); 1..6 = fixed (1 = original sharpness)

  [Audio] MasterVolume / EffectsVolume / CommentaryVolume / MovieVolume   0..100 (percent)
  [Saves] Folder   where saves are kept (default: Saved Games\WWE Crush Hour)

Saves are stored in %USERPROFILE%\Saved Games\WWE Crush Hour\mc0\ (backed up by OneDrive / cloud sync
like other games). Saves from an older version in GameFiles\mc0\ are copied there on the first start;
the old folder is left as it was. Saving writes a copy and swaps it in, so a crash mid-save keeps the
previous save.
