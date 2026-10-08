# Frame pacing and stalls: WWE Crush Hour (SLUS_203.85)

## Frame pacing

- The game's frame limiter `ps2_wait_for_vsync` (0x1771a0) spins until the vsync counter at 0x49A9F0 reaches a
  threshold, then calls `VidTexManager::PostVSync` and clears the counter. `_ps2_vsync_handler` (0x177170, VBLANK
  interrupt) increments the counter. The original threshold is 2 (30 fps); `tools/patch_60fps.py` makes it come from
  `ps2x_frame_vsync_threshold()` (1 when `TargetFPS >= 60`). Game logic uses a real-time stopwatch and Havok steps
  with a variable dt, so the game keeps its speed.
- `rwDMAVSYNCHandler` (0x33cf90, RenderWare's VBLANK handler) flips the display (`sceGsPutDispEnv` -> DISPFB write ->
  `ps2GsThreadDeferFlip`) and kicks the next GIF chain. The flip is the EE/GS-worker back-pressure point (the EE runs
  at most one frame ahead).
- VBLANK events are scheduled on host time (`EeScheduler::processDueDeadlines`). After a host stall the vblank
  clock restarts from now instead of replaying the missed vblanks (that ran the game fast-forward afterwards).

## Stall sources found (2026-10-06)

| Symptom | Cause | Fix |
|---|---|---|
| 0.1-1 s freezes mid-match | Cold reads from a busy hard disk: `sceCdRead` (bundles), `sceCdStRead` (VidTex arena-screen movies), native SENG `streamPlay` | `host_file_cache.cpp`: asset folders preloaded at boot, other files <= 64 MB loaded on first read |
| 2-3 s at exactly 30 fps when an arena screen / HUD portrait video plays | `sceMpegGetPicture` waited (waitVSync) until the next picture was due, blocking the game loop | small videos (<= 256x256) keep the previous picture instead (`PS2X_MPEG_TEXTURE_WAIT=1` = old behaviour) |
| Seconds-long GS waits in test runs | test screenshots written to disk under the D3D lock | written on a worker thread |
| Console window; slow logging | Release exe was a console app | `PS2X_SHOW_WINDOWS_CONSOLE` defaults OFF |

Result: match mean 60.1-60.2 fps, minimum 58-59.5 (before: mean 57.5, p10 45, dips to 30).

## Diagnostics

- `PS2X_HITCH_LOG=1`: every frame (flip to flip) over 25 ms with the time the EE waited for the GS worker; frames
  over `PS2X_HITCH_SAMPLE_MS` (default 45) list the sampled guest call targets (1 ms sampler on the branch ring;
  `0xeeeeeeee -> X` = scheduler resumed a thread/interrupt at X); a stall over 100 ms dumps the guest threads
  (`[hitch-threads]`).
- `PS2X_PROFILE=<file> PS2X_PROFILE_STALL=1` (with `PS2X_HITCH_LOG`): native stacks sampled only while no frame
  was flipped for 50 ms. `PS2X_PROFILE_GS=1` samples the GS worker instead, `PS2X_PROFILE_SEC=<s>` starts late
  (e.g. 150 = in the match). Report: `python tools/prof_report.py <file> <exe .map> 30 --stacks`.
- `PS2X_CD_STATS=1` (stream reads per 2 s), `PS2X_CD_LOG=1`, `PS2X_INTC_LOG=1` (INTC causes raised while masked),
  `python tools/fpsstat.py <stdout> [from_sec]`.

## Widescreen (`Widescreen=1`, test override `PS2X_WIDESCREEN=1`)

`tools/patch_widescreen.py` makes `RwCameraSetViewWindow` (0x330350) call `ps2x_widescreen_view_window` on entry.
Calls from the game's camera API (`pcpl_set_camera_fov / _width_fov / _aspect_ratio / _view_window`,
0x26f740-0x26fa6c) for a camera whose frame buffer is a full-screen camera raster (type 2, width >= 512) get the
view window widened by 4/3; texture cameras (env maps, the arena screen feed) keep theirs. The 640-wide frame is
shown stretched to 16:9, so 3D keeps its proportions with a wider view. The HUD and menus are stretched: they mix
sprites laid out across the view window (`PcplSpriteBank::FlushSprites` builds the matrix from `viewWindow`),
Im2D in screen pixels (`DrawFace` portraits, boxes, load bar) and Im2D full-screen polys, so narrowing only one
of them (tried: sprites at 3/4 width) pulls the portrait frames away from the portraits.
