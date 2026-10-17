# Port tools

All tools find their files through `ps2x_paths.py`; nothing needs editing per machine. Set:

| Variable | Meaning | Default |
|----------|---------|---------|
| `PS2X_PORT_ROOT` | the port's working tree: `GameFiles/`, `recomp/`, `testrun/` | `D:\Xbox Games Ports\WWE Crush Hour` |
| `PS2X_RECOMP_OUTPUT` | the generated game code | `<PS2X_PORT_ROOT>/recomp/output` |

The runtime sources are always this checkout.

## Building the game code

1. Generate `recomp/output` with `ps2_recomp port/recomp/config.toml` (run from `PS2X_PORT_ROOT`).
2. `python port/tools/apply_patches.py` applies every build patch in the right order (`--list` shows them). All
   patches are idempotent, so re-running is safe.
3. Dump and compile the VU1 microcode (`PS2X_VU_DUMP`, `vu1_recomp.py`).
4. Configure CMake with `-DPS2X_GAME_DIR=<recomp/output>` and build `ps2EntryRunner` (`lowbuild.ps1`). Shipping
   builds add `-DPS2X_ENABLE_DIAG=OFF -DPS2X_ENABLE_RUNTIME_LOGS=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF
   -DPS2X_ENABLE_IOP_RPC_TRACE=OFF` (or `-DPS2X_SHIPPING=ON`, which sets all of them).

## Layout

| Path | Contents |
|------|----------|
| `patch_*.py` | build patches for the generated code, run by `apply_patches.py` |
| `diag/` | opt-in diagnostic patches (`apply_patches.py --diag NAME`) and one-off debugging scripts (`tmp_*.py`) |
| `applied/` | one-time edits of the runtime sources, already part of this checkout; kept for reference, do not re-run |
| `*test.ps1`, `bench.ps1`, `matchbench.ps1` | hidden scripted test runs (`bgtest.ps1` drives the runner) |
| `regress.ps1` | runs the scripted tests and compares their screenshots with a saved baseline |
| `vu1_recomp.py`, `vu1_disasm.py` | VU1 microcode recompiler and disassembler |
| `make_*.py` | generators for the port's own art (`ps2xRuntime/src/lib/gs/*.inc`, `res/game.ico`) |
