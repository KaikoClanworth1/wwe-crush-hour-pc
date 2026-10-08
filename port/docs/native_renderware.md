# Native RenderWare rendering: WWE Crush Hour (SLUS_203.85)

The goal is to draw RenderWare geometry with Direct3D 11 straight from RenderWare's own data. This replaces the chain RenderWare's PS2 driver normally uses: instancing, DMA, VIF unpacking, VU1 microcode, then the GIF stream. It is switched on with `PS2X_NATIVE_WORLD=1` (the code is in `runtime/native_rw.h`, `gs/native_rw.cpp` and `gs/gs_d3d11_backend.cpp`, with hooks from `tools/patch_nativeworld.py`).

## Design

Native draws go to the same render targets as everything still on the PS2 path, and in the same order. To guarantee that, they are inserted **into RenderWare's DMA stream** rather than issued when the EE calls render:

1. `_rpSectorDefaultRenderCallBack` (0x2f4160) records the sector being rendered (`nrw::sectorBegin`).
2. `RpMeshPS2AllBridgeCallBack` (0x2fdf80) runs normally: texture cache access and upload, VU1 constant unpacks, and the GS registers sent via VIF `DIRECT`. Its last qword is normally a DMA `CALL` into the mesh's instanced data (unpacks + `MSCAL`/`MSCNT`), stored at 0x2fe6c4. For world-sector meshes, `nrw::bridge` replaces that qword with a zero-length DMA `CNT` tag. Because of TTE, its upper half reaches the VIF as `MARK 0xC0DE`, followed by a `NOP` whose immediate indexes an EE-side side table (sector, `RpMesh`, prim type, material colour, and so on).
3. When the VIF reaches the marker (`ps2_vif1_interpreter.cpp`), `nrw::marker` snapshots VU1 data memory `976..1023` (lights through per-object constants). It then queues an `nrw::MeshJob` on the GS thread, in order with the GIF packets already submitted.
4. On the GS thread, `nrw::dispatchPacket` calls `GS::nativeDraw(prim)` with the `PRIM` from the job's GIF tag (VU1 1018, bits 47..57). That call hands the current GS draw state to `GSRasterBackend::SubmitNative`.
5. The D3D11 backend runs the job through `flushPending` (the same render target, texture, blending, alpha test and depth logic as GS draws). The vertices, however, come from immutable vertex and index buffers built once from the sector arrays, and `vs_native` does the VU1 math.

## RenderWare data (3.x, PS2 build; offsets verified in memory)

### `RpWorldSector` (0x90 bytes)

| Offset | Field |
|---|---|
| +0x00 | type |
| +0x04 | triangles |
| +0x08 | vertices (`RwV3d`) |
| +0x0C | normals (`RpVertexNormal`: s8 x, y, z, pad) |
| +0x10 | texCoords[8] (`RwTexCoords` u, v) |
| +0x30 | preLitLum (`RwRGBA`) |
| +0x34 | repEntry |
| +0x80 | mesh header |
| +0x84 | pipeline |
| +0x88 | matListWindowBase |
| +0x8A | numVertices |
| +0x8C | numTriangles |

### `RpMeshHeader`

flags (1 = tristrip), numMeshes (u16), serial (u16), totalIndices, firstMeshOffset. The mesh entries follow, 12 bytes each.

### `RpMesh`

indices (u16, into the sector's vertex array), numIndices, material.

### `RpMaterial`

texture +0x0, colour +0x4.

### `RxPS2AllPipeData` (the bridge's `s1`)

| Offset | Field |
|---|---|
| +0x04 | matPvtData (VU1 code table at +0x18) |
| +0x08 | sourceObject |
| +0x0C | meshHeader |
| +0x28 | transType |
| +0x29 | primType |
| +0x2A | matModulate |
| +0x2B | vu1CodeIndex |
| +0x2C | mesh |
| +0x30 | cacheEntryRef |
| +0x3C | surfaceProperties |
| +0x40 | texture |
| +0x44 | matCol |

## The VU1 program (image `bb66df1b684d7a58`, RenderWare's generic PS2All code)

- **Entry 0 (per object) and `0x2aa8` (`jr vi15`, resume per batch).** World sectors resume into `0x138` (lighting pre-pass), then jump through `vi12` to an output routine: `0x0d30` is no-clip/strip, `0x0e50` is clip, with further variants for fog and lists.

- **VU1 memory.**

| Address | Contents |
|---|---|
| 976.. | light list |
| 1008–1011 | object→clip matrix columns |
| 1014 | clip / guard data |
| 1015 | screen scale |
| 1016, 1017 | screen offset parts |
| 1018 | GIF tag (PRE + PRIM) |
| 1019 | material colour scale |
| 1020 | surface properties |
| 1023 | per-object flags (`.x`) and output-routine address (`.y`) |

- **Setup.** `C0..C2.z = 0` and `C3.z = 1`, so depth comes from `q`. For every column, `C.xy -= 0.5 * C.w` and then `C.xy *= scale.xy`; also `C3.z *= scale.z`. The offset is `1016.xyz + 1017.xy`.

- **Per vertex.** The input is 4 qw per vertex: position, uv, colour (ints → float), normal (ints / 128).
  - `clip = x*C0 + y*C1 + z*C2 + C3`, `q = 1/clip.w`, `scr = clip.xyz * q + offset`.
  - xy and Z are output via `FTOI4` (so `Z = trunc(scr.z * 16)`).
  - STQ = `(u*q, v*q, q)`.
  - RGBA = `FTOI0(min(colour + lights, 255) * MEM[1019])`.

- **Lighting.** The light list starts at 980; each light's type is in the low 16 bits of `.w` of its first qword, and the list ends at the first unknown type. The object-space transform is `m0..m2 = MEM[976..978]` with translation `MEM[979].xyz`. Surface coefficients are `MEM[1020]` (x ambient, z diffuse). Lights accumulate into the vertex colour before the clamp.
  - Type 2, ambient (1 qw, handler 0x4b0): `col += L * surf.x`.
  - Type 1, directional (2 qw, 0x538): `dir = (m0*dx + m1*dy + m2*dz) * -MEM[979].w`, then `col += L * surf.z * max(0, n . dir)`.
  - Type 128, point (2 qw, 0x658): `P = m0*px + m1*py + m2*pz + MEM[979].xyz`, `R = pw * m0.w`, `d = P - pos` (not normalised), then `col += L * surf.z * max(0, n . d) * max(0, 1/|d| - 1/R)`.
  - Types 129 and 130, spot (3 qw, 0x7d0 / 0x9b8): not ported, and not seen in the match.

- **Clip variants** (object flag bit 1). The VU clips each triangle in this space: `x * MEM[1021].y`, `y * MEM[1022].y` and `z = MEM[1021].z + MEM[1022].z * w`, all against `w`. Natively the vertex shader outputs homogeneous positions (screen position × w, with `noperspective` colour and STQ, as the GS interpolates them) and two `SV_ClipDistance`s, `w - z` and `w + z`. D3D's x/y clipping and the GS scissor cover the rest. Without this, triangles that cross behind the camera turned into giant triangles in the front-end scenes.

- **Fog** (object flag bit 0). `F = clamp(fz * w, fz*fw, fz*fw + 255) - fz*fw` with `fz, fw = MEM[1014].zw`.

## Verification

`PS2X_NATIVE_WORLD=verify` keeps the VU1 path and puts the marker in front of the CALL. The CPU twin of the shader (`computeExpected`) is compared with the GIF packet that VU1 kicks; reports are keyed by object flags and the set of lights present. `PS2X_NRW_VERIFY_CLIP=1` adds the clip variants and matches each VU vertex to the nearest native one, since their output is re-clipped triangles. Match and menu: XYZ, Z, F, RGBA and STQ are exact for unlit, ambient/directional-lit and point-lit sectors; clip variants differ only for vertices that the VU clipped.

## Node path (RenderWare's PS2 pipeline skipped)

`PS2AllNodeBody` (0x2fbf60, patched by `tools/patch_nativenode.py`) first calls `nrw::nodeBody()`. When the object is
supported, nothing of RenderWare's PS2 pipeline runs for it -- no object setup, instancing, mesh bridges, texture cache
or VU1 uploads. Instead one marker per mesh is appended to RenderWare's open VIF packet and the native renderer draws
the mesh in stream order with constants and GS state computed from the RenderWare structures.

- **Markers.** RenderWare's VIF packet is a DMA chain in which every writer starts with its own tag, so each marker is
  a tag too: CNT qwc 0 whose TTE upper half carries `MARK 0xC0DE` + `NOP index`. `_rwDMAPktPtr` (0x48FE2C) and
  `rwDMABufTopPtr` advance; room is checked like `_rwDMAOpenVIFPkt`'s fast path.
- **GIF DIRECT sections.** A packet often has a DIRECT section open (type flag 0x80000000, header tag address at
  0x48FDF4, back-patched with CNT qwc + VIF DIRECT qwc when the next non-DIRECT writer opens the packet). Markers
  appended inside it become GIF payload and are lost -- in matches almost all of them were. `writeMarkers` closes
  the section first exactly like `_rwDMAOpenVIFPkt` (patch the header, or give back an empty section's qword).
- **Constants.** LTM x view rows `{x, y, z, z}`, the camera block from 0x4BE1B0, the light block
  (`RxPipelineNodePS2AllApplyLight` layout), and per mesh the GIF tag PRIM, material colour, surface properties,
  clip planes and flags (`meshConstants`). TEX1 is the raster extension's K/L/MXL over the bridge's filter.
- **Covered setups.** RpAtomic / RpWorldSector (default pipe, MatFX materials without an effect -- MatFX effect 0 uses
  the generic codes), the game's `object_setup_callback` and its copy of the generic bridge, PGen tri meshes, PDS G3x
  Generic atomics (same lighting math as the G3x program; the ambient / directional values are read from VU1
  995..997 when the VIF reaches the marker -- what `RpPDS_G3x_ADL_LightingUpload` put there, or in the title and
  menus, which draw G3x objects without an upload, whatever an earlier upload left; `PS2X_NRW_G3X_LIGHTS=check`
  compares with the uploaded RpLights: 0 differences over 940,001 meshes), G3x Gem env maps (second additive pass, `UV = 0.5 * N.envFrame - 0.5` in object space, colour =
  coefficient x lit colour; camera-texture env rasters are sampled from their GPU render target) and RwIm3D
  (vertices snapshotted into a ring; GS state from the registers).
- **Parallel projection** (transType bit 8, HUD / 2D cameras): RenderWare's ISO VU1 math -- screen = C.v + offset
  without a divide, only the translation column shifted by half a pixel, all columns scaled (`bindNative`). Fogged
  ISO objects stay on RenderWare. `PS2X_NRW_ISO=0` turns it off.
- **MatFX env maps on generic objects** (`RpPDS_G3_Gem_PS2AllMatBridgeCallBack`, effect 2): the generic pass plus the
  G3x env pass with `UV = 0.5 - 0.5 * (N x M).xy`, M = `_rpMatFXSyncEffectMatrix` (object LTM x inverse env LTM,
  orthonormalised; inverse env LTM for sectors). Plain env textures only.
- **Skinned objects** (`RpPDS_G3_Skin_PS2AllObjectSetupCallBack`, drivers and wrestlers): bone matrices as
  `_rpSkinMatrixUpdating`, up to four weights per vertex blended on the CPU into the job ring each frame, drawn as a
  ring job with the atomic's lit constants and the mesh's native GS state. `PS2X_NRW_SKIN=0` turns it off.
- **RwIm2D** (`tools/patch_im2d.py`, `PS2X_NRW_IM2D=0` turns it off): RenderWare's Im2D VU1 program on the CPU (GS XY
  = (x, y) + 2D offset at 0x4BE260, Z = z, STQ = (u, v) / z, RGBA = colour * scale at 0x4BE230 / 0x4BE220, F = 255),
  drawn through the standard GS-vertex path. `PS2X_NRW_IM2D=log` pairs calls with the VU1 output.
- **Game particles (PGen).** The particle generators are the game's own VU C programs
  (`_$VUCCode_particleGenerator{Sprite,Streak,Instanced}` and 2Cam split-screen versions, code array 0x402980,
  chosen by `PGenBridgeCallback` from parameter-block fields 0xC0 / 0xE8 / 0xCC). Each code chain is one RET tag
  with VIF MPG (the program) and STCYCL + UNPACK V4-32 (constants, the output GIF tag at 270, UV frame tables), so
  the runtime rebuilds the program and its initial data memory from game memory. Per effect it adds the parameter
  block (384 bytes at `*(atomic + *(0x4B5C68) + 0x10)` -> VU 984..1007), the object constants (1008..1018) and the
  emitter history (three V4-32 clusters at plugin +4 / +8 / +0xC, stride 3 from VU 0, up to 90 entries), runs the
  compiled program natively against that private memory (`ps2vu1nRunPrivate`) and draws the kicked triangles
  (PACKED ST / RGBAQ / XYZF2) as one job with the texture decoded from the raster. Timing matters: params and
  constants are copied into RenderWare's packet at render time, but the history is DMA-referenced and the game
  writes its newest entry after rendering, so the program runs when the VIF reaches the marker. The 2Cam variants
  (never seen in single player) were synthesized from the code chains into `vu1dump/` and compiled with the rest;
  they also run natively (same inputs: their second camera is in the parameter block). Verified in a split-screen
  match (`tools/mptest.ps1`): 117,500 runs, 95,880 of them 2Cam, all exact; the crowd there: 33,850 batches exact.
  The verify skips each program's own scratch (absolute stores before reads): sprite / instanced 729..733, 2Cam
  sprite / instanced 736..744, 2Cam streak 640..648. `PS2X_NRW_PGEN=verify` repeats every RenderWare run natively and compares
  inputs and decoded vertices; `PS2X_PGEN_DUMP=<dir>` captures runs (VU state, data memory, kicks).
- **The crowd (RpCrowd).** The stands are tiles (four corners each) that RenderWare's hand-written crowd program
  fills with billboard spectators (count from area x density, placement and animation frame from the VU random
  generator, seeded per tile with RINIT, so a run is deterministic). Natively the setup's constants (VU 957: GIF tag
  template, 958/959: frame counts, height / camera +0x6C, grid reciprocals, 960..: frame UV rectangles / columns)
  and the bridge's (1019 colour, 1021..1023 small clip planes -- the bridge zeroes the large-band globals) come from
  the RpCrowd structures at `*(gp - 0x7C58)`. The tiles exist only in RenderWare's instance data (the game frees the
  geometry arrays), so the marker walks that DMA chain (STCYCL 1/3, UNPACK V3-32 / V2-32 / V4-8, ITOP, MSCAL) batch
  by batch and runs the compiled program on each. Output: strips with ADC restarts, GS context 2 (the setup's
  ALPHA_2 / CLAMP_2 / TEST_2 applied to the job). `PS2X_NRW_CROWD=verify`: 36,125 batches / 2.6M triangles exact.
- **Morph targets.** Atomics with several morph targets (`RpInterpolator` at atomic + 76: flags, start / end target
  s16 at +80 / +82, time +84, recip +88, position +92; targets stride 28, vertices +20, normals +24) are blended on
  the CPU into the job ring (`morphVertices`, 36-byte vertices, factor = position x recip) and drawn like skins;
  when start == end or the factor is <= 0 / >= 1 the start / end target is drawn as it is.
- **The world (`PCPL_WPipeNodeBody`, 0x2b57f0, `tools/patch_worldnode.py`).** The game's own world node draws every
  sector in the camera's visible list (`PCPL_CameraWorldExt` = camera + `*(gp - 0x7BA0)`: array +0, count +8; sectors
  with no polygons, u16 at +0x8C, skipped) with its own clip test (six planes at camera + `*(0x48FEE0)` + 0x60, stride
  0x14, corner index bytes at +0x10; box at sector + 0x68) and PS2All's private-data layout. Natively the hook probes
  every visible sector with `nodeObject` (setup type `kPcplSector`, clip decided by that test), checks the open VIF
  packet has room for every marker, then draws them all and returns TRUE -- RenderWare builds no world packets. All or
  nothing per call: anything unsupported leaves the whole call to RenderWare (whose bridges still draw natively
  through the bridge hook). Trimming the game's visible list to hand only some sectors back blacked the screen out
  for good, so it is not done. Sectors whose texture is a camera raster (type 2, the arena's live video screen) are
  drawn from the render target at the raster's VRAM address. 100% of node calls native in RAW and Hell in a Cell
  matches; the game thread spends more time idle than with RenderWare's world packets. `PS2X_NRW_WORLDNODE=0` turns
  it off, `=probe` only counts.
- **Resource entries.** PGen history buffers and the crowd tiles live in RenderWare's instance memory, which its arena
  reclaims from entries not used this frame. Natively the entry is moved to the "used" list every frame exactly like
  the inlined `RwResourcesUseResEntry` in `PS2AllNodeBody` (`useResEntry`).
- **PATH3 uploads.** Texture uploads sit on the GIF channel while PATH3 is masked: RenderWare kicks a frame's uploads
  (~65 EOP packets) as one chain, and every VIF MSKPATH3 unmask releases one. Native draws that sample VRAM (Im3D,
  ISO, Im2D) carry the release their RenderWare packet had. When fewer unmasks than packets reached the VIF (native
  MatFX env objects), the leftovers used to stay queued and the backlog grew every frame -- textures arrived seconds
  late and the Match Type panel showed garbage. A new masked GIF kick now first sends the previous kick's leftovers
  (the channel cannot be re-kicked while busy). `PS2X_P3_DEPTH=1` logs the queue depth (healthy: ~65, steady);
  `PS2X_P3_TRACE_AT=<sec>,<n>` traces masks, flushes and markers. `PS2X_P3_RELEASE_ALL` is wrong (a frame reuses
  texture slots, so early uploads overwrite textures still needed). For the same reason native Im3D / Im2D draws no
  longer add a release of their own (`PS2X_NRW_P3RELEASE=1` restores it): the texture cache's upload already has
  its release in the VIF stream (`late=` in the `[p3q]` line, packets left for the next kick, is 0 either way), and
  the extra one let the next draw's upload land early -- car name tags lost glyphs ("HI##NI" for CHRIS BENOIT)
  whenever the texture cache was busy. `PS2X_CLOBBER_LOG=<n>` tracks the last writer of every GS block (uploads,
  copies, render-target write-backs) and reports VRAM textures / CLUTs a write-back overwrote;
  `PS2X_P3_LATE_LOG=<n>` names late packets' destinations.
- **Not native.** Objects whose base texture is a live camera texture that RenderWare's texture cache has to place
  first (they go through the bridge hook instead: RenderWare's bridge sets TEX0, the mesh is drawn natively from
  its constants); edited geometry (`lockedSinceLastInst`, re-instanced by RenderWare); untextured particle effects
  (~110 draws per RAW match; `PGenBridgeCallback` uploads no texture for them and only clears TME in RenderWare's
  prim state, which the particle program does not read: all six programs' GIF tags at VU 270 are PRIM triangle with
  TME + ABE (`0x902dc00000008014`, PACKED ST/RGBAQ/XYZF2 x3), so the PS2 samples whatever TEX0 the GS last
  had -- an accident of draw order that native draws, which never write the GS registers, cannot reproduce; they
  stay on RenderWare).
  `PS2X_GS_PRIMSTATS=1` lists what still reaches the GS as PS2 drawing commands every 10 s: in a match it is almost
  only `SkyCameraClear`'s 32-pixel sprite strips (frame and depth clears, ~40 per frame). They stay: the clear packet
  also sets and restores FRAME / ZBUF / TEST / SCISSOR, which the GS layer's render-target and texture model uses.
  Coverage: menus 380,000 node objects native, 0 on RenderWare; matches 1.63M native vs ~8,300 (camera textures,
  via the bridge hook).
- **Turbo motion blur** (`MotionBlurOpen` / `OverlayRender`, RenderWare's example code, on for 1.5 s when
  `VehicleCamera::UpdateMotionBlur` fires): a sub-raster of the camera raster drawn with RwIm2D over the screen --
  a full-screen quad into the back buffer sampling the other frame buffer (TEX0 tbp 0, CT16S, 1024x512 at 50/128
  alpha). The backend samples the GPU render target directly (`PS2X_RTFALLBACK_LOG=<n>` reports any draw at a
  target's address that is decoded from VRAM instead: none). The full-screen rainbow / lavender noise seen when hit
  and the white stair-step blocks near burning wrecks only ever appeared in runs with the extra PATH3 release (above);
  not seen since. `PS2X_GS_TRACE_GSONLY=1` limits `PS2X_GS_TRACE` to draws without native state.
- **Widescreen 2D layer** (`Widescreen=1`). The 3D cameras get a 16:9 view window (`tools/patch_widescreen.py`) and the
  640-wide frame is shown stretched to 16:9, so 2D would come out a third too wide. The backend draws the 2D layer 3/4
  as wide about the frame's centre (shader `rtSize.z/w`): RwIm2D triangles (on the CPU) and parallel-projection (HUD
  camera, `transType & 8`) jobs -- Im3D per triangle (prepareIm3D flags vertices in `nrm.w`: sprite batches mix HUD
  pieces with full-width quads), atomics as a whole. Triangles about as wide as the frame (>= 576 px: fades, flashes,
  the motion blur, full-screen backgrounds) and draws into camera textures stay as they are. A car's name tag is
  placed at the 3D camera's projection of the car (`EnemyInfoHudElement::Update`), so `tools/patch_ws_enemyinfo.py`
  spreads that x by 4/3 about the centre first (`ps2x_ws_hud_x`). The target brackets are 3D effects and need nothing.
  Frames without a 3D scene (movies, loading / text screens) are shown 4:3 with side bars and their 2D left alone:
  a frame is wide when the previous one drew perspective sectors / atomics into the frame; the 2D correction and
  the presentation aspect (latched with the frame in `Present`, `ps2host::setPresentWide`) use the same flag.
  `PS2X_WS_HUD=0` turns the 2D correction off (every frame 16:9 again).
- **Verification.** `PS2X_NRW_NODE=verify` runs the node path as a dry run next to RenderWare's pipeline and compares
  the GS state at each bridge and the VU1 constants at each marker (0 mismatches over 1.24M meshes).
  `PS2X_NRW_ENVLOG=1` counts markers written vs consumed. `PS2X_NRW_NODE=0`, `PS2X_NRW_GEM=0`, `PS2X_NRW_IM3D=0` turn
  parts off; `PS2X_NRW_STATS=1` reports coverage and why objects stay on RenderWare.

## Status

- World sectors (through the game's world node), atomics, G3x (incl. env maps and the title / menu objects without
  lights), Im3D, parallel projection, MatFX env maps, skinned and morphing models, Im2D, particles and the crowd render
  natively; a match runs at ~60 fps.
- Still to do: spot lights (unsupported light types are counted as `light=` rejects: 0 in every run so far).
