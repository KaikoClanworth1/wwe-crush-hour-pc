# SENG (Sound ENGine) EE→IOP protocol — WWE Crush Hour (SLUS_203.85)

Spec for a host-side (C++/XAudio2) replacement of `MODULES\SENG.IRX`.

## 0. Sources and confidence

The EE side comes from the recompiled functions in `recomp\output\*.cpp`, read from their MIPS disassembly comments.

The IOP side comes from disassembling `GameFiles\MODULES\SENG.IRX`, which is better than expected: **the IRX is not stripped**. It has a full `.symtab` and Metrowerks DWARF-1 `.debug` info: function names, struct member names and offsets, enum names, and local variable names. The original source file names are embedded in it (`...\lib_sound\src\ps2_sound\iop\seng_*.c`). Everything below that describes IOP behaviour was read directly from those handlers.

Confidence markers used in this document:
- **[C]**: confirmed from code on both sides, or from IRX code together with the DWARF names.
- **[I]**: inferred, high confidence.
- **[?]**: uncertain.

Some useful helper symbols on the EE side:

| Address | Symbol |
|---|---|
| `0x109FB8` | `fptoui` (float → unsigned int, truncating) |
| `0x116EE0` | `FlushCache` |
| `0x119C30` | `sceSifBindRpc` |
| `0x119E00` | `sceSifCallRpc` |
| `0x119FF0` | `sceSifCheckStatRpc` |
| `0x134FA8` | `strcpy` |
| `0x135140` | `strlen` |
| `0x131648` | `memcpy` |
| `0x2651A0` | `pcpl_cddvd_num_files` |
| `0x2651B0` | `pcpl_cddvd_get_lfile(int)` |
| `0x265200` | `pcpl_cddvd_get_lfile(const char*)` |

---

## 1. Transport: RPC

| Item | Value |
|---|---|
| RPC server id (SID) | **`0x1234`** [C]. EE: `sceSifBindRpc(this+0x148B8, 0x1234, 0)` in `SoundDevice::Init`. IOP: `sceSifRegisterRpc(sd, 0x1234, _event_rpc_dispatch, event_rbuff, 0, 0, qd)` in `_main_loop`. |
| RPC function number | Always **0**. The IOP ignores it; any non-zero value only triggers a printf. |
| Call mode | `SIF_RPCM_NOWAIT` (1). The EE waits later in `Sync` via `sceSifCheckStatRpc`. |
| EE send buffer | `SoundDevice+0x00000`. The command stream is at most `0x14000` bytes, because the IOP `event_rbuff` is `0x14000`. The EE area `this+0..this+0x14040` is used as the command buffer. |
| Send size | `this+0x14884` = byte length of the stream including the terminator and its pad. |
| EE receive buffer | `SoundDevice+0x14040`, receive size **`0x840`**. The IOP only fills `event_sbuff` (0x800 bytes); the last 0x40 bytes are unrelated IOP memory. Treat them as don't-care. |
| IOP buffers | `event_rbuff` 0x14000 (commands), `event_sbuff` 0x800 (status reply). |
| IOP thread | `start` creates `_main_loop` (priority 0x6C, stack 0x800). |

### 1.1 SoundDevice (EE object) field map [C]

This is the global `SoundDevice::mpInstance` at `0x4B60D0`. `SoundDeviceMgr::mpDevice` is at `0x4B60B0`.

| Offset | Type | Meaning |
|---|---|---|
| `+0x00000` | u8[0x14040] | Command stream buffer |
| `+0x14040` | u8[0x840] | RPC receive / status block |
| `+0x14880` | u8* | Command write pointer |
| `+0x14884` | u32 | Byte length of the last flushed stream |
| `+0x14888` | u8 | "synced" flag. `Sync` sets it to 1; `Flush`/`Reset` clear it to 0. |
| `+0x1488C` | f32 | Master volume (0..1), set by `InterfaceSoundLib::SetGlobalControl(1, v)`. Multiplied into every event volume the EE sends. |
| `+0x14890` | u32 | `max_events` (also the number of groups) |
| `+0x14894` | u32 | `max_banks` |
| `+0x14898` | u32 | `max_streams` |
| `+0x1489C` | u32 | `max_listeners` |
| `+0x148A0` | ptr | Group status array: `max_events` × 16 bytes `{u8 used; u32 status; void(*hook)(u32 status, void* arg); void* arg}` |
| `+0x148A4` | ptr | Event status array (same layout) |
| `+0x148A8` | ptr | Stream status array (same layout; status initialised to 1 = OFF) |
| `+0x148AC` / `+0x148B0` | ptr | Listener and bank "used" byte arrays |
| `+0x148B4` | fn* | Idle callback, called while `Sync` spins on a busy RPC |
| `+0x148B8` | `sceSifClientData` | RPC client (0x28 bytes). `+0x148DC` is `client.serve`. |

**IDs are allocated on the EE.** `SoundDevice::{Event,Group,Stream,Listener,Bank}Allocate` scan the "used" arrays and return an index, or 0xFFFF on failure. The IOP never allocates IDs; it indexes its tables directly with the IDs it receives.
- Group IDs and event IDs both range over `0..max_events-1`.
- Stream IDs range over `0..max_streams-1`, listener IDs over `0..max_listeners-1`, and bank IDs over `0..max_banks-1`.

### 1.2 Per-frame protocol [C]

`ISndEng::Sync` / `ISndEng::Flush` are called once per frame by `InterfaceSoundLib::Sync`/`Flush`.

1. **`SoundDevice::Sync()`**
   - If a flush is not outstanding, it prints `"Sync called with no pending RPC commands, Flushing automatically!"` and flushes first.
   - It then spins while `sceSifCheckStatRpc(client)` is non-zero, calling the idle hook at `+0x148B4`.
   - It sets `synced = 1` and calls `UpdateStatusArrays()`, which parses the reply (see §3).
2. Game code appends commands through the helpers.
3. **`SoundDevice::Flush()`**
   - If not synced, it prints and syncs first.
   - It appends **`0x0027`** (END_BATCH) followed by one pad u16, stores the length at `+0x14884`, and calls `FlushCache(0)`.
   - It calls `sceSifCallRpc(client, 0, 1 /*NOWAIT*/, this, len, this+0x14040, 0x840, 0, 0)`.
   - It resets the write pointer to `this` and clears `synced`.

Consequences for the replacement:
- **The status the EE reads in `Sync` is the reply to the previous `Flush`.** The simplest replacement is to hook `sceSifCallRpc` for SID 0x1234 and process the batch synchronously. Then write the status block into the receive buffer, and make `sceSifCheckStatRpc` return 0.
- Voice-end detection runs once per batch, in `END_BATCH`.

**`SoundDevice::Init(SoundInitStruct*)`** (`0x2B7050`):
1. Bind the RPC client, retrying until `client.serve != 0`.
2. Store the limits from `SoundInitStruct` and call `InitStatusArrays`.
3. Emit `INIT` (0x1E) followed by `INIT_IOP_FS` (0x1F) with the CD file table.
4. Call `Flush` and then `Sync`.

**`SoundDevice::Reset()`**:
1. Emit `RESET` (0x20) and its pad.
2. Sync if needed, then append END_BATCH and send the RPC in the same way as `Flush`.
3. Call `ResetStatusArrays` and zero the 0x840-byte receive area.

---

## 2. Command stream format

The stream is a sequence of commands. Each command starts with a **u16 opcode**, followed by its arguments, in little-endian order.

The IOP dispatcher (`srpc_process_cmds`) loops `p = gSengCmdHandlers[*p](ret_area, p+1)` until a handler returns NULL. Only `END_BATCH` (0x27) returns NULL. An unknown opcode is undefined behaviour (it indexes out of the table).

**Alignment rule.** Every command is a multiple of 4 bytes long and starts 4-byte aligned. The one exception is `BANK_LOAD`, which re-aligns explicitly after its strings. The EE fills unused halfwords by advancing the pointer without writing, so **pad halfwords contain stale buffer data, not zero**. u32/s32 fields are always at a 4-byte-aligned address, because the IOP uses `lw`.

In the tables below:
- "Size" includes the opcode.
- Offsets are bytes from the opcode.
- `pad` = ignored u16.

### 2.1 Opcode table

IOP handler names are taken from the `gSengCmdHandlers` table at IRX `.data+0x7EC4` [C].

| Op | IOP handler | Size | Layout (offset:type name) | Sent by (EE) |
|---|---|---|---|---|
| `0x00` | `scmd_bank_load` | var | 2:u16 bank_id, 4:u16 bank_flags, 6:char dir[] NUL, char name[] NUL, pad to 4 | `SoundDevice::BankLoad` ← `SoundBank::Load` ← `InterfaceSoundLib::LoadBank` |
| `0x01` | `scmd_bank_unload` | 4 | 2:u16 bank_id | `SoundBank::Unload` |
| `0x02` | `scmd_event_create` | 8 | 2:u16 event_id, 4:u16 group_id, 6:pad | `SoundMgr::EventAllocate` |
| `0x03` | `scmd_event_destroy` | 4 | 2:u16 event_id | `SoundMgr::EventFree`, `SoundMgr::Reset` |
| `0x04` | `scmd_event_start` | 4 | 2:u16 event_id | `ISL::SamplePlay`, `SoundInstCrossfade::SetVolPitch` |
| `0x05` | `scmd_event_stop` | 4 | 2:u16 event_id | `SoundInst::Stop`, `SoundInstCrossfade::SetVolPitch` |
| `0x06` | `scmd_event_set_volume` | 8 | 2:u16 event_id, 4:u16 volume, 6:pad | `ISL::SamplePlay`, `SampleSetControl`, `CrossfadePlay`, `SetVolPitch`, `SoundInst::Stop`, `SoundInstCrossfade::Stop` |
| `0x07` | `scmd_event_set_pan` | 8 | 2:u16 event_id, 4:u16 pan, 6:pad | `SoundDevice::EventSetPan` (from `SampleSetControl`) |
| `0x08` | `scmd_event_set_pitch` | 8 | 2:u16 event_id, 4:u16 pitch, 6:pad | `ISL::SamplePlay`, `SampleSetControl`, `CrossfadePlay`, `SetVolPitch` |
| `0x09` | `scmd_event_set_sample` | 12 | 2:u16 event_id, 4:u16 bank_id, 6:u16 sample_idx, 8:u16 flags, 10:pad | `ISL::SamplePlay`, `CrossfadePlay` |
| `0x0A` | `scmd_group_create` | 8 | 2:u16 group_id, 4:u16 group_flags, 6:pad | `SoundMgr::GroupAllocate` (the game always passes flags `0x11`) |
| `0x0B` | `scmd_group_destroy` | 4 | 2:u16 group_id | `SoundMgr::GroupFree` |
| `0x0C` | `scmd_group_start` | 4 | 2:u16 group_id | `ISL::SamplePlay`, `CrossfadePlay` |
| `0x0D` | `scmd_group_stop` | 4 | 2:u16 group_id | `SoundInst::Stop` |
| `0x0E` | `scmd_group_pause` | 4 | 2:u16 group_id | `SampleSetControl`, `SetGlobalControl(5,1.0)` |
| `0x0F` | `scmd_group_resume` | 4 | 2:u16 group_id | `SampleSetControl`, `SetGlobalControl(5,0.0)` |
| `0x10` | `scmd_group_set_flags` | 8 | 2:u16 group_id, 4:u16 flags, 6:pad | **never sent** |
| `0x11` | `scmd_group_set_pos` | 16 | 2:u16 group_id, 4:s32 x, 8:s32 y, 12:s32 z | **never sent** |
| `0x12` | `scmd_group_set_volume` | 8 | 2:u16 group_id, 4:u16 vol_ratio, 6:pad | `ISL::SamplePlay` (0x100), `CrossfadePlay`, `CrossfadeSetControl`, `SoundInst::Stop`, `SoundInstCrossfade::Stop` |
| `0x13` | `scmd_group_set_falloff` | 8 | 2:u16 group_id, 4:u32 falloff_dist (0 is treated as 1) | **never sent** |
| `0x14` | `scmd_listener_update` | 24 | 2:u16 listener_id, 4:s32 pos_x, 8:s32 pos_y, 12:s32 pos_z, 16:s32 face_x, 20:s32 face_y | `SoundDevice::ListenerUpdate` |
| `0x15` | `scmd_listener_activate` | 4 | 2:u16 listener_id | `ISL::LoadListener`, `ListenerSetControl(id,1.0)` |
| `0x16` | `scmd_listener_deactivate` | 4 | 2:u16 listener_id | `ISL::Initialize`, `Reset`, `Release`, `Terminate`, `UnloadListener`, `ListenerSetControl(id,0.0)` |
| `0x17` | `scmd_stream_create` | 12 | 2:u16 stream_id, 4:u32 block_len_bytes, 8:u16 stream_flags, 10:pad | `SoundMgr::StreamAllocate` |
| `0x18` | `scmd_stream_play` | 16 | 2:u16 stream_id, 4:s32 sample_rate, 8:u32 first_lsn, 12:u32 file_size_bytes | `SoundDevice::StreamPlay` ← `ISL::StreamPlay` |
| `0x19` | `scmd_stream_stop` | 8 | 2:u16 stream_id, 4:u16 fade_frames, 6:pad | `SoundInstStream::Stop` (fade is always 0) |
| `0x1A` | `scmd_stream_pause` | 4 | 2:u16 stream_id | `StreamSetControl`, `SetGlobalControl(5,1.0)` |
| `0x1B` | `scmd_stream_resume` | 4 | 2:u16 stream_id | `ISL::StreamPlay` (stitch master), `HandleNotification`, `StreamSetControl`, `SetGlobalControl(5,0.0)` |
| `0x1C` | `scmd_stream_set_playlist` | 8+8n | 2:u16 n_tracks, 4:s32 sample_rate, then n × {u32 first_lsn, u32 file_size} | **never sent** |
| `0x1D` | `scmd_stream_set_volume` | 8 | 2:u16 stream_id, 4:u16 lvol, 6:u16 rvol | `SoundDevice::StreamSetVolume`, `ISL::StreamPlay`, `StreamStop`, `StreamSetControl` |
| `0x1E` | `scmd_init` | 20 | 2:u16 max_events, 4:u16 max_events_per_group, 6:u16 max_listeners, 8:u16 max_banks, 10:u16 max_streams, 12:u16 max_playlist_len, 14:pad, 16:u32 effect_area_size | `SoundDevice::Init` |
| `0x1F` | `scmd_init_iop_fs` | var | 2:u16 fstype. If fstype == 1 (CDDVD): 4:u32 num_files, 8: num_files × 36-byte `sceCdlFILE`. If 0 (HOST): size 4. | `SoundDevice::Init` (always fstype 1) |
| `0x20` | `scmd_reset` | 4 | 2:pad | `SoundDevice::Reset` |
| `0x21` | `scmd_reset_regions` | var | 4: s32 region_sizes[] terminated by −1 | **never sent** |
| `0x22` | `scmd_set_region` | 4 | 2:s16 region | **never sent** |
| `0x23` | `scmd_clear_region` | 4 | 2:s16 region | **never sent** |
| `0x24` | `scmd_remove_all` | 4 | 2:pad | **never sent** |
| `0x25` | `scmd_set_spkr_mode` | 4 | 2:u16 mode (0 = mono, 1 = stereo) | `ISL::Initialize`, `ISL::Reset`, `SetGlobalControl(0,x)` |
| `0x26` | `scmd_set_effect_params` | 24 | 2:u16 evol, 4: `{s32 core; s32 mode; s16 depth_L; s16 depth_R; s32 delay; s32 feedback}` (20 bytes) | **never sent** |
| `0x27` | `scmd_end_batch` | 4 | 2:pad. Terminates the stream and produces the status reply. | `SoundDevice::Flush`, `SoundDevice::Reset` |

"Never sent" means no EE function writes that opcode: I grepped every function that touches the `+0x14880` write pointer. A replacement can implement those opcodes as no-ops, or implement them for completeness.

Implementation hints for parsing:
- **`BANK_LOAD` alignment** [C]. The IOP computes `next = (p_after_name_NUL + 3) & ~3` using absolute addresses. The buffer base is aligned, so align on the offset from the stream start.
- The IOP `INIT` handler returns `data+0x12`, which gives the 20-byte size above.
- The IOP `LISTENER_UPDATE` handler reads exactly 5 s32 values: pos xyz and a 2D facing.

### 2.2 Argument units and scaling

| Quantity | Wire format | EE conversion (confirmed in code) | IOP use |
|---|---|---|---|
| Event volume (op 06) | u16 **0..16383** (0x3FFF) | `clamp(vol * master * 16383.0, 0, 16383)`, then `fptoui`. `vol` is 0..1; `master` is `SoundDevice+0x1488C`. | SPU voice volume = `vol_ratio[ch] * volume >> 8` (see §4.4). 0x3FFF is full scale. |
| Event pitch (op 08) | u16 SPU pitch, **0x1000 = 48 kHz** | `clamp(pitch * vbi.sample_rate * 4096 / 48000, 0, 16383)`. `pitch` is a 1.0-based ratio; `sample_rate` is from the VBI entry. | voice pitch = `group.pitch_ratio * pitch >> 8`. **Playback rate in Hz = pitch × 48000 / 4096.** |
| Event pan (op 07) | u16 **0..255** (0 = left, 255 = right, 127/128 ≈ centre). Reset value **1000** means "not panned". | `EventSetPan(p)`: clamp p to [−1, 1], then `fptoui((p+1)/2 * 255)`. | L = `PanVols[256-pan]*vol>>8`, R = `PanVols[pan]*vol>>8` (table in Appendix A). |
| Group volume (op 12) | u16 ratio, **0x100 = unity** | Constant 0x100 in `SamplePlay`. Crossfade code sends scaled values. | Multiplied into the channel ratios. |
| Stream volume (op 1D) | u16 lvol, rvol, **0..16383** | `clamp(v*16383, 0, 16383)` for each channel. `StreamSetVolume(id, l, r)` passes floats 0..1. | Written directly as SPU VOLL/VOLR (see §6.4). |
| Stream sample rate (op 18) | s32 Hz | `cvt.w.s` of a float. The game always uses **22050.0**. | pitch = `rate*4096/48000` |
| Listener pos/facing (op 14) | s32 fixed point **×256** | `pos = (x*256, z*256, y*256)` (Y-up world converted to Z-up); `facing = (fx*256, fz*256)` | 3D panning only |
| Group falloff (op 13) | u32, same units as positions | — | default 0x9600 = 150.0 world units |

### 2.3 Bank loading (op 0x00) [C]

EE path:
1. `InterfaceSoundLib::LoadBank(const char* path, ...)` upper-cases `path` and splits it at the last `\` or `/` into `dir` and `name`.
2. It calls `SoundBank::Load(bank, dir, name, flags=0)`.
3. `SoundBank::Load` itself reads `"%s\\%s.VBI"` (fmt at `0x4040E0`) through `Bundle::CopyFile`, so that the EE has its own copy of the VBI. It stores `sample_count = size >> 4` at `SoundBank+0x10` and the VBI pointer at `+0x8`/`+0xC`.
4. It then emits `BANK_LOAD(bank_id, flags=0, dir, name)`.

- `dir` examples: the base string in the ELF is `"cddvd_image\audio\banks\"` (`0x3D8030`/`0x3D84B0`), with `A-J\`/`K-Z\` subfolders built by the formats `"A-%c\%s"`/`"%c-Z\%s"`. The resulting strings are like `CDDVD_IMAGE\AUDIO\BANKS\K-Z` + `RAW`, or `CDDVD_IMAGE\AUDIO\BANKS` + `VCOMMON`/`SHELL` [I; exact prefix not traced through every caller].
- IOP behaviour:
  1. `sprintf(buf, "%s\\%s.vbi", dir, name)` → `_load_vbi_data`, then `"%s\\%s.vbk"` → `_load_vbk_data`.
  2. In CD mode, `_load_file_cd` keeps only the part after the last `\`, upper-cases it, and matches it with `memcmp(name, cdlfile.name, strlen(name))` against the `sceCdlFILE` table from op 0x1F. That is a prefix match, so `RAW.VBI` matches `RAW.VBI;1`. It reads `ceil(size/2048)*2048` bytes from `lsn`.
  3. The VBK is uploaded whole to SPU RAM (64-byte aligned). The IOP copy is freed unless `bank_flags == 1`.
- **Host replacement:** resolve `<GameFiles>\` + `dir` with the `CDDVD_IMAGE\` prefix stripped + `\` + `name` + `.VBI/.VBK`. Fall back to a basename search, which is what the IOP does.
- Bank IDs are 0..`max_banks-1` (2 by default: typically `VCOMMON` plus a per-arena bank, or `SHELL`).
- `BANK_UNLOAD` (0x01) is a **no-op on the IOP**. It reads the id and does nothing; SPU memory is only reclaimed by `RESET`. A replacement may free the bank.

### 2.4 Sample selection

`EVENT_SET_SAMPLE(event, bank_id, sample_idx, flags)` [C]:
- `sample_idx` is the index into the bank's VBI (16-byte entries).
- The IOP computes the SPU start address as `bank.spu_addr + vbi[sample_idx].vag_offset`.
- `sample_idx` comes from `InterfaceSoundLib::FindBankIndex` (name → index). After loading the bank, `LoadBank` also loads a text file through `Bundle` and `strtok`s it to build the name table [I].
- `flags` is the event flags word (§4.2). `SamplePlay` passes the SoundInst's loop flag, `vfunc 0x8C`.

---

## 3. Status reply block (`SoundDevice+0x14040`, written by `scmd_end_batch`) [C]

This is a packed u16 array. It is variable-length and **not** fixed-offset beyond the first section:

```
u16 stream_state[max_streams];        // play_state of stream 0..max_streams-1 (always present)
{ u16 group_id; u16 group_status; }*  // one pair per group whose status changed since the last batch
u16 0xFFFF;                           // terminator
{ u16 event_id; u16 event_status; }*  // one pair per event whose status changed since the last batch
u16 0xFFFF;                           // terminator
(rest of the 0x840 bytes: garbage / stale)
```

The IOP checks `max_events*8 + max_streams*2 <= 0x800`. The defaults give 200×8 + 2×2 = 1604.

**Status enums** (from DWARF):

| Kind | Values |
|---|---|
| Event | 0 `EVENT_STATUS_UNUSED`, 1 `EVENT_STATUS_INACTIVE`, 2 `EVENT_STATUS_ACTIVE` |
| Group | 0 `GROUP_STATUS_UNUSED`, 1 `GROUP_STATUS_ACTIVE`, 2 `GROUP_STATUS_INACTIVE` (**the order differs from events**) |
| Stream `play_state` | 0 `UNUSED`, 1 `OFF`, 2 `PRELOAD`, 3 `PLAYING`, 4 `FADING_OUT`, 5 `PAUSED` |
| Voice (internal) | 0 UNUSED, 1 INACTIVE, 2 RESERVED, 3 PLAYING |

"Dirty" reporting: every call to `seng_event_set_status` / `seng_group_set_status` sets the dirty bit, **even if the value is unchanged** and even when it comes from an EE command such as create/start/stop/destroy. `END_BATCH` emits and clears all dirty entries, in ascending ID order.

Status transitions:

| Trigger | Event status | Group status |
|---|---|---|
| `EVENT_CREATE` | → 1 (INACTIVE) | |
| `EVENT_START` | → 2 (ACTIVE) | its group → 1 (ACTIVE) |
| `EVENT_STOP` | → 1 | |
| `EVENT_DESTROY` | → 0 | |
| `GROUP_CREATE` | | → 2 (INACTIVE) |
| `GROUP_START` | all its events → 2 | → 1 |
| `GROUP_STOP` | all its events → 1 | → 2 |
| `GROUP_DESTROY` | all its events → 0 | → 0 |
| **Natural end of a non-looping sample** | ACTIVE → **INACTIVE (1)** | |
| `END_BATCH` with group flag 2 and no events left | | → 0 |

The natural-end transition happens when the voice reaches ENDX and `event.flags & 1 == 0`. **This is the "finished" signal the EE waits for.**

EE parsing (`UpdateStatusArrays`):
- For each stream it stores the new status and calls the stream hook every frame.
- For group and event pairs, it calls the hook only if the status differs from the stored one. When the status is 0 it clears the hook.
- Hooks: `StatusHookEvent` (`0x16F940`), `StatusHookGroup` (`0x16FAB0`), and the stream status handling in `InterfaceSoundLib::HandleNotification` (`0x16E540`, which issues `STREAM_RESUME` for stitched streams).
- **Stream "finished" = `play_state` returns to 1 (OFF)** after 2/3.

---

## 4. Event / group / voice model (IOP semantics to reproduce)

### 4.1 Tables [C, from DWARF]

`event` (20 bytes, `gSndEvents[max_events]`):

| Off | Field | Notes |
|---|---|---|
| 0 | u16 group_id | |
| 2 | u16 flags | §4.2 |
| 4 | u8 status | |
| 5 | u8 dirty | |
| 6 | u16 bank_id | |
| 8 | u16 voice_id | 0xFFFF = none |
| 10 | u16 sample_idx | |
| 12 | s32 volume | |
| 16 | u16 pitch | |
| 18 | u16 pan | 1000 = unset |

`group` (`0x30 + 2*max_events_per_group` bytes, rounded up to 4; `gSndGroups[max_events]`):

| Off | Field | Notes |
|---|---|---|
| 0 | u16 flags | |
| 2 | u8 status | |
| 4 | s32 vol_ratio | |
| 8 | u16 old_pitch_ratio | |
| 10 | u16 pitch_ratio | 0 = paused |
| 12 | u16 num_events | |
| 14 | u8 dirty | |
| 15 | u8 audible | |
| 16 | s32 pos[3] | |
| 28 | u32 dist | |
| 32 | u16 listener | |
| 36 | s32 channel_vol_ratios[2] | |
| 44 | u32 falloff_dist | |
| 48 | u16 events[max_events_per_group] | 0xFFFF = empty slot |

`EVENT_CREATE` appends the event to `group.events[num_events++]` and sets `voice_id = 0xFFFF` and `pan = 1000`. `GROUP_CREATE` sets `num_events = 0`, `vol_ratio = 0x100`, `pitch_ratio = 0x100`, and `falloff = 0x9600`.

`listener` (28 bytes):

| Off | Field |
|---|---|
| 0 | u16 active |
| 4 | s32 pos[3] |
| 16 | s32 facing[3] (only [0] and [1] are written) |

`GROUP_PAUSE` sets `pitch_ratio = 0`; `GROUP_RESUME` sets it back to 0x100. Pause therefore means pitch 0 (the voice freezes; it is not keyed off).

### 4.2 Flags

Group flags (`GROUP_CREATE`/`SET_FLAGS`) [C semantics, names inferred]:

| Bit | Meaning |
|---|---|
| 0x01 | **2D / non-positional**: channel ratios = `vol_ratio`, always audible. The game always uses this. |
| 0x02 | Auto-free: when an event stops or ends it becomes UNUSED and is removed from the group. When the group becomes empty it becomes UNUSED. |
| 0x04 | Keep or reserve a voice even when the event is inactive or inaudible (a voice is allocated even for inactive events). |
| 0x08 | 3D: no panning (L = R = 0x80) |
| 0x10 | 3D: no doppler |

The game creates every group with **`0x11`**: 2D, no doppler, no auto-free, no voice reservation.

Event flags (`EVENT_SET_SAMPLE`) [C]:

| Bit | Meaning |
|---|---|
| 0x01 | **Loop**. ENDX does not end the event. The VAG loop flags (§7) make the SPU loop. |
| 0x02 | Alternate envelope: ADSR1 = 0x0F0F, ADSR2 = 0xDFFF (slow linear attack, slower exponential release). The default is ADSR1 = 0x800F, ADSR2 = 0xDFE0 (instant attack, instant release). |
| 0x04 | Mute left channel |
| 0x08 | Mute right channel |

### 4.3 `END_BATCH` algorithm (`scmd_end_batch`, IRX 0x0FE8) [C]

1. Read ENDX for both cores. For each voice in state PLAYING whose event does not have `flags & 1`: if ENDX is set for that voice, the voice becomes INACTIVE and **the event becomes INACTIVE (dirty)**.
2. For each group with status ACTIVE, run `_position_snd_group` (§4.4).
3. For each group, for each event slot: derive `stop` and `remove` from the event status and group flags:

   | Event status | stop | remove |
   |---|---|---|
   | UNUSED | 1 | 1 |
   | INACTIVE | 1 | 1 if `(gflags & 2) \|\| !(gflags & 4)` |
   | ACTIVE | 1 if `(gflags & 2) \|\| !(gflags & 4)` | same as stop |

   Then:
   - If the event is ACTIVE and has a voice that is not playing, start (key on) the voice.
   - If `group.audible == 1`, clear `stop` and `remove`.
   - If `stop` and the voice is playing: key off.
   - If `remove` and a voice exists: free the voice (`voice_id = 0xFFFF`). If additionally `gflags & 2`, set the event to UNUSED and put 0xFFFF in its slot.
4. Compact every group's `events[]` by removing 0xFFFF entries. If `num_events` reaches 0 and `gflags & 2`, the group becomes UNUSED.
5. For each ACTIVE group with events, for each event without a voice: if `(event ACTIVE && group.audible) || (gflags & 4)`, allocate a voice. If one is available, and the event is ACTIVE, start it.
6. For each voice that is not RESERVED:
   - Status PLAYING: if the voice was just keyed on, set the SPU start address to `bank.spu_addr + vbi[sample_idx].vag_offset`, set the ADSR from event flag 2, and schedule KON. If it was not just keyed on, re-set the address only if the SPU NAX differs.
   - Then compute L/R volumes (§4.4) and the pitch, and write VOLL, VOLR and PITCH.
   - Status INACTIVE or UNUSED with a pending key-on: convert it to KOFF.
7. Write KOFF, wait about 21 µs, write KON, for each core.
8. Write the status reply (§3). Return NULL, which ends the batch.

Voice pool: **48 voices** (`gNumVoices = 48`, 24 per core, `core = id / 24`). Streams also take voices from this pool (one per mono stream, two per stereo stream). If no voice is free, the event simply has no voice and is silent; there is no stealing. A native mixer can ignore the limit, or emulate it at 48 minus stream voices.

### 4.4 Volume, pan, and pitch math (`_position_snd_group`, `_position_snd_voice`) [C]

```
// group, per batch (only for ACTIVE groups):
if (g.flags & 1) {                         // 2D (the only mode the game uses)
    g.ratio[0] = g.ratio[1] = g.vol_ratio; g.audible = 1;
} else {                                   // 3D
    _pan_and_doppler(g);                   // sets g.ratio[], g.dist, g.pitch_ratio (see below)
    att = 256 - clamp(g.dist*256 / g.falloff, 0, 256);
    for ch: g.ratio[ch] = (g.ratio[ch] * (att * g.vol_ratio)) >> 16;
    g.audible = (g.ratio[0] || g.ratio[1]);
}
// voice:
for ch in {0 = L, 1 = R}:
    if (e.pan == 1000) v = (g.ratio[ch] * e.volume) >> 8;
    else if (ch == 0)  v = (PanVols[256 - e.pan] * e.volume) >> 8;   // group ratio NOT applied!
    else               v = (PanVols[e.pan] * e.volume) >> 8;
    v = clamp(v, -0x3FFF, 0x3FFF);
if (e.flags & 4) L = 0;   if (e.flags & 8) R = 0;
if (speaker_mode == MONO) L = R = (L + R) / 2;
pitch = (g.pitch_ratio * e.pitch) >> 8;    // SPU pitch, 0x1000 = 48 kHz; 0 when paused
```

Notes:
- `PanVols[256]` reads one byte past the table. That byte is 0x54 (the first byte of the next string), so pan = 0 gives L ≈ 84/255. A replacement should clamp the index to 255. The full table is in Appendix A.
- Final linear gain for XAudio2 = `v / 16383.0`.
- 3D panning and doppler (`_pan_and_doppler`, not used by this game) work as follows:
  - Use the nearest active listener: `dist = |g.pos - l.pos|`, using an integer sqrt.
  - `lr = (dx*face[1] - dy*face[0]) >> 8`, `p = clamp(lr*256/dist, -256, 256)`.
  - Target ratios: L = (256-p)/2, R = (256+p)/2. Each frame they are averaged with the previous values.
  - Doppler (if not flag 0x10 and the listener is the same as last frame): `pitch_ratio = 0xA0000 / (0xA00 ± Δdist)`, clamped to [0x40, 0x180].

---

## 5. Speaker mode, effects, regions

- `SET_SPKR_MODE` (0x25) sets `gSpkrMode` (0 = mono, 1 = stereo). EE mapping in `SetGlobalControl(0, v)`: v = 1.0 → 0; v = 2.0, 3.0, or 4.0 → 1. The value is cached at `InterfaceSoundLib+0x40` and re-sent on Init and Reset.
- `INIT.effect_area_size` = 0x80 (effectively no reverb). `SET_EFFECT_PARAMS` (0x26) is never sent.
- Regions (0x21–0x23) partition SPU memory and are never sent. `RESET` frees all banks, events, groups, voices, and streams and resets the SPU allocator.

---

## 6. Streams (commentary)

### 6.1 Allocation (op 0x17) [C]

`InterfaceSoundLib::Initialize` allocates **2 streams**, each with `block_len = 0x4800` (18432 bytes = 9 sectors) and **`flags = 0x08`**.

The IOP requires `block_len % 0x800 == 0`. It allocates `block_len` bytes of IOP RAM and `2*block_len` bytes of SPU RAM (a double buffer), plus one voice (two if stereo).

Stream flags:

| Bit | Meaning |
|---|---|
| 0x01 | Stereo. Each block is split into L = first `block_len/2` bytes and R = the second half, i.e. interleave = `block_len/2`. |
| 0x02 | Loop the file: restart at `first_lsn` at EOF. |
| 0x04 | Use the playlist from op 0x1C: advance to the next track (wrapping) at EOF. |
| 0x08 | **Start paused**: `STREAM_PLAY` sets pitch 0, so the stream preloads without advancing until `STREAM_RESUME`. |

`STREAM_PAUSE` sets pitch 0. `STREAM_RESUME` sets pitch = rate×4096/48000.

### 6.2 Playback (op 0x18) [C]

`InterfaceSoundLib::StreamPlay`:
1. `lfile = pcpl_cddvd_get_lfile(inst->CDFileName())` → `StreamInfo{ f32 rate = 22050.0f, u32 lsn = lfile->lsn, u32 size = lfile->size }`.
2. Emit `STREAM_SET_VOLUME(id, v*16383, v*16383)` (clamped).
3. Emit `STREAM_PLAY(id, (int)22050, lsn, size)`.
4. If the instance has a stitched follow-up (`StitchNext`), it is started recursively on the other stream slot, with its volume, and it stays paused, because every stream has flag 8.
5. The instance that is the stitch master gets `STREAM_RESUME(id)`.
6. When a stream reports OFF, `HandleNotification` resumes the next stitched stream (op 0x1B) and adjusts volumes (op 0x1D).

This is how commentary lines are chained gaplessly.

**The file is identified only by `first_lsn` and `file_size_bytes`, not by name.** The host must map the LSN back to a file. The mapping is available in the `sceCdlFILE` table sent in op 0x1F; see §6.5. The source files are the headerless mono 22050 Hz VAG ADPCM files under `IOPDATA\COMM_JR\**\*.VAG` (627 files).

IOP state machine:
1. `STREAM_PLAY` sets `play_state = PRELOAD(2)` (and prints "Stream already playing!" if the state was not OFF).
2. A CD read thread fills the halves, patches VAG flags (§6.3), and DMAs to SPU. The state reaches PLAYING(3) once audio is running [I; exact transition point in `_handle_trans_event` not fully traced].
3. At EOF (with no loop or playlist), the last block is flagged as end. When the voice reports ENDX with ENVX == 0, `play_state = OFF(1)`. That is the "finished" status the EE sees.

`STREAM_STOP(id, fade)`:
- `fade == 0` (the only case the game uses): immediate KOFF and `play_state = OFF(1)`.
- `fade > 0`: FADING_OUT(4). The volume is ramped by `fade_frames` IOP timer ticks, then the stream turns OFF.

### 6.3 VAG flag patching done by the IOP on stream buffers [C]

For reproducing loop and end behaviour on a raw file:

| Position | Flag written (byte 1 of the 16-byte ADPCM block) |
|---|---|
| First block of the buffer | 6 (loop start) |
| Last block of each half | 3 (loop end, repeat) |
| Final data at EOF | 1 on the second-to-last block, then 7 on the last block (end / stop) |

A native implementation simply decodes the file linearly: decode `file_size_bytes` of ADPCM from the file (ignoring embedded flags, or stopping at flag 7/1) and signal OFF when the decoded data has been played out.

### 6.4 Stream volume application [C]

- Mono stream: VOLL = VOLR = `(lvol + rvol) / 2`. Mono streams therefore cannot be panned.
- Stereo stream in stereo speaker mode: the L voice gets (VOLL = lvol, VOLR = 0) and the R voice gets (VOLL = 0, VOLR = rvol).
- Stereo stream in mono speaker mode: every output is `(lvol + rvol) / 4`.
- Gain = value / 16383. Note that the EE does **not** multiply stream volume by the master volume.

### 6.5 `INIT_IOP_FS` file table (op 0x1F) [C]

The body is `num_files` copies of `sceCdlFILE`, 36 bytes each:

| Off | Field |
|---|---|
| 0 | u32 lsn |
| 4 | u32 size |
| 8 | char name[16] |
| 24 | u8 date[8] |
| 32 | u32 flag |

The copy comes from the EE `pcpl_cddvd` table:
- `_cddvd_init` builds the table from the NULL-terminated name list at **`0x3F7950`**. The list has 1064 entries: indices 0–35 are the `\AUDIO\BANKS\...` VBI/VBK pairs, 189+ are `\IOPDATA\COMM_JR\...\*.VAG`, and so on. It ends at the literal `"END"`.
- Each entry is `{char* path; sceCdlFILE file;}` (0x28 bytes) at `*(0x4A9718)`, with the count at `*(0x4A9720)`.
- The table is filled by `sceCdSearchFile(path + ";1")`.

Implications for the replacement:
- **LSN values are whatever the recompiled runtime's `sceCdSearchFile` produced.** The host should build `lsn → full path` from the EE table itself: walk `0x4A9718`, where `+0` is the path pointer and `+4` is the LSN. The 16-char `name` field in the RPC table is a basename only and is not unique in general.
- Alternatively, hook `InterfaceSoundLib::StreamPlay` or `pcpl_cddvd_get_lfile`. Hooking at RPC level is cleaner.
- `size` is the exact byte size. Stream data starts at byte 0 of the file; there is no VAG header in the COMM_JR files.

### 6.6 PSS/MPEG audio

PSS movie audio does **not** go through SENG. `VidTex::AudioCallback` and `audioDec*` (`0x29C9A0..0x29D010`, the Sony `ezmpeg`-style sample) send PCM to the IOP through `sceSdRemote` / `SDRDRV.IRX` (libsdr, `sceSdTransToIOP`) [I]. It is out of scope for this protocol and needs its own shim, for example decoding the PSS audio stream on the host.

---

## 7. VBI / VBK / VAG details [C]

**VBI** is a headerless array of 16-byte entries. It matches `@anon1` in `seng_bank.c` DWARF:

| Off | Type | IOP name | Meaning |
|---|---|---|---|
| 0 | s32 | `vag_offset` | Byte offset of the sample in the VBK (16-byte aligned) |
| 4 | f32 | `sample_rate` | Hz (22050, 32000, 44100 seen) |
| 8 | f32 | `length` | **Duration in seconds.** This is the previously unknown "param". Check: SHELL #0 has 3952 bytes = 247 blocks × 28 = 6916 samples, and 0.3117 s × 22050 = 6873 samples, i.e. the data minus trailing blocks. |
| 12 | u32 | `flags` | **bit0 = looped sample**. Across all banks, 46 samples have flags = 1 and 540 have 0. |

The sample count is `file_size / 16`. The IOP loads the file rounded up to 2048 bytes; the EE uses the exact size.

**VBK** is raw SPU ADPCM: all samples concatenated, uploaded as-is to SPU RAM. A sample runs from `vag_offset` to the next entry's offset (or EOF). There is no leading zero block.

VAG block flags (byte 1 of every 16-byte block), verified over all 586 samples in all banks:

| VBI flags | Pattern | Meaning |
|---|---|---|
| 0 (one-shot) | 0 … 0, **1**, **7** | 1 = last data block (end + mute); 7 = trailing silent terminator block |
| 1 (looping) | **6**, 2 … 2, **3** | 6 = loop start at the first block; 2 = loop body; 3 = loop end + repeat. The loop always covers the whole sample from offset 0. |

The IOP itself never reads `vbi.flags` or `vbi.length`. Looping on hardware comes from the VAG flags, and the "event finished" logic comes from **event flag bit0** (which the EE supplies).

For a native mixer:
- Treat VAG flag 3 or 7 as loop/end markers: loop back to the last flag-6 block on flag 3; stop on flag 1 or 7.
- Or use `vbi.flags & 1` together with the event loop flag.
- Report the event as finished when a non-looping voice hits end.
- ADPCM decode as in `vag_decode.py`: filter table {0,0},{60,0},{115,−52},{98,−55},{122,−60}; 28 samples per block.

Playback rate = `event.pitch × 48000 / 4096`, which the EE already derived from `vbi.sample_rate × pitch_ratio`. Use the wire pitch rather than the VBI rate so that pitch bends work.

---

## 8. Game-specific constants and typical sequences

`SngEngInitializer::SngEngInitializer` (`0x168610`) builds a default `SoundInitStruct` [C]. `SMSoundMgr::InitLib` may override it [?; not checked]:

| Off | Field | Default |
|---|---|---|
| +0 | max_events | 200 (0xC8) |
| +2 | max_events_per_group | 20 |
| +4 | max_listeners | 2 |
| +6 | max_banks | 2 |
| +8 | max_streams | 2 |
| +A | max_playlist_len | 20 |
| +C | effect_area_size | 0x80 |

The init sequence (first batch) is:

```
1E INIT(200,20,2,2,2,20,pad,0x80)  1F INIT_IOP_FS(1, 1064 × sceCdlFILE)  27 END
```

Then `InterfaceSoundLib::Initialize`:

```
25 SET_SPKR_MODE(mode)  [flush/sync]
17 STREAM_CREATE(0,0x4800,8)  17 STREAM_CREATE(1,0x4800,8)  [flush/sync]
16 LISTENER_DEACTIVATE(0)  16 LISTENER_DEACTIVATE(1)
```

The deactivates make the IOP print a harmless "inactive listener" error.

Play a one-shot sample (`InterfaceSoundLib::SamplePlay`):

```
0A GROUP_CREATE(g, 0x11)   (only if the SoundInst has no group yet)
12 GROUP_SET_VOLUME(g, 0x100)
0C GROUP_START(g)
02 EVENT_CREATE(e, g)      (only if there is no event yet)
09 EVENT_SET_SAMPLE(e, bank, idx, loopflag)
06 EVENT_SET_VOLUME(e, vol*master*16383)
08 EVENT_SET_PITCH(e, pitch*rate*4096/48000)
04 EVENT_START(e)
```

Later, a status pair `(e, 1)` means it finished (or `(e, 2)` means it started). `SoundInst::Stop` emits ops `06`, `05`, `12` and `0D` [I: I confirmed the opcodes, not the exact argument values]. Freeing sends `03`/`0B`.

`SetGlobalControl(id, v)`:
- id 0: speaker mode (op 25).
- id 1: master volume (EE-side only; it affects later op 06 values). It triggers an immediate Flush and Sync.
- id 5: v = 1.0 pauses everything (`0E` for every playing group, `1A` for each active stream); v = 0.0 resumes (`0F`, `1B`).
- ids 2–4 are no-ops.

---

## Appendix A — `_au8PanVols[256]` (IRX `.data` 0x816C) [C]

```
 32, 32, 32, 32, 32, 32, 32, 32, 33, 33, 33, 33, 33, 33, 34, 34, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 38, 38, 39, 39, 40, 40,
 41, 41, 42, 42, 43, 43, 44, 45, 45, 46, 47, 47, 48, 49, 49, 50, 51, 52, 52, 53, 54, 55, 56, 57, 58, 58, 59, 60, 61, 62, 63, 64,
 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 79, 80, 81, 82, 83, 84, 85, 87, 88, 89, 90, 91, 93, 94, 95, 96, 98, 99,100,
101,103,104,105,106,108,109,110,112,113,114,116,117,118,120,121,122,124,125,126,128,129,130,132,133,135,136,137,139,140,141,143,
144,146,147,148,150,151,152,154,155,157,158,159,161,162,163,165,166,167,169,170,171,173,174,175,177,178,179,181,182,183,184,186,
187,188,189,191,192,193,194,196,197,198,199,200,202,203,204,205,206,207,208,210,211,212,213,214,215,216,217,218,219,220,221,222,
223,224,225,226,227,228,229,229,230,231,232,233,234,235,235,236,237,238,238,239,240,240,241,242,242,243,244,244,245,245,246,246,
247,247,248,248,249,249,250,250,251,251,251,252,252,252,253,253,253,253,254,254,254,254,254,254,255,255,255,255,255,255,255,255
```

Index 256, reached by pan = 0 on the left channel, reads 0x54 = 84 in the original.

## Appendix B — Tools used / how to re-derive

- **IRX:** the IRX is a relocatable ELF: `.text` at file 0x90 (VA 0), `.data` at file 0x7E40 (VA 0x7DB0), `.symtab` at 0xBDD0, `.debug` (DWARF v1) at 0xCF10. It disassembles directly with capstone (MIPS32 LE). `gSengCmdHandlers` is at VA 0x7EC4 (40 entries).
- **EE:** the helper scripts used during this research lived in the session scratchpad and were not kept. EE string addresses map to the ELF file offset as `off = addr − 0x100000 + 0x80`.
