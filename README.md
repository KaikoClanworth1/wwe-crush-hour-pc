# WWE Crush Hour — native PC port

Unofficial native Windows port of **WWE Crush Hour** (PlayStation 2, SLUS-203.85), built by static
recompilation: the game's EE program and its VU1 microcode are compiled ahead of time to x64, and the
RenderWare graphics are drawn natively with Direct3D 11 from the game's own data. Nothing is
interpreted at run time (the IOP side is high-level emulation). **Bring your own disc** — the repository holds no game files (see *Not included*).

The engine is a fork of [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) (its README:
[README.PS2Recomp.md](README.PS2Recomp.md)); the port's own files are in [`port/`](port/).

## Features

- 60 fps (TargetFPS=30 for the original pacing), menus and matches steady
- True widescreen: wider 3D view, HUD / menus / name tags kept in proportion, 2D screens pillarboxed
- Up to 4K: the internal render scale follows the window / screen size (1080p 3x, 1440p 4x, 4K 6x)
- Native RenderWare renderer: world (incl. the live arena screens), cars, reflections, skinned drivers,
  particles, the crowd, HUD and menus; split screen verified against the original VU programs
- Xbox controllers (XInput) with Xbox button prompts (glyphs from WWE SmackDown vs Raw 2011)
- 2-player split screen, keyboard support, FPS counter (F1), borderless fullscreen (Alt+Enter)

## Layout

| Path | Contents |
|------|----------|
| `ps2xRecomp/`, `ps2xAnalyzer/` | the recompiler (R5900 -> C++) and ELF analyzer |
| `ps2xRuntime/` | runtime: memory, kernel / IOP HLE, VIF / GIF / GS, the D3D11 GS backend, the native RenderWare path (`src/lib/gs/native_rw.cpp`), the Win32 host |
| `port/recomp/config.toml` | recompiler configuration for SLUS_203.85 (stubs, skips, patches) |
| `port/tools/` | patches applied to the generated code (`patch_*.py`), the VU1 recompiler (`vu1_recomp.py`), build (`lowbuild.ps1`) and hidden test scripts (`bgtest.ps1`, `drivetest.ps1`, `menutest.ps1`, `mptest.ps1`) |
| `port/docs/` | technical notes: native RenderWare renderer, performance, sound engine protocol |
| `port/release/` | the player README, launcher and default settings shipped with a build |

## Building (outline)

1. Extract `SLUS_203.85` and the disc contents from your own copy of the game into `GameFiles/`.
2. Run the recompiler with `port/recomp/config.toml` to generate `recomp/output/`, then apply the
   `port/tools/patch_*.py` scripts to the generated files.
3. Dump the game's VU1 microcode (`PS2X_VU_DUMP`) and compile it with `port/tools/vu1_recomp.py`.
4. Configure with CMake (`out/build`) and build the `ps2EntryRunner` target (`port/tools/lowbuild.ps1`).

The tools currently use the absolute paths of the development machine (`D:\Xbox Games Ports\WWE Crush Hour`).

## Not included

The disc image, the game's files, the generated game code (`recomp/output/`), the VU1 microcode dumps and
compiled microcode, symbol / function lists taken from the game, and build outputs.

Two small tracked files are derived from game art and must be regenerated or removed before this repository
is made public: `ps2xRuntime/src/lib/gs/xbox_button_icons.inc` (button-glyph texture patches built from Crush
Hour's and SmackDown vs Raw 2011's textures by `port/tools/make_xbox_icons.py`) and `ps2xRuntime/res/game.ico`
(the window icon, from the game's memory-card icon, `port/tools/make_icon.py`).
