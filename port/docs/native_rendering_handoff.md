# Native rendering for PS2 static recompilations: a handoff guide

Written for another Claude working on a PS2Recomp-style port (EE code recompiled to C++, running on PC). It explains
how the WWE Crush Hour port (RenderWare 3.x, SLUS-203.85) stopped emulating the PS2 graphics chain and draws the
game's geometry with Direct3D 11 straight from the game's own data, and what carries over to other games. The
game-specific details (addresses, offsets, numbers) are in `native_renderware.md`; this file is the method.

---

## 1. The problem

A recompiled PS2 game still talks to PS2 graphics hardware. Every frame, the engine:

1. builds DMA chains in EE memory (geometry "instanced" into VU-ready packets, texture uploads, GS register writes),
2. kicks them to VIF1, which unpacks data into VU1 memory and starts microcode (`MSCAL`/`MSCNT`),
3. runs VU1 microcode, which transforms, lights, clips and `XGKICK`s GIF packets,
4. sends GIF packets to the GS, which rasterises into 4 MB of VRAM (swizzled, paletted textures, 16/24/32-bit
   targets).

Emulating all of this works (the first milestone of this port did), but it is slow and fragile: VU1 interpretation was
~190 ns per instruction pair, and millions of pairs per frame meant 1 fps. VRAM-level texture decode, CLUT handling,
render-target aliasing and PATH3 timing are a constant source of garbage textures and stalls. Static recompilation of
VU1 microcode (`vu1_recomp.py`) got it to playable, but "native" means skipping the chain entirely for most draws.

## 2. The core idea: draw from the engine's data, in the engine's order

Do not reimplement the engine's renderer from scratch. Keep the engine's own render loop (culling, sorting, state,
render-target switches all stay correct for free) and replace only the **last mile** -- the geometry that would go
through DMA -> VIF -> VU1 -> GIF -- with a native draw built from the engine's CPU-side structures.

Two rules make this work:

1. **Order is everything.** Native draws must land in the same render targets, in the same order, as the draws still
   on the emulated path (clears, 2D, effects you haven't ported yet). Never draw "when the EE calls render". Instead
   put a **marker into the engine's own DMA stream** where the geometry would have been, and draw when the VIF
   (emulated) reaches that marker. Everything stays serialised with the GIF packets around it.
2. **Bit-exact before fast.** For each piece you port, build a verify mode that runs the original path and the native
   computation side by side and compares the results (vertices, colours, UVs, GS state). Only then switch the
   original off. This caught dozens of subtle mistakes (half-pixel offsets, FTOI truncation, light ordering, Z scale).

## 3. Architecture in this port

```
EE (recompiled game)                    VIF1 (emulated)               GS thread / D3D11 backend
--------------------------------------  ----------------------------  ------------------------------------------
engine render loop (unchanged)
  -> hooked pipeline node / bridge:
     record object, mesh, material,     
     constants in an EE-side side table
     append marker to the DMA packet --> reaches MARK 0xC0DE + NOP idx
                                        snapshot what the VU would    -> MeshJob queued in order with GIF packets
                                        have seen (VU1 data memory,     -> D3D11: same RT/blend/depth logic as GS
                                        DMA-referenced data)               draws; vertices from cached immutable
                                                                           VB/IB built from engine arrays; vertex
                                                                           shader reproduces the VU1 math
```

### 3.1 Markers

The VIF packet is a DMA chain. A marker is a zero-length `CNT` DMA tag; with TTE on, its upper 64 bits go to the VIF
as two VIF codes: `MARK 0xC0DE` and `NOP` whose immediate indexes the side table. The VIF interpreter recognises the
pair and calls the native renderer. Costs nothing on the emulated side.

Pitfalls found:
- If the engine has an open **GIF DIRECT section** in the packet, markers appended inside it become GIF payload and
  vanish. Close the section the way the engine's own "open VIF packet" function does (patch its header tag).
- Respect the engine's packet-space checks (`_rwDMAOpenVIFPkt` fast path) or you overrun the buffer.
- Data the VU would read through a DMA *reference* (not copied into the packet) can change after the EE "renders":
  snapshot it when the VIF reaches the marker, not when the hook runs (particle history, light uploads).

### 3.2 Two levels of hook

1. **Bridge level (first step, lowest risk).** The engine still does all its per-object work (texture cache, uploads,
   VU constants, GS registers via DIRECT); only the final DMA `CALL` into instanced geometry is replaced by a marker.
   GS state comes from the real registers. Good for proving the vertex math.
2. **Node level (the real win).** Hook the pipeline node itself (RenderWare `PS2AllNodeBody`). For supported objects
   nothing of the engine's PS2 pipeline runs: no instancing, no bridge, no VU uploads. You compute the VU constants
   and GS state from the engine structures yourself (matrices, camera block, lights, material colour, TEX0/TEX1 from
   the raster). Unsupported objects fall through to the original pipeline, which still works.

Make each hook **all-or-nothing per call** when the engine batches (the game's world node draws every visible sector
in one call: either handle all of them or hand the whole call back; partially trimming the engine's lists broke it).

### 3.3 The backend

- One render-target/texture/blend/depth/alpha-test model shared by GS-path draws and native jobs, so a native mesh
  and an emulated sprite interleave correctly.
- Geometry: immutable vertex/index buffers built once per engine object from its CPU arrays (positions, normals, UVs,
  prelit colours, mesh index lists), keyed by object + content; bounds-check everything (objects get freed during
  level loads).
- Animated data (skinning, morph targets, particles, Im3D/Im2D immediate geometry): blended or generated on the CPU
  into a per-frame ring buffer.
- The vertex shader is a transcription of the VU1 program's math (see 4). Output homogeneous position (screen * w)
  with `noperspective` colour/STQ to match the GS's interpolation, and use `SV_ClipDistance` for the near/far planes
  the VU clipped against.
- Textures: decode the engine's raster (in EE memory) rather than VRAM when you can, cache by raster + content hash.
  Render-target textures (env maps, live video screens, motion blur) must be sampled from the GPU render target, not
  read back.

## 4. Reverse-engineering the VU1 microcode

You need the exact math, not an approximation.

1. Dump the microcode images the game uploads (`PS2X_VU_DUMP`) and identify each by hash. Engines usually ship few
   programs: RenderWare's generic PS2All program covers most geometry; the rest are skin, MatFX/env, particles,
   crowd, Im2D, etc.
2. Disassemble (a small VU disassembler + the recompiler's output) and map VU data memory: where the matrices,
   screen scale/offset, GIF tag, material colour, lights, flags and output-routine selector live.
3. Note every exact operation: `FTOI4` (truncate *16) for XY/Z, `FTOI0` for colour, the half-pixel bias folded into
   the matrix (`C.xy -= 0.5*C.w`), Z scale, clamp order (add lights, then `min(255)`, then multiply by material
   colour), fog formula, how light types are encoded and when the list ends.
4. Write a CPU twin of the shader and compare it, vertex by vertex, against the GIF packets the VU really kicked
   (verify mode). Key reports by object flags and light set, so you see exactly which variant diverges.
5. For programs you can't hand-port economically (particle generators, crowd), **run the compiled microcode on the
   CPU against a private copy of VU memory** (the statically recompiled VU1 program), feed it the inputs the engine
   would have DMA'd, and draw what it kicks. Still "native" (no emulated VIF/GIF), and exact by construction.

## 5. Order of work that worked

1. Get the emulated path correct first (you need it as the reference and as the fallback).
2. Statically recompile VU1 microcode (performance prerequisite; also gives you runnable programs for step 5 above).
3. Bridge-level markers for static world geometry; verify exact.
4. Node-level path for world sectors and atomics (no PS2 pipeline at all); verify GS state + VU constants exact.
5. Lighting variants (ambient, directional, point), clip and fog variants.
6. Parallel projection (HUD / 2D cameras), Im3D, Im2D.
7. Env maps / MatFX, skinning, morph targets.
8. Engine-specific programs (particles, crowd) via private VU runs.
9. The game's own world node (biggest single perf win: the engine builds no world packets at all).
10. Measure coverage: a stats mode that counts native vs fallback per object type and *why* each fallback happened.
    At the end of this port: ~1.63M objects native vs ~8,300 fallback per match; menus 100% native.

## 6. Things that bit us (check these in any port)

- **PATH3 / texture upload timing.** Engines kick texture uploads on GIF PATH3 while masked and release one packet per
  VIF `MSKPATH3` unmask. Releasing too many or too few corrupts textures (early uploads overwrite slots still in use;
  a growing backlog makes textures arrive seconds late). Native draws must not add releases of their own, but must
  not swallow the ones the original packets carried either.
- **Draw-order accidents.** Some PS2 draws depend on whatever GS register value the previous draw left (e.g. particle
  programs with TME set but no texture upload). Native draws never write GS registers, so they can't reproduce that;
  leave such draws on the emulated path.
- **Resource lifetimes.** Engines reclaim instance memory not "used" this frame. If you skip the engine's pipeline,
  replicate its "mark resource used" call or your data gets freed under you.
- **HLE stubs.** Wrong libgraph/libgs HLE (frame/Z buffer addresses) shrank the texture cache and caused rings,
  garbage and render-into-wrong-target bugs that looked like renderer bugs. Check stubs against the real ELF code.
- **Float semantics.** EE FPU/VU differ from IEEE (no inf/NaN, `x/0 = +-FLT_MAX`, clamping). Get these right in the
  recompiler or physics/culling NaNs leak into rendering.
- **Widescreen.** Widen the 3D view window in the game's camera setup, then draw the 2D layer (HUD, menus) at 3/4
  width about the centre in the backend; leave full-width quads and render-to-texture draws alone; pillarbox frames
  with no 3D scene. Screen-space things placed from 3D projections (name tags) need their x spread too.

## 7. Tooling you'll want

- Environment switches per feature: off / on / verify / log (`PS2X_NRW_*` here). Being able to flip one subsystem
  back to the emulated path instantly is how you bisect.
- Hidden, scripted test runs (window hidden, input from a timeline anchored to calls of a known menu function),
  periodic screenshots, RAM snapshots at a point relative to "match start", and a status line that can peek guest
  memory.
- A GS-primitive histogram (what still reaches the GS) and a VRAM "last writer" tracker for clobbered textures.
- Patch scripts that edit the generated C++ idempotently (hooks at function entry or after a known label), so a
  re-generation of the recompiled code can re-apply them.

## 8. Adapting to a game that isn't RenderWare

The method is engine-agnostic; only the hook points change. Find, in the engine:

- the function that turns an object + material into a DMA packet for VU1 (the "bridge"),
- the per-object pipeline entry above it (the "node"),
- the CPU-side source arrays (positions, UVs, normals, colours, index lists) and the matrices/lights it uploads,
- the microcode programs it uses and their VU memory layout.

Start with the bridge-level marker for one object type, make verify mode report zero mismatches, then climb. Keep the
emulated path alive the whole time as the reference and the fallback.
