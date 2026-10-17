# Emulation audit and de-emulation plan

A review of everything in the port that is still PS2 emulation, or close to it, and a plan for replacing it with
ordinary PC-game code. Snapshot of the code as of October 2026. File references are repository-relative.

"Emulated" here means the code still models PS2 hardware or Sony-library behaviour. The game's EE code itself is
already native x64 code (static recompilation); the issue is the PS2 machine that is still simulated around it.

---

## Part 1 — Audit

### Status at a glance

| Subsystem | State today | Hot path? | Verdict |
|-----------|-------------|-----------|---------|
| Game code (EE) | Recompiled C++ that still models MIPS registers in a context struct | always | Native code, but in an emulated shape |
| Guest memory | Flat 32 MB RDRAM, masked addressing, scratchpad and MMIO on a slow path | always | Emulated |
| EE kernel / threads | Emulated scheduler: cooperative guest threads, exception-based unwinding, synthetic cycle counter | always | Emulated |
| Timing | Emulated 60.0 Hz VBLANK, timers driven by synthetic cycles, three pacers stacked | always | Emulated |
| FPU | PS2 non-IEEE semantics (clamping, x/0, round-toward-zero) | always | Must stay, mostly |
| DMA / VIF1 / GIF | Interpreted; used to keep native draws in order | always | Emulated backbone |
| GS VRAM | Full 4 MB swizzled VRAM image is the source of truth for textures and render targets | always | Emulated |
| 3D geometry | Native D3D11 from RenderWare data (about 1.63M native vs 8,300 fallback objects per match) | always | **Native** |
| VU1 | Mostly bypassed; particles and crowd still run the recompiled microcode; interpreter as a fallback | partial | Partly emulated |
| 2D / HUD / Im2D | CPU copy of the VU program, drawn with GS-register state and VRAM textures | always | Half emulated |
| FMV | FFmpeg decode, then pushed back through guest RAM, GIF and VRAM | movies | Emulated delivery |
| Audio (SENG) | Native XAudio2 sound engine with pre-decoded VAG | always | **Native** |
| IOP | R3000 interpreter linked in but idle (all IRX modules handled natively) | no | Dead weight |
| SIF RPC | Still the transport between the game and the native sound engine | per frame | Emulated glue |
| Disc I/O | Made-up CD sector numbers mapped onto extracted files; about 1 GB preloaded into RAM | loads | Emulated API |
| Saves | Emulated libmc on a folder next to the exe, plus the PS2 memory-card screens | saves | Emulated API |
| Input | libpad / DualShock 2 state machine on top of XInput | always | Emulated API |
| Platform | Win32 + D3D11 + XAudio2 + XInput; ini + .bat launcher | always | Native but Windows-only |

### 1. Recompiled code shape (CPU, highest hidden cost)

- **Registers in memory.** `R5900Context` (`ps2xRuntime/include/ps2_runtime.h:61-160`) holds 32 × 128-bit GPRs, plus
  the FPU, VU0 and COP0 registers. Every generated function is `void f(uint8_t* rdram, R5900Context* ctx, PS2Runtime*)`
  (`ps2xRecomp/src/lib/function_emitter.cpp:86`). `rdram` is a `uint8_t*`, which may alias anything, so after each
  guest store the C++ compiler must reload registers from `ctx`. Guest registers almost never stay in host registers.
- **A `ctx->pc` store before every instruction** (`function_emitter.cpp:204`). The aliasing above means the compiler
  can't remove them.
- **Each function opens with a `switch (ctx->pc)` over its resume points** (`function_emitter.cpp:93-104`). When the
  scheduler takes over, the generated code returns all the way up the stack, and each caller is re-entered by pc
  (`ps2xRuntime/src/lib/Kernel/EeScheduler.cpp:355-371`).
- **Every JAL/JALR goes through `runtime->dispatchGuestBranch`** (`ps2xRuntime/src/lib/ps2_runtime.cpp:1808-2010`).
  On each call it does a branch-ring write, several getenv-backed debug checks, a checkpoint charge, a table
  lookup, and then an indirect call. None of this can be inlined across guest functions.
- **Every backward branch** calls `eeCheckpointDue()` out of line (`ps2xRecomp/src/lib/control_flow_emitter.cpp:175-185`).

### 2. Memory

- **RDRAM.** A flat 32 MB array. Every access masks the address and checks `Ps2IsSpecialAddress`
  (`ps2xRuntime/include/ps2_runtime_macros.h:150-330`, `ps2_address.h:62-71`).
- **Scratchpad (0x70000000).** It is treated as a "special" address, so every access takes the slow path:
  try/catch, `translateAddress`, `loadScalar` (`ps2xRuntime/src/lib/ps2_memory.cpp:908-953`). RenderWare and
  Havok use the scratchpad heavily.
- **MMIO.** Registers live in an `unordered_map` (`ps2_memory.h:380`). The DMAC model has chains, SPR DMA and an
  async worker (`ps2_memory.cpp:42-76, 1297, 1496-1610, 1835, 2750`). There are about 1,080 MMIO instruction sites
  (`port/recomp/config.toml:879-1960`).
- **48-entry TLB model** (`ps2_memory.cpp:510, 738-840`). Not on the hot path; dead weight.

### 3. EE kernel, threads, interrupts

- **Scheduler.** `EeScheduler` runs every guest thread on one host thread. It has 128 priority queues and a
  65,536-cycle time slice (`ps2xRuntime/include/runtime/ee_scheduler.h:283-290`).
- **Blocking.** Blocking calls throw `EeDispatcherTransfer` to unwind to the dispatcher (`EeScheduler.cpp:645, 934,
  1237, 1256, 1855`). C++ exceptions are costly on Win64.
- **Interrupts, alarms and callbacks.** Each is a `GuestInvocation` that copies a ~1.3 KB context. `dispatchIrq`
  allocates, sorts and builds a `std::function` for every event (`EeScheduler.cpp:1389-1452`).
- **Kernel calls.** Semaphores, event flags, alarms and INTC/DMAC handler tables are emulated
  (`ps2xRuntime/src/lib/Kernel/Syscalls/*`). About 390 Sony/libc functions are HLE stubs (`config.toml:23-411`).

### 4. Timing

- **VBLANK** is a host timer at exactly 16,667 µs (60.0 Hz, not 59.94) (`EeScheduler.cpp:37-38, 1940-2085`). It
  raises INTC handlers, the GS vsync callback and the CSR field toggle. The game's limiter (`ps2_wait_for_vsync`,
  `port/tools/patch_60fps.py`) spins on that counter.
- **EE timers T0-T3** count synthetic cycles (32 per back-edge, 8 per call), not wall time
  (`ps2_memory.cpp:569-651`, `ee_scheduler.h:288-289`). Havok's `hkPs2SystemClock` reads T1. COP0 `Count` never
  advances (`ps2xRecomp/src/lib/cop0_translator.cpp:46-47`).
- **Three pacers are stacked:** the emulated VBLANK, the host `TargetFPS` sleep loop
  (`ps2xRuntime/src/lib/host/win32_host.cpp:289-311`), and swap-chain vsync with a waitable object (`:634-643`).
  The result is a hard 60 fps cap and judder on 120/144/165 Hz screens.
- **Open question:** `performance.md` says game logic uses a "real-time stopwatch". Check whether that stopwatch
  is backed by the host clock or by T1. If it reads T1, game time follows loop counts, not real time.

### 5. FPU / MMI / VU0

- **FPU.** The EE thread runs with MXCSR set to round-toward-zero plus FTZ/DAZ (`EeScheduler.cpp:218`).
  FPU add/sub/mul clamp to ±FLT_MAX, and division returns ±FLT_MAX on x/0 (`ps2_runtime_macros.h:620-660`).
  Patches (`patch_fpu_*.py`, `patch_vftoi_sat.py`) add more PS2 semantics.
- **These semantics carry real weight.** Animation loops, Havok slab tests and MOPP trees break without them.
  They should stay globally and only be removed per function once a function is verified.
- **MMI.** The MMI 128-bit ops map almost 1:1 onto SSE, so they are effectively native already.

### 6. Graphics

Defaults are native: `PS2X_NATIVE_WORLD` (`ps2xRuntime/src/lib/gs/native_rw.cpp:757-765`), `PS2X_NRW_NODE`
(`:1128-1140`) and `PS2X_NRW_RASTERTEX` (`gs_d3d11_backend.cpp:1300-1311`) are all on unless set to `0`. The
emulated hardware is still the backbone, though:

- **DMA → VIF1 → GIF ordering (always on).**
  - Native draws are found as `MARK 0xC0DE` + `NOP idx` markers inside the interpreted VIF stream
    (`ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp:463-480`).
  - PATH3 masking releases one queued GIF packet per unmask (`:425-462`), a long-running source of texture
    corruption.
  - The GIF arbiter copies every packet (`gs/ps2_gif_arbiter.cpp:33-48`).
  - The GS frontend parses tags under a recursive mutex and assembles vertices one kick at a time
    (`gs_frontend.cpp:655-750, 1574-1695`).
  - RenderWare still builds VIF packets and texture-upload chains every frame.
- **The GS VRAM image is the source of truth.**
  - A 4 MB swizzled image (`gs_d3d11_backend.cpp:408`, `ps2_gs_memory.cpp`, `ps2_gs_psm*.h`).
  - "Native" raster textures are made by replaying RenderWare's upload DMA chain into a private emulated GS
    memory and then deswizzling (`gs_d3d11_backend.cpp:1315-1530`).
  - Only mip 0 is decoded (`MipLevels = 1`, `:803`; `SampleLevel(...,0)`, `:306`), and samplers are point or
    linear only (`:752-760`), so textures shimmer at 3–6× internal resolution.
  - Cache keys are hashed again on every draw.
- **Render targets are keyed by GS VRAM addresses** (FBP/FBW/PSM, `:1059-1148`).
  - Sampling from a render target is detected by address arithmetic (`:2708-2730`).
  - `syncRtToVram` (`:909-995`) maps a staging texture immediately, a full GPU stall, and re-swizzles pixels into
    VRAM whenever an upload, CLUT load or VRAM texture overlaps a dirty render target.
  - No MSAA anywhere (`SampleDesc.Count = 1`); resolution scaling is the only anti-aliasing.
  - The shaders reproduce the GS TFX path, 0–128 alpha, FTOI4 truncation and `noperspective` interpolation
    (`:212-345`).
- **VU1.**
  - Particles (PGen) and the crowd still run the statically recompiled microcode on a private VU memory
    (`native_rw.cpp:2352, 2885`).
  - Untextured particles, locked geometry, fogged ISO objects and spot lights stay on the full emulated path.
  - The VU1 interpreter (`ps2xRuntime/src/lib/vu/`) is the fallback for unknown programs
    (`vu1_native_glue.cpp:297-457`).
- **2D / Im2D / Im3D.**
  - These run a CPU copy of RenderWare's VU program, then draw through the GS-register path with VRAM textures
    (`native_rw.cpp:1787+, 1890, 2060`).
  - Widescreen is a 3/4 x-scale heuristic in the shader (`gs_d3d11_backend.cpp:264, 631-647, 2884-2895`).
  - Sky clears are about 40 GS sprite strips per frame.
- **FMV.**
  1. FFmpeg decodes the movie (native).
  2. The frame is written back into guest RAM in PS2 macroblock layout (`Kernel/Stubs/MPEG.cpp:1964-2010`).
  3. The game uploads it to VRAM over GIF.
  4. Full-screen movies take the CPU present path: every render target is read back, then uploaded again
     (`gs_d3d11_backend.cpp:3575-3594`).
  5. Playback is paced on emulated vsync (`MPEG.cpp:2675-2695`).
- **Fallback-only:** the GS software rasterizer (`gs_cpu_backend.cpp`, `PS2X_GS_CPU`) and libgraph HLE
  (`Kernel/Stubs/GS.cpp`).

### 7. IOP, audio, files, saves, input

- **IOP.** No IOP code runs by default: SENG.IRX goes to `NativeSeng`, and the pad, memory-card and libsd modules
  are reported as loaded (`ps2_runtime.cpp:933-975`). The R3000 interpreter is still linked
  (`ps2xRuntime/CMakeLists.txt:557`), still receives EE cycles (`EeScheduler.cpp:511`), and allocates 2 MB.
  - **Risk:** `NativeSeng::enabled()` turns false when there is **no audio device at boot**
    (`audio/native_seng.cpp:750-756`), and SENG.IRX then really runs on the barely tested interpreter.
- **SIF RPC.** This is the transport between the game and the native sound engine. Arguments are found with a
  heuristic register-or-stack guess (`plausiblePack`, `Kernel/Syscalls/RPC.cpp:371-445`) under a global mutex.
- **Audio** is native (XAudio2, VAG decoded when banks load). What remains:
  - a source voice is created and destroyed per sound (`native_seng.cpp:182, 297`);
  - commentary is decoded whole, on the EE thread, on every play, looked up by matching made-up sector numbers
    (`:361-420`);
  - no device-loss handling (no `OnCriticalError`);
  - no volume settings;
  - the `sceSdRemote` fake SPU2 clock advances only when polled (`Kernel/Stubs/Audio.cpp:77-195`);
  - the old generic backend (`ps2_audio.cpp`, `ps2_audio_vag.cpp`) still sniffs every `fioRead` for "VAGp"
    (`Kernel/Syscalls/FileIO.cpp:62-85, 129-153`).
- **Disc I/O.**
  - `sceCdSearchFile` hands each file an invented sector number (`Kernel/Stubs/CD.cpp:583-687`,
    `Helpers/Support.h:393-399`).
  - `sceCdRead` turns the number back into a path by scanning the whole map on every read
    (`Support.h:488-504`).
  - Reads are synchronous on the EE thread. That is why about 1 GB is preloaded into RAM at boot
    (`host_file_cache.cpp:23-27`).
- **Saves.**
  - Emulated libmc on top of `<exe dir>/mc0`; it always reports a formatted 8 MB card
    (`Kernel/Stubs/MemoryCard.cpp:41, 121-157`).
  - Files are opened with ANSI `std::fopen` (`:483`), so non-ASCII user paths break.
  - Writes are not atomic.
  - The folder is not writable under Program Files, and the PS2 memory-card screens remain.
- **Input.**
  - libpad's DualShock 2 state machine runs on top of XInput (`Kernel/Stubs/Pad.cpp`).
  - Pressure buttons are binary 0/0xFF (`:160-172`), so analog triggers are lost.
  - The deadzone is square (`win32_host.cpp:652-658`).
  - XInput is polled on all four slots every frame (`:1035`).
  - Keyboard keys are hard-coded (`:1094-1119`), with no rebinding and no DualSense/DS4/Switch support.
- **Platform.**
  - The host is Win32/D3D11/XAudio2/XInput only.
  - Settings are an ini read relative to the working directory (`ps2_runtime.cpp:1162`). That is why the .bat
    launcher must `cd` first.
  - Settings are display-only and read-only, with no in-game settings page.
  - The window title still says "PS2-Recomp" (`ps2xRuntime/src/main.cpp:216`).
  - Install is PowerShell `Mount-DiskImage` + robocopy.
  - PS2Recomp leftovers: `games_database.cpp`, `game_overrides.cpp`, `ps2_debug_panel.cpp`.
- **Build reproducibility.**
  - The `port/tools/patch_*.py` scripts regex-edit the generated `recomp/output` sources using hard-coded
    `D:\...` paths.
  - Several of them duplicate fixes already made in the recompiler.
  - A clean checkout can't reproduce the build.

### What realistically stays

A static recompilation can't drop these without rewriting the game:

- **Guest data layout.** Game structures are 32-bit-pointer layouts, so the guest address space stays, but it can
  become a fixed host base with no masking.
- **Guest thread semantics.** The game calls `WaitSema`, `SleepThread`, etc. These can move to fibers, but they
  can't become free-running OS threads: the game assumes single-core, priority-ordered execution.
- **PS2 float semantics,** except in verified hot leaf functions.

---

## Part 2 — Plan

Each step keeps the port's existing pattern: an env/ini switch with `off / on / verify`, the old path kept as a
fallback and reference until verify mode shows zero mismatches, then the old path is deleted. Phases are ordered
by value/risk and by dependency. Phases 1–3 are largely independent of each other.

### Phase 0 — Foundations (prerequisite for everything)

1. **Reproducible build.**
   - Move every `port/tools/patch_*.py` edit into the recompiler or into `config.toml` function overrides and
     hooks.
   - Replace the hard-coded `D:\` paths with a configurable game root.
   - Delete the one-off and diagnostic patches (`tmp_*.py`, `*log*.py`, `*trace*.py`).
   - Goal: regenerate `recomp/output` from a clean checkout plus a disc.
2. **CI regression harness.** Turn the hidden test scripts (`bgtest`, `drivetest`, `menutest`, `mptest`) into a
   repeatable smoke test with screenshot and RAM-snapshot comparison. Every later phase depends on catching
   regressions automatically.
3. **Release hygiene.**
   - Compile debug hooks out of hot paths (`dispatchGuestBranch`, the scheduler loop, `checkpointDue`) behind
     `#if PS2X_DIAG`.
   - Turn the `PS2X_ENABLE_RUNTIME_LOGS` / `AGRESSIVE_LOGS` / `IOP_RPC_TRACE` options off in release builds.
   - Rename the window and drop the PS2Recomp leftovers.

### Phase 1 — Quick, low-risk wins

| Step | Change | Removes |
|------|--------|---------|
| 1.1 | `NativeSeng` always on; a null output when no audio device is present | Any real use of the IOP interpreter |
| 1.2 | Drop `ps2xIOP` from the shipping build (keep it for tests); delete `ps2_audio*.cpp` and the VAG sniffing in `fioRead` | Dead emulation code, a per-read cost |
| 1.3 | Saves in `%USERPROFILE%\Saved Games\WWE Crush Hour` (XDG on Linux), wide paths, write to a temp file then rename, configurable root | Saves next to the exe, ANSI paths, corruption risk |
| 1.4 | Scratchpad as a 16 KB host array tested in the memory fast path; flat array for the hot MMIO registers | The slow path for every SPR access |
| 1.5 | Audio polish: pool source voices, handle device loss (`IXAudio2EngineCallback`), decode commentary streams on a worker thread or at install time, master/SFX/commentary volume | The per-sound voice churn, EE-thread decode stalls |
| 1.6 | Config next to the exe or in `%APPDATA%`; a Settings page in the existing mod menu (display, audio, controls) that writes back to it; the exe finds `GameFiles` itself | The .bat `cd` requirement, the read-only ini |

### Phase 2 — Make the recompiled code behave like compiled code (biggest CPU win)

All of these are recompiler changes, validated by the Phase 0 harness:

1. Emit `ctx->pc` only at call sites, checkpoints and instructions that can fault, not per instruction.
2. Cache GPRs in C locals inside each function; spill them at calls, checkpoints and the scheduler boundary.
3. Mark `rdram` as `__restrict`, or use typed accessors, so stores stop forcing register reloads.
4. Turn JALs to known targets into direct C calls with an inline "did the scheduler take over?" check; keep
   `dispatchGuestBranch` only for JALR / unknown targets.
5. Inline `eeCheckpointDue()` as a single flag test.
6. Reserve guest RAM at a fixed host base so `ptr = base + addr`, with no masking or wraparound branch.

Expected effect: a large drop in EE-thread time. That leaves headroom for higher frame rates and lower-end
hardware. Risk: medium; the codegen changes touch every function, hence the harness first.

### Phase 3 — Timing and frame pacing

1. Back EE timers T0-T3 (and COP0 `Count`) with `QueryPerformanceCounter`, so they follow wall time. Before
   that, answer the open question of what `GameClock` and Havok read for time.
2. Drive the game's VBLANK from presentation (one tick per presented frame) instead of a fixed 16,667 µs timer,
   and collapse the three pacers into one host frame gate.
3. Allow targets above 60 fps (120/144/display refresh) once a play-through confirms animation and Havok use dt
   everywhere. Add a fixed-step cap for any system that doesn't.
4. VRR support: `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`, an uncapped mode, and a frame-rate limiter in the settings
   page.
5. Stretch goal: render interpolation (simulate at 60, render at display rate) if a high-rate simulation proves
   unstable.

Risk: medium; mainly gameplay or physics timing that assumes a fixed step.

### Phase 4 — Input

1. Replace XInput with SDL3 gamepad (or GameInput): DualSense/DS4, Switch, Steam Deck, hot-plug, radial
   deadzones.
2. Feed analog triggers into the pressure bytes, and drive both rumble motors with proportional strength.
3. Add a binding table saved to config, with a remap page in the mod menu, and keyboard rebinding.
4. Optional: feed the game's `PcplController` directly instead of building libpad DMA reports, then retire the
   libpad state machine.

### Phase 5 — Graphics de-emulation (largest effort, do in order)

1. **FMV direct to D3D11.** This is independent and the best first target.
   - A host-side player (FFmpeg or Media Foundation) decodes into YUV textures, converted to RGB in a shader.
   - Full-screen PSS is presented directly. VidTex and arena screens are written into the D3D texture bound to
     that raster.
   - This removes the guest-RAM macroblock write-back, the VRAM upload and the CPU present path with its full
     render-target readback.
2. **Load-time textures.**
   - Hook RenderWare raster creation and texture read, and decode each RwRaster once into a D3D texture with a
     full mip chain (optionally BCn).
   - Key it by RwRaster pointer and lifetime, and add anisotropic filtering.
   - This allows a **texture replacement pack** (load PNG/DDS by hash).
   - It removes the per-draw chain replay into a private GS memory and the per-draw CRC hashing.
3. **Native 2D/HUD.**
   - Hook `PcplSpriteBank::FlushSprites`, `DrawFace`, Im2D and Im3D so they carry an RwTexture instead of reading
     TEX0 from GS registers.
   - Batch sprites natively, and lay out the HUD by anchor/aspect ratio instead of the 3/4 x-scale heuristic.
   - After this, nothing reads textures from VRAM.
4. **Render targets from camera rasters.**
   - Create render targets from RenderWare camera rasters (`RwCameraBeginUpdate`) instead of GS FBP addresses.
   - Add a native camera clear in place of the `SkyCameraClear` sprite strips.
   - This removes `syncRtToVram` / `uploadRtFromVram` and their GPU stalls.
   - Enable **MSAA** with a resolve, and make FTOI4/12.4 truncation optional ("PS2-accurate" vs "clean" setting).
   - High risk: motion blur, env maps and the arena screen use raster-sharing tricks. Verify each one.
5. **Finish VU1.**
   - Hand-port PGen particles and the crowd to C++ or a compute shader using the existing
     `PS2X_NRW_PGEN/CROWD=verify` harnesses.
   - Port untextured particles, locked/fogged/spot-light leftovers.
   - Then delete the VU1 interpreter, `vu1_recomp.py`, the microcode dumps step and VIF UNPACK.
6. **Remove DMA/VIF/GIF/VRAM.**
   - Once 1–5 are done, the hook points emit a native command list straight to the renderer.
   - Delete the VIF1 interpreter, PATH3 emulation, the GIF arbiter, the GS frontend, the GS VRAM model and the
     software rasterizer.
   - Stub RenderWare's PS2 DMA manager (`_rwDMA*`) to no-ops.
   - This removes the build-step dependency on VU microcode dumps as well.

PC-quality features that come along: MSAA (step 4), anisotropic filtering and mips (step 2), texture packs
(step 2), clean HUD scaling at any aspect ratio including ultrawide (step 3).

### Phase 6 — Files and the sound transport

1. **Path-based loader.**
   - Hook the game's `pcpl_cddvd_*` / `Bundle` layer (`pcpl_cddvd_get_lfile` at 0x2651B0/0x265200 is already
     mapped) to open host paths directly with `std::filesystem` wide paths.
   - Read asynchronously on a worker thread.
   - Shrink the 1 GB preload to hot bundles only. This removes the made-up sector numbers and the linear map scan.
2. **Drop SIF RPC for sound.** Hook `SoundDevice::Flush/Sync` (or `InterfaceSoundLib::*`) via a config override so
   the game calls `NativeSeng` directly. This removes the register-or-stack guessing heuristic and the global
   RPC mutex.
3. **Saves without the card.** Hook the game's season save/load so the PS2 memory-card screens are skipped, and
   replace libmc entirely.

### Phase 7 — Scheduler on fibers

- Implement guest threads as host fibers (`CreateFiber` / `SwitchToFiber`, ucontext on Linux), one C stack per
  guest thread.
- Interrupt/alarm handlers run on a dedicated fiber.
- This removes exception-based unwinding, the resume-label `switch (ctx->pc)` re-entry, and most pc bookkeeping.
- It must keep single-core, priority-ordered semantics; don't use free-running OS threads.
- High difficulty and moderate risk: the scheduler holds many subtle fixes (DI hold, nested resume). Do it after
  Phase 2, because both touch the codegen's return protocol.

### Phase 8 — Cross-platform host (optional)

- Put the host behind SDL3: window, input (already done in Phase 4), audio (FAudio, miniaudio or SDL3 audio
  behind a thin mixer interface).
- Move the renderer to Vulkan or SDL_GPU.
- This becomes far cheaper after Phase 5, when the renderer is a plain command list with no GS model left.
- Result: native Linux / Steam Deck instead of Proton.

### Dependency summary

```
Phase 0 (build + harness) ──┬─> Phase 1 (quick wins)
                            ├─> Phase 2 (codegen) ──> Phase 7 (fibers)
                            ├─> Phase 3 (timing)
                            ├─> Phase 4 (input) ─────────────────────┐
                            ├─> Phase 5.1 (FMV)                       │
                            ├─> 5.2 textures ─> 5.3 2D ─> 5.4 RTs ─┬─> 5.6 remove DMA/VIF/GIF/VRAM ─> Phase 8
                            │                            5.5 VU1 ──┘                                  ^
                            └─> Phase 6 (files, RPC, saves) ─────────────────────────────────────────┘
```

### Suggested first milestone

Phase 0 + Phase 1 + Phase 5.1 (FMV) + Phase 3.1–3.2 (wall-clock timers, present-driven VBLANK). Each piece is
low to medium risk and independently shippable. Together they remove the IOP, fix saves, remove the worst GPU
stall (movie readback) and set up high-refresh support.
