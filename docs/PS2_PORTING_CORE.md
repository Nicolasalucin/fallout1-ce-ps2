# PS2 Porting Core — Fallout 1 → PlayStation 2

Exhaustive reference for the PlayStation 2 platform layer of **fallout1-ce-ps2**, written to be
self-sufficient as the direct base for porting **fallout2-ce** (sibling folder
`../fallout2-ce-ps2/`).

- **Provenance:** reflects commit `741769e` (branch `ps2`), release tag `ps2-v1.0.0-rc1`.
- **Development history:** built in phases F0–F9 (F0 environment, F1 boot/timing, F2 video,
  F3 input, F4 I/O, F5 audio, F6 saves, F7 memory/stability, F8 OSK + controls, F9 storage).
  Every source file carries its phase tag in its header comment.
- **Language convention:** code, comments and this document are English (the engine and its
  upstream are English); user-facing docs (README) are bilingual EN/ES.

---

## 1. Architecture overview

The port keeps the upstream engine essentially untouched (~92% as-is) and replaces the platform
edges (~6% new code) behind *existing engine interfaces*:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  fallout1-ce engine (src/game, src/int, src/plib, src/movie_lib.cc)         │
│  — compiled as-is, with small guarded fixes (`__PS2__`) and dev-only        │
│    instrumentation (`PS2_DEV_SMOKE_TEST`)                                    │
├───────────────────────────────┬─────────────────────────────────────────────┤
│  engine interfaces used as-is │  platform replacements (same API)           │
│  ─ SDL2 subset (SDL.h)        │  ─ src/platform/ps2/sdl/sdl_shim.c          │
│  ─ src/platform_compat.h      │  ─ src/platform/ps2/platform_compat_ps2.cc  │
│  ─ gnw_main() entry point     │  ─ src/platform/ps2/main.cc (own main)      │
├───────────────────────────────┴─────────────────────────────────────────────┤
│  PS2 native platform layer (C99 + 2 C++ shims):                             │
│  video_gs · audio_audsrv · input_pad · ps2_osk · fs · ps2_save ·            │
│  ps2_storage · ps2_mem · ps2_time · boot                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│  ps2sdk / gsKit (EE: fileio, SIF RPC, threads, timers)                      │
├─────────────────────────────────────────────────────────────────────────────┤
│  IOP side: rom0:LIBD/LIBSD/SIO2MAN/PADMAN + embedded .irx (usbd/usbhdfsd/    │
│  bdm/usbmass_bd/bdmfs_fatfs/sio2man/mx4sio_bd/audsrv), executed from RAM    │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Key invariants**

- The engine renders into one **8-bit indexed 640×480 surface**; everything above the GS is
  paletted, exactly like the desktop build. No engine buffer is ever converted in place.
- All engine file access is relative (`master.dat`, `data\…`, `…\SLOT01\SAVE.DAT`) and goes
  through `platform_compat` / `db` → the PS2 filesystem layer. Saves are redirected without
  the engine knowing.
- The engine is single-threaded. The **only** worker thread is the audio mixer
  (`audio_audsrv.c`). The EE kernel is priority-scheduled and **does not preempt a spinning
  thread**, so every busy-wait in the port yields (`SDL_Delay`, `DelayThread`). This is a hard
  rule inherited from the movie player's A/V sync loops.
- SDL2 is not linked. A minimal **SDL2 subset** (`src/platform/ps2/sdl/SDL.h`, ~750 lines) is
  compiled in its place and wins over any other SDL.h on the include path.

---

## 2. File inventory

### 2.1 Platform layer — `src/platform/ps2/`

| Path | LOC | Role / key symbols |
|---|---|---|
| `main.cc` | 49 | PS2 entry point (replaces `src/plib/gnw/winmain.cc`). Declares `GNW95_isActive=true`, `GNW95_title`; calls `ps2_boot(argv[0])`, then `ps2_video_selftest()` (non-smoke only), then `fallout::gnw_main()`. |
| `boot.c` | 33 | `ps2_boot()`: init order **time → input → storage → fs → save**; prints `[ps2] boot ok (F1), timebase … ms`. |
| `ps2_time.h/.c` | 29/35 | `ps2_time_ms()` / `ps2_time_us()` over `GetTimerSystemTime()` (EE bus clock 147 456 000 Hz). |
| `ps2_mem.h/.c` | 34/69 | EE heap instrumentation (F7): `ps2_mem_init/report/used_kb/free_kb/sbrk_kb` over newlib `mallinfo()` + `sbrk()` high-water mark. Smoke-only consumers; inert in release. |
| `ps2_video.h` | 52 | Video backend contract: `ps2_video_init/shutdown/present/set_palette/selftest`. |
| `video_gs.c` | 432 | GS renderer (see §4.1). Buffers, palette LUT, gsKit setup, per-phase profiler, F2 self-test pattern. |
| `ps2_input.h` | 44 | Input contract + F8 smoke hooks (`ps2_input_smoke_press/release/release_pending`) + `ps2_platform_in_combat()`. |
| `input_pad.c` | 463 | libpad DualShock 2 backend → synthetic SDL events (see §4.3). |
| `ps2_osk.h` | 48 | OSK contract: `ps2_osk_open/close/toggle/update/is_open/render`. |
| `ps2_osk.c` | 525 | Pad-driven on-screen keyboard, software-composited overlay (see §4.4). |
| `ps2_fs.h` | 67 | Filesystem contract: `ps2_fs_init/resolve/fopen/canonicalize/root_count/root_path`, F5.2 smoke counters. |
| `fs.c` | 447 | Device probing, dynamic roots, case-insensitive canonicalization, 64 KiB DAT buffering (see §4.6). |
| `ps2_save.h` | 72 | Save contract: `ps2_save_init/root/remap/slot_ensure_meta/slot_wipe`; constants `PS2_SAVE_SLOT_COUNT=10`, `PS2_SAVE_ICON_SYS_SIZE=964`, `PS2_SAVE_ICON_ICN_SIZE=928`. |
| `ps2_save.c` | 521 | Save redirection + PS2 browser metadata (icon.sys/icon.icn) (see §4.7). |
| `ps2_storage.h` | 40 | Storage bootstrap contract: `ps2_storage_init(argv0)`, `ps2_storage_elf_dir()`. |
| `ps2_storage.c` | 210 | argv0 parsing, loader guards, tiered IOP bring-up (see §4.5). |
| `ps2_audio.h` | 43 | Audio contract: `ps2_audio_open/pump/pause/close`; callback type mirrors the SDL one. |
| `audio_audsrv.c` | 473 | audsrv output + mixer worker thread (see §4.2). |
| `platform_compat_ps2.cc` | 339 | Engine platform-compat replacement (see §4.9). |
| `sdl/sdl_shim.c` | 1473 | SDL2 subset implementation (see §4.8). |
| `sdl/SDL.h` | 750 | The SDL2 subset the engine is compiled against. |
| `sdl/ps2_sdl_internal.h` | 32 | Event-injection helpers shared with input/OSK/audio. |

Total ≈ 6 280 lines for the whole platform layer.

### 2.2 Build system and tooling

| Path | Role |
|---|---|
| `CMakeLists.txt` (PS2 block, ~line 300-470) | Platform sources, smoke option, IRX embedding, audsrv copy, include priority, compile options, link libs, output name. Excludes `winmain.cc` + `platform_compat.cc` when `PS2=TRUE`. |
| `cmake/toolchain-ps2.cmake` | EE toolchain file (see §5.1). |
| `os/ps2/build/Dockerfile` | Derived build image (`ps2dev/ps2dev:latest` + cmake/make/ninja/git/ca-certificates). |
| `tools/run_smoke.sh` | Automated PCSX2 smoke runner (see §6). |
| `tools/make_dist.sh` | Release bundle builder (Release + strip + verify, see §5.4). |
| `tools/check_no_assets.sh` | CI guard: refuses to track proprietary game assets (`.dat/.frm/.acm/.mve/.map/…`, `local_assets/`, `data/`, `mods/`, `saves/`, `build_ps2/`). |
| `dist/FALLOUT/` | Shipping bundle: `FALLOUT.ELF` (gitignored, regenerated), `SYSTEM.CNF` (`BOOT2 = cdrom0:\FALLOUT.ELF;1`), `README_PS2.txt`. |
| `local_assets/` | Game data for dev (gitignored): `MASTER.DAT`, `CRITTER.DAT`, `DATA/…`, `saves/`. |

### 2.3 Engine files touched (all in this repo, upstream lineage)

| File | Guard | Purpose |
|---|---|---|
| `src/plib/db/db.cc` | `__PS2__` (20 sites) | I/O performance: write aggregation, slurp of read-only loose files, DAT entry slurp/decode, 1-slot decoded-entry reuse (§4.6.4). |
| `src/int/intrpret.cc` + `intrpret.h` | `__PS2__` (5 + field) | `Program` holds a 16-byte-aligned `jmp_buf`; the engine allocator guarantees only 8 → over-allocate and align, remember `rawAllocation`. |
| `src/game/scripts.cc` + `scripts.h` | `__PS2__` (5) | `new_obj_id()` O(ids×world) rescan → 32 768-bit id bitmap, rebuilt every 128 calls or when dirty. |
| `src/game/object.cc` | `__PS2__` (2) | `obj_read_obj()` marks the id bitmap dirty for file-assigned ids. |
| `src/game/map.cc` | `__PS2__` (1) | Map-load refresh callback cadence 8 KiB → **128 KiB** (PS2 presents are vsync-locked). |
| `src/movie_lib.cc` | `__PS2__` (5) | A/V sync waits: `SDL_Delay(1)` + `ps2_audio_pump()` (non-preemptive kernel must yield for the mixer). |
| `src/game/main.cc` | `PS2_DEV_SMOKE_TEST` | Smoke takeover: replaces intro + main menu with `ps2_smoke_main()`. |
| `src/plib/gnw/memory.cc` | smoke only (8) | Exposes engine allocator counters `ps2_mem_engine_cur_kb/max_kb/report`. |
| `src/game/art.cc`, `pipboy.cc`, `lip_sync.cc`, `loadsave.cc`, `map.cc`, `scripts.cc`, `object.cc`, `src/int/support/intextra.cc`, `intrpret.cc` | smoke only | Dev-only instrumentation/profiling (timings, counters, dialogue/save hooks) used by the smoke verdict. |

---

## 3. Boot sequence

```
main()                                   src/platform/ps2/main.cc
 └─ ps2_boot(argv0)                      boot.c
     ├─ ps2_time_init()                  kicks the EE system timer
     ├─ ps2_input_init()                 rom0:SIO2MAN + rom0:PADMAN, padInit/padPortOpen
     ├─ ps2_storage_init(argv0)          ELF dir from argv0; loader guards; tiered IOP bring-up
     ├─ ps2_fs_init()                    validates roots (MASTER.DAT + CRITTER.DAT)
     └─ ps2_save_init()                  mc0: → mc1: → host fallback
 ├─ ps2_video_selftest()                 non-smoke only (±20 s F2 pattern/pacing probe)
 └─ gnw_main()                           engine
     └─ SDL_CreateRenderer → ps2_video_init()          (GS + dmaKit up)
     └─ engine audio init  → SDL_OpenAudioDevice → ps2_audio_open()  (lazy)
     └─ SDL_RenderPresent  → palette + ps2_video_present + ps2_audio_pump()
```

Everything before `gnw_main()` is tolerant to failure (no pad → no input; no audio → silent
play; no data root → warning + `host:` diagnostic). Boot must never hard-fail because a
peripheral device is absent.

---

## 4. Subsystems

### 4.1 Video — GS hardware rendering (`video_gs.c`)

**Pipeline per frame (authoritative — this is the final F2/F5 design):**

```
engine 8bpp 640×480 surface (0.3 MB, engine-owned)
      │  CPU palette LUT lookup  (~1 ms on the EE)
      ▼
s_fb_rgba: CT32 640×480 (1.2 MB, BSS, 64-byte aligned)
      │  [optional] OSK composite (ps2_osk_render)
      │  FlushCache(0) + gsKit_texture_upload  (DMA to VRAM)
      ▼
GS VRAM: GS_PSM_CT32 texture 640×480 (alloc via gsKit_vram_alloc, GS_VRAM_BLOCKSIZE_256)
      │  full-screen textured quad (gsKit_prim_sprite_texture_3d, NEAREST filter)
      ▼
gsKit_queue_exec → gsKit_sync_flip → NTSC 640×448 @ 60 Hz interlaced
```

- The palette lookup is done **on the EE** ("Option A"): `s_palette` (256×CT32, 64-byte
  aligned) is a CPU LUT; the GS CLUT/CSM1/T8 paths are deliberately not used.
- Some comments in `sdl_shim.c` still describe an earlier T8+CLUT iteration — `video_gs.c` is
  authoritative.
- **GS mode:** NTSC, `GS_INTERLACED`, field mode, `GS_PSM_CT32` / `GS_PSMZ_16S`, Z off,
  double buffering ON, dithering OFF, primitive alpha OFF, gsKit **one-shot** mode
  (`gsKit_mode_switch(GS_ONESHOT)`); DMA: `dmaKit_init(..., D_CTRL_RCYC_8, 1<<DMA_CHANNEL_GIF)`.
- **Framebuffers / buffers:**

  | Buffer | Size | Notes |
  |---|---|---|
  | engine surface | 307 200 B | 8bpp 640×480, engine-owned |
  | `s_fb_rgba` | 1 228 800 B | CT32 upload source, 64-byte aligned |
  | `s_staging` | 307 200 B | insurance for non-640×480/pitch surfaces (never hit today) |
  | `s_palette` | 1 KiB | LUT, GS CT32 order `0xAABBGGRR`; alpha clamped to 0x80 (GS 1.0) |
  | VRAM texture | ≈1.2 MB | CT32 640×480, blocksize 256 |

- **Per-phase profiler:** `ps2_video_debug_print_phases()` reports accumulated ms for
  upload/queue/exec/flip (`[ps2] present phases xN: …`); used during bring-up.
- **Palette updates:** `SDL_RenderPresent` rebuilds the LUT only when the engine palette is
  dirty, then forwards `ps2_video_set_palette(rgba[256])` (A/B/G/R order per entry).
- **First-present log:** asserts memory pointer/pitch and 64-byte alignment.
- **F2 self-test** (`ps2_video_selftest`, skipped in smoke builds): paints a rotating-palette
  pattern with 8-px moving strips, validates frame pacing, prints phase timings; costs ~20 s.

### 4.2 Audio — audsrv + mixer worker (`audio_audsrv.c`)

The engine mixes 16-bit stereo PCM through its SDL device callback. The port runs that
callback on a **dedicated EE thread** that streams to the SPU2 through audsrv; playback keeps
advancing while the engine thread is busy (the MVE player spins against the sound cursor).

**IOP bring-up on `ps2_audio_open()` (exact order):**

1. `sceSifInitRpc(0)`.
2. `SifLoadModule("rom0:LIBSD")` — failures tolerated (LIBSD may be resident).
3. If the audsrv RPC server is not already bound, try **file-based loads** in this order:
   `host:audsrv.irx` → `host:local_assets/audsrv.irx` → `mass:/FALLOUT/audsrv.irx` →
   `hdd0:/FALLOUT/audsrv.irx` (`SifLoadModule`, then a **bounded 1500 ms bind** against the
   audsrv server; if the module loaded but its server never appears, it is unloaded so the
   next path can reuse the name).
4. **Embedded module** (`SifExecModuleBuffer` on `g_audsrv_irx`, generated by CMake) — the
   normal, self-contained path on real hardware.
5. Final bounded bind (2000 ms); no server → `[ps2] audio: no audsrv RPC server, continuing
   without sound` and the engine disables sound.
6. `audsrv_init()`; format candidates: **desired rate → desired×2 (22050→44100) → 48000 →
   44100**.
7. Start the worker thread (16 KiB stack, same priority as the caller, `_gp` inherited),
   initially paused until the engine unpauses.

**Why the file-first order:** under PCSX2 the IOP HLE breaks the raw EE→IOP DMA transfer used
by `SifExecModuleBuffer` (the module "loads" but never runs); `SifLoadModule` of a file is the
only working route there. PCSX2 resolves `host:` to the ELF's directory, so CMake copies
`audsrv.irx` next to the ELF post-build.

**Mixer worker:**

- Chunk = `samples × 4 bytes`, samples clamped to `PS2_AUDIO_MAX_SAMPLES = 2048` (≤ 8 KiB).
- Ring of `PS2_AUDIO_RING_CHUNKS = 4` chunks; the worker mixes ahead, paced by an **absolute
  deadline** (sleep `deadline − now`; a full ring drops the chunk deliberately rather than
  slipping). If the deadline is missed by >4 chunks (resume/disk hitch), the schedule resyncs
  instead of bursting stale audio.
- Pause uses a **sticky semaphore** (never `SleepThread`/`WakeupThread`): an unpause racing
  the pause check can never leave the mixer asleep forever (a lost wakeup = the movie player
  busy-waits forever → black screen).

**Main-thread pump (`ps2_audio_pump`)** — the only SIF-touching side of audio:

- Called from `SDL_RenderPresent()` and from the movie player's wait loops (via the
  `__PS2__` hook in `movie_lib.cc`).
- Rate-clamped to one feed per **8 ms** (frame presents are ≥16 ms apart, so per-frame feeds
  always go through).
- Feeds as many chunks as `audsrv_available()` allows; `audsrv_play_audio` never blocks.
- Only the main thread does SIF; the worker never does (it must stay interruptible).

**Pause/close:** pause mutes the SPU2 ring (`audsrv_stop_audio`) and parks the worker on the
semaphore; close stops the worker (bounded wait for `THS_DORMANT`), then `audsrv_stop_audio` +
`audsrv_quit`.

### 4.3 Input — libpad DualShock 2 (`input_pad.c`)

- **Init:** `SifInitRpc(0)`; `SifLoadModule("rom0:SIO2MAN")`, `("rom0:PADMAN")` (may already be
  resident); `padInit(0)`; `padPortOpen(0, 0, buf)` with a 256-byte, 64-byte aligned DMA buffer.
- **Poll model:** `ps2_input_poll()` is called from the SDL shim's event pump; internally the
  pad is actually read at most once per frame. States handled: stable (read), unstable
  (release all synthetic keys/buttons), disconnect/reconnect, digital→analog mode request.
- **Synthetic events:** the pad posts through the shim (`ps2_sdl_post_key/mouse_move/
  mouse_button/mouse_wheel`), so `kb.cc` / `mouse.cc` / `input.cc` consume ordinary SDL events.

**F8 approved mapping (README-verified):**

| Pad | Effect |
|---|---|
| Left stick | cursor, smooth acceleration curve |
| Right stick | scroll wheel |
| D-pad | arrow keys |
| Cross | left click |
| Circle | right click / interact |
| Square | inventory (I) |
| Triangle | character sheet (C) |
| Select | Pip-Boy (P) |
| Start | main menu / close (Escape) |
| L1 | change active hand (B) |
| L2 | cycle item/weapon mode (N) |
| R1 | attack / enter combat (A); **inside combat**: end-combat attempt (Return) |
| R2 | end turn / skip dialog (Space) |
| R3 | automap (Tab) |
| L3 | toggle the on-screen keyboard |

- **R1 contextual latches on press**: `binding_code()` asks the C-linkage bridge
  `ps2_platform_in_combat()` (implemented in `platform_compat_ps2.cc`, reads `isInCombat()`)
  and latches the SDL scancode so the matching release always uses the same key.
- **Stick math:** deadzone `0.22`, rescale to 0..1; cursor speed =
  `axis × (0.15 + 0.85·|axis|) × 450 px/s`; velocity low-pass `0.35` per poll; wheel max
  `14 steps/s`.
- **Smoke hooks** (`PS2_DEV_SMOKE_TEST`): `ps2_input_smoke_press(buttons)` /
  `ps2_input_smoke_release()` / `ps2_input_smoke_release_pending()` drive the *real* pad path
  (active-high `PAD_*` bits) without hardware.

### 4.4 On-screen keyboard (`ps2_osk.c`)

A modal, pad-driven overlay that types into engine text widgets by injecting ordinary SDL key
events — the engine has no knowledge of it.

- **Opens automatically** when the engine calls `SDL_StartTextInput()` (`beginTextInput()`:
  save-name prompt, editor, file dialogs); **closes** on `SDL_StopTextInput()`. **L3** toggles
  it at any time.
- While open it **owns the whole pad**: D-pad/left stick move the grid cursor (auto-repeat:
  400 ms delay, 110 ms rate; stick deadzone 56), Cross types, Square backspace, Start submits
  (Return), Circle cancels (Escape), L3 closes.
- Letters are injected **uppercase** (shift held, console save-name style); digits, `_`, `-`,
  `.`, space unshifted.

**Grid (10×4):**

```
A B C D E F G H I J
K L M N O P Q R S T
U V W X Y Z 0 1 2 3
4 5 6 7 8 9 _ - . space
```

**Rendering:** software composite straight into `s_fb_rgba` from `ps2_video_present()` (filled
rects + a compact 5×7 bitmap font). No engine buffer is touched; zero cost while closed.

| Geometry constant | Value |
|---|---|
| Panel | 560×178 @ (40, 284); border 2 |
| Grid origin | (60, 344); cell 52×28 |
| Colors | `OSK_GS(r,g,b)` = `0x80<<24 | b<<16 | g<<8 | r` (GS CT32); amber selection, dark cells, light text |

### 4.5 Storage bootstrap — IOP bring-up (`ps2_storage.c`)

Two jobs, both fully tolerance-to-failure:

**1. ELF directory from `argv0`** (`storage_extract_elf_dir`):

- Strips one leading `"` (some loaders quote the path).
- Finds the last `/` or `\`:
  - separator present + `:` present → keep everything up to and including the separator;
  - no separator but `:` present → device root (`mass:` → `mass:/`).
  - deviceless relative path or bare filename → **ignored** (nothing can be probed).
- Backslashes → `/`; ensures a trailing `/`; length cap 64 (`PS2_STORAGE_DIR_MAX`).
- Exposed via `ps2_storage_elf_dir()`; `fs.c` prepends `<ELF dir>` and `<ELF dir>local_assets/`
  to the root list.

**2. Tiered IOP driver bring-up** — only when no loader already provides I/O:

```
guard 1: if mass: (or mass0:) responds  → loader-managed, IOP untouched, return
guard 2: if host: responds              → development I/O (PCSX2/ps2link), skip USB, return
sceSifInitRpc(0)

tier 1  usbd.irx + usbhdfsd.irx                       (classic USB stick)
tier 2  bdm.irx + usbmass_bd.irx + bdmfs_fatfs.irx    (BDM USB, OPL-style)   [only if no mass:]
tier 3  sio2man.irx + mx4sio_bd.irx                   (MX4SIO)               [only if still none]
```

- Modules are **embedded in the ELF** (CMake generates a C array per module from
  `$PS2SDK/iop/irx/<name>.irx`; symbols `g_ps2_<name>_irx[]` + `_size`) and executed from RAM
  with `SifExecModuleBuffer` — no loose `.irx` files ship next to the game data.
- Every load result is logged; the usual failure is "already loaded" (OPL/uLaunchELF preload
  their IOP stack). Failures never stop the boot — the device probe is the real test.
- Final log: `mass: ready` or `mass: not available` (the game may still run from
  `mc0:`/`cdrom0:`/`host:`/`hdd0:`).

**About `fileXio` / `iomanX`:** the port loads **no** `fileXio.irx`/`iomanX.irx`. EE-side file
access goes through ps2sdk's fileio client (newlib/stdio — baked into the binary), and the
IOP drivers above register with the IOP file manager it talks to. When a loader (OPL,
uLaunchELF, ps2link, PCSX2) is in charge, the full stack (including iomanX/fileXio) is already
resident and this whole section is skipped. If a future port ever needs the fileXio-specific
API surface, embed `fileXio.irx` + `iomanX.irx` with the same generated-array pattern.

### 4.6 Filesystem (`fs.c` / `ps2_fs.h`)

Maps relative engine names (`master.dat`, `sound/music/01hub.acm`, `data\…` with backslashes)
onto PS2 storage.

**Limits:** `PS2_FS_MAX_ROOTS=12`, `PS2_FS_ROOT_MAX=40` (chars per root), `PS2_FS_PATH_MAX=256`,
`PS2_FS_DAT_BUFSIZE=64 KiB`.

**Root order** (dynamic ELF-relative roots first, then the fixed fallback chain):

```
<ELF dir>/                    (from argv[0], media deployments)
<ELF dir>/local_assets/       (dev layout)
mass:/FALLOUT/                (USB stick / MX4SIO in a FALLOUT folder)
mass:/                        (USB stick, data at root)
mass0:/FALLOUT/               (numbered mass device)
cdrom0:/                      (CD/DVD)
cdrom0:/FALLOUT/
hdd0:/FALLOUT/                (internal HDD PFS partition)
host:local_assets/            (PCSX2 hostfs / ps2client dev layout)
host:../local_assets/         (ELF in build_ps2/ under the repo)
host:/                        (PCSX2 host root)
```

- A root is accepted **only when both `MASTER.DAT` and `CRITTER.DAT` open there** (accepting a
  root with only one file would just move the failure deeper into the boot).
- Log lines: `[ps2] fs: data root N = '…'` per root; `[ps2] storage: asset root detected at …`
  for root 0; if nothing is found, a warning plus a diagnostic dump of `host:` entries.

**`ps2_fs_fopen/resolve` algorithm:**

1. Normalize: backslashes→slashes, leading `./` stripped.
2. **Save remap first** (`ps2_save_remap`); hits are canonicalized and opened directly (§4.7).
3. If the path carries a device (`dev:`), canonicalize it in place and open.
4. Otherwise try every accepted root in order.

**`ps2_fs_canonicalize` (case-insensitive, per component):**

- Splits `dev:` / `dev:/` mount, then each `/`-separated component:
  - `.` skipped; `..` kept **verbatim** without a directory match;
  - otherwise `opendir` + `readdir` with `strncasecmp` **exact length** match (device roots are
    accepted both as `mass:` and `mass:/` via a trailing-slash `opendir` retry);
- A missing **last** component is kept with the caller's spelling only when `writable`
  (file creation); any other missing component fails the resolution.

> **⚠ Deliberate omission — do not add a direct ALL-CAPS `open()` probe.** The `readdir` scan
> above already resolves existing names case-insensitively on every device class the port uses
> (FAT32, ISO9660, host, PFS). A blind uppercase `open()` could only add a *failed* open for
> genuinely missing files — and under PCSX2 a failing `open()` at runtime corrupts the hostfs
> HLE (SIF RPC state; the VM crashes ~100 ms later with a TLB miss, e.g. pc `0x291658`). This
> was bisected in F9. The literal uppercase probes in `fs_validate_root()` (MASTER.DAT /
> CRITTER.DAT) are safe because they happen once, at boot, on paths that always exist when
> the root is valid.

**Open behavior (`fs_do_open`):** non-writable `.dat` files get a 64 KiB stdio block buffer
(`setvbuf`) — the big datafiles are read in large sequential blocks.

**F5.2 smoke counters:** `ps2_fs_stat_fopen_calls`, `ps2_fs_stat_canonicalize_calls`
(consumed by the smoke harness to quantify map-load I/O).

**4.6.4 DAT/stream layer (`db.cc`, `__PS2__`)** — the IOP has no page cache and no write-back
cache; every stdio call is a synchronous SIF round trip. Guards:

- **Write aggregation:** streams opened for writing carry `PS2_DB_WRITE_FLAG` (0x100); writes
  accumulate in RAM (initial 64 KiB, doubling up to `PS2_DB_WRITE_BUF_MAX = 1 MiB`; single
  writes ≥1 MiB bypass) and flush with one `fwrite` at close, before seeks, and before reads.
- **Slurp read-only loose files** into RAM (maps, overrides) — removes the dominant cost of
  map/art loading.
- **DAT entries:** uncompressed entries (kind 32) are read once into RAM; compressed chunked
  entries (kind 64) are fully decoded once (`db_slurp_dat_entry_chunked`).
- **1-slot decoded-entry reuse:** a fully-decoded entry buffer ≤ `PS2_DB_DECODE_CACHE_MAX`
  (1 MiB) is parked on close and reused if the next open decodes the same
  database/offset/length/kind (art size probe → immediate load pattern).

### 4.7 Saves (`ps2_save.c` / `ps2_save.h`)

The engine treats a save slot as a directory (`SAVEGAME\SLOTxx\` with `SAVE.DAT`, `*.SAV`,
`AUTOMAP.SAV`). Every relative path is run through `ps2_save_remap()` before resolution:

```
data\SAVEGAME                    → <root>/
data\SAVEGAME\SLOT01\X.SAV       → <root>/<slot dir>/X.SAV
```

- Match is case-insensitive; `data\` prefix optional; `SLOTnn` must be 01..10
  (`PS2_SAVE_SLOT_COUNT=10`); remainder separators are normalized to `/`.

**Device selection (probed once, first use):**

| Priority | Root | Slot directory name |
|---|---|---|
| 1 | `mc0:/` | `FALLOUT1_S01` … `FALLOUT1_S10` |
| 2 | `mc1:/` (mc0 absent/unformatted) | `FALLOUT1_S01` … `FALLOUT1_S10` |
| 3 | `<first data root>saves/` (host fallback, always used by dev/smoke builds) | `SLOT01` … `SLOT10` |

Limits: `PS2_SAVE_ROOT_MAX=80`, `PS2_SAVE_PATH_MAX=256`.

**PS2 browser metadata** (`ps2_save_slot_ensure_meta`, triggered from `compat_mkdir()` when the
slot directory is created; written once, never rewritten):

- **`icon.sys` — 964 bytes:** standard layout (magic, amber background gradient corners,
  default light setup, 68-byte Shift-JIS title at `0xC0` — "Fallout" / "- Slot NN" with the
  line break before byte 7 —, three icon filenames at `0x104`).
- **`icon.icn` — 928 bytes:** "PS2 Icon Format v0.5" geometry — 36 f16 vertices (24 bytes
  each) carrying amber vertex colors, one static animation frame, RLE-compressed uniform white
  texture so the mesh shows only its vertex colors.

**`compat_remove` / slot wipe workaround:** ps2sdk's `fileio.h` documents `fioRemove` as broken
(its RPC dispatch falls through into "mkdir", so every successful `remove()` leaves an empty
directory with the same name). The port calls `remove(path)` **and then `rmdir(path)`
immediately**; the `rmdir` fails harmlessly when the phantom did not appear.
`ps2_save_slot_wipe(slot)` (dev helper, used by smoke) applies the same pattern to every file
plus the slot directory.

### 4.8 SDL2 subset (`sdl/sdl_shim.c` + `sdl/SDL.h`)

**Implemented API surface** (the engine is compiled against this header only):

- **Init/lifecycle:** `SDL_InitSubSystem`, `SDL_WasInit`, `SDL_QuitSubSystem`, `SDL_Quit`,
  `SDL_SetHint`, `SDL_GetTicks`, `SDL_Delay`, `SDL_LogMessageV`, `SDL_ShowSimpleMessageBox`.
- **Window/renderer/texture:** `SDL_CreateWindow/DestroyWindow/SetWindowTitle`,
  `SDL_CreateRenderer/DestroyRenderer/RenderSetLogicalSize`,
  `SDL_CreateTexture/DestroyTexture/QueryTexture/UpdateTexture`,
  `SDL_RenderClear/RenderCopy/RenderPresent`.
- **Surfaces (8bpp indexed):** `SDL_CreateRGBSurface`, `SDL_CreateRGBSurfaceWithFormat`,
  `SDL_FreeSurface`, `SDL_LockSurface`, `SDL_UnlockSurface`, `SDL_BlitSurface`,
  `SDL_SetPaletteColors`, `SDL_SetSurfacePalette`.
- **Events/input state:** `SDL_PollEvent`, `SDL_PumpEvents`, `SDL_FlushEvents`,
  `SDL_StartTextInput`, `SDL_StopTextInput`, `SDL_GetModState`,
  `SDL_GetRelativeMouseState`, `SDL_SetRelativeMouseMode`, `SDL_ShowCursor`,
  `SDL_CreateSystemCursor`, `SDL_GetCursor`, `SDL_SetCursor`, `SDL_FreeCursor`.
- **Timers:** `SDL_AddTimer`, `SDL_RemoveTimer` (16 slots, pumped from the event path on the
  main thread; callback returning 0 unregisters).
- **Strings:** `SDL_strcasecmp`, `SDL_strncasecmp`, `SDL_strupr`, `SDL_strlwr`, `SDL_itoa`,
  `SDL_strdup`, `SDL_free`.
- **Audio:** `SDL_OpenAudioDevice`, `SDL_CloseAudioDevice`, `SDL_PauseAudioDevice`,
  `SDL_NewAudioStream`, `SDL_AudioStreamPut`, `SDL_AudioStreamGet`, `SDL_FreeAudioStream`,
  `SDL_MixAudioFormat`.

**Key mechanics:**

- **Opaque objects hold only what the engine uses.** `SDL_Window` = title; `SDL_Renderer` =
  logical size; `SDL_Texture` = format/size/pixels/pitch.
- **Surface storage trick:** the engine creates an "RGB888 streaming texture surface" only to
  blit the 8-bit screen into it before upload. The shim stores that surface as **8bpp indexed
  data too**, so the blit is a plain memory copy and `SDL_UpdateTexture` just records the
  pointer (zero-copy); presentation forwards it to the video backend.
- **Event queue:** fixed ring of 64 `SDL_Event` (`EVENT_QUEUE_SIZE`), drop-oldest on overflow;
  injected via `ps2_sdl_push_event` (timestamps filled if zero). Builders in
  `ps2_sdl_internal.h`: `ps2_sdl_post_key`, `_mouse_move`, `_mouse_button`, `_mouse_wheel`,
  `_window_event`. The first pump posts `FOCUS_GAINED` + `EXPOSED` (the engine treats itself
  as inactive until focus arrives).
- **Mouse:** relative accumulators consumed by `SDL_GetRelativeMouseState`; absolute position
  clamped to 640×479; button mask maintained from synthetic events.
- **Text input:** a simple flag; `SDL_StartTextInput/StopTextInput` is what the OSK listens to.
- **`SDL_Delay`:** absolute-deadline wait with `DelayThread(1000)` yields every millisecond —
  a bare busy-wait starves the audio worker on the non-preemptive EE kernel (see `movie_lib`
  deadlock rationale).
- **`SDL_RenderPresent`** = palette forward (if dirty) → `ps2_video_present(texture->pixels…)`
  → `ps2_audio_pump()`. Feeding audio here guarantees progress even on frames where the
  engine skips the event pump.
- **Audio:** `SDL_OpenAudioDevice` wires the engine callback into `ps2_audio_open`
  (`PS2_SDL_AUDIO_DEVICE_ID = 1`); `SDL_PauseAudioDevice` → `ps2_audio_pause`;
  `SDL_AudioStream` performs the rate/format conversion the engine requests (fixed-point),
  so the mixer callback always produces device-format PCM.

### 4.9 Platform compatibility (`platform_compat_ps2.cc`)

Replaces upstream `src/platform_compat.cc` (excluded by CMake on PS2) with the same API,
resolving paths through `fs.c`. Functions:

| Function | Behavior on PS2 |
|---|---|
| `compat_stricmp/strnicmp/strupr/strlwr/itoa` | via `SDL_*` helpers |
| `compat_splitpath/makepath` | Windows-style path split/join (drive `//x:` aware) |
| `compat_read/write/lseek/tell/filelength` | POSIX passthrough on the given handle |
| `compat_fopen` | `ps2_fs_fopen` |
| `compat_mkdir` | resolve (allow missing last) → `mkdir(0755)` → **`ps2_save_slot_ensure_meta`** |
| `compat_remove` | resolve → `remove()` **+ `rmdir()`** (fioRemove phantom workaround) |
| `compat_rename` | resolve old (exact) + new (allow missing) → `rename` |
| `compat_timeGetTime` | `ps2_time_ms()` |
| `compat_windows_path_to_native` | `\` → `/` |
| `compat_resolve_path` | save remap first, else canonicalize in place / against roots (best effort) |
| `compat_strdup` | `SDL_strdup` |
| `getFileSize(FILE*)` | ftell/fseek-based (no fileno) |
| `ps2_platform_in_combat()` | `extern "C"` bridge returning `isInCombat()` (R1 context) |

### 4.10 Memory

- **Budget:** 32 MB RDRAM total. The port relies on the engine's own caps plus measured
  headroom; no custom arena allocator was needed (F7 conclusion).
- **Instrumentation:**
  - `ps2_mem` (F7): newlib `mallinfo()` (`uordblks` used, free) + `sbrk(0)` **high-water mark**
    from the first snapshot (never shrinks on this allocator). Prints `[ps2][smoke] mem:` lines.
    Release builds are inert (only the smoke harness calls it).
  - `plib/gnw/memory.cc` (smoke): engine allocator current/max counters
    (`ps2_mem_engine_cur_kb/max_kb/report`).
- **Engine cache caps the port leans on** (config keys, upstream defaults):

  | Cache | Cap | Where |
  |---|---|---|
  | Art cache | `art_cache_size = 8` (MB → `8 MiB`) | `gconfig.cc` default, `art.cc` `cache_init` |
  | SFX cache | `cache_size = 448` (KB → `448 KiB`) | `sounds` section, `gsound.cc` |
  | Decoded DAT entry reuse | 1 MiB (1 slot) | `db.cc` `PS2_DB_DECODE_CACHE_MAX` |
  | Write aggregation buffer | 64 KiB → 1 MiB | `db.cc` `PS2_DB_WRITE_BUF_*` |
  | DAT stdio buffer | 64 KiB | `fs.c` `PS2_FS_DAT_BUFSIZE` |
  | Audio ring | 4 × ≤8 KiB chunks | `audio_audsrv.c` |
  | Video buffers | ~1.5 MB BSS + ~1.2 MB VRAM | `video_gs.c` |

- **Smoke guardrails (F7):** during the world tour the heap must **plateau** (≤
  `SMOKE_TOUR_PLATEAU_KB = 2048` KiB growth between settle windows) and stay under
  `SMOKE_TOUR_CEIL_KB = 20480` KiB (20 MB). Reference worst-case across tour/dialogue/combat
  measured ≈ 17.6 MB — comfortably inside the 32 MB budget.

### 4.11 Time (`ps2_time.c`)

`GetTimerSystemTime()` on the EE bus clock (`147456000 Hz`): `ps2_time_ms()` (uint32,
wraps ≈49.7 days, SDL-like) and `ps2_time_us()` (uint64, no wrap). If the boot banner shows a
wildly wrong `timebase` value, the divisor is the single place to fix.

---

## 5. Build system

### 5.1 Toolchain — `cmake/toolchain-ps2.cmake`

- Requires `PS2SDK` and `PS2DEV` env vars (fatal otherwise).
- `CMAKE_SYSTEM_NAME Generic`, processor `mips`; `CMAKE_TRY_COMPILE_TARGET_TYPE
  STATIC_LIBRARY` (replaces the removed `CMakeForceCompiler`, CMake 3.20+ / 4.x).
- Compilers: `mips64r5900el-ps2-elf-gcc` / `-g++`.
- **CFLAGS:** `-I$PS2SDK/ee/include -I$PS2SDK/common/include -I$PS2DEV/gsKit/include
  -I$PS2SDK/ports/include -D_EE -DPS2 -D__PS2__ -G0`
  (`-G0`: no GP-relative addressing limits; `-D__PS2__` drives all engine fixes).
- **LDFLAGS:** `-L$PS2SDK/ee/lib -L$PS2DEV/gsKit/lib -L$PS2SDK/ports/lib
  -Wl,-zmax-page-size=128 -T$PS2SDK/ee/startup/linkfile`.
- `CMAKE_FIND_ROOT_PATH` = PS2DEV/PS2SDK/ports (library+include modes ONLY); EE pkg-config
  pinned. Sets `PS2=TRUE / PLATFORM_PS2=TRUE / EE=TRUE` (which flips on the CMake PS2 block).

### 5.2 CMake integration (`CMakeLists.txt`, PS2 block)

- **Options:** `PS2_BUILD` (set automatically by the toolchain; can be set manually with
  `-DPS2_BUILD=ON`), `PS2_DEV_SMOKE_TEST` (dev-only harness).
- **Platform sources** (13 files, §2.1) + when smoke: `src/game/ps2_smoke.cc/.h` and
  `target_compile_definitions(PS2_DEV_SMOKE_TEST=1)`.
- **IRX embedding** (no external tooling — pure CMake): `file(READ … HEX)` + regex → generated
  C files in the build dir:
  - `audsrv_irx_data.c` → `g_audsrv_irx[]` / `_size`;
  - one file per storage module (`usbd usbhdfsd bdm usbmass_bd bdmfs_fatfs sio2man mx4sio_bd`)
    → `g_ps2_<name>_irx[]` / `_size`.
- **`audsrv.irx` copy** next to the ELF via `add_custom_command(POST_BUILD)` (PCSX2 `host:`
  path).
- **Include priority:** `target_include_directories(BEFORE PRIVATE "src/platform/ps2/sdl")` so
  the SDL subset wins over any other SDL.h; platform dir added after.
- **Compile options:** `-Wcast-align=strict -fno-strict-aliasing` (engine-wide); Release adds
  `-ffast-math` (R5900 has no FP64 hardware). PS2 platform files additionally get
  `-fsingle-precision-constant -Wno-cast-align` — deliberately **not** applied to upstream code
  (it breaks e.g. `std::clamp<double>` overload resolution in `game/options.cc`).
- **Link:** `gskit dmakit graph pad audsrv m` (+ static `adecode`/`fpattern` via upstream
  FetchContent). No SDL2, no `platform_compat.cc`, no `winmain.cc` when `PS2=TRUE`.
- **Artifact:** `OUTPUT_NAME fallout1-ce` + `SUFFIX .elf` → `build_ps2*/fallout1-ce.elf`
  (PCSX2 boots `.elf` directly).

### 5.3 Docker build image — `os/ps2/build/Dockerfile`

```dockerfile
FROM ps2dev/ps2dev:latest   # cross toolchain + ps2sdk + gsKit + ps2-packer (Alpine)
RUN apk add --no-cache cmake make ninja git ca-certificates   # + git/CA for FetchContent
```

```sh
# once:
docker build -t fallout-ce-ps2-build os/ps2/build
# one-shot build (README):
docker run --rm -v "$(pwd):/src" -w /src fallout-ce-ps2-build sh -c \
  "cmake -B build_ps2 -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-ps2.cmake \
         -DCMAKE_BUILD_TYPE=Release && cmake --build build_ps2 -j\$(nproc)"
```

The smoke/release scripts use a **persistent** container named `fps2-build`
(`docker start fps2-build` + `docker exec …`); create it once with the same mounts and a
long-running command (e.g. `sleep infinity`). Note: build trees created inside the container
are **root-owned** — scripts create symlinks/files via `docker exec`, not from the host.

Build directories: `build_ps2` (dev), `build_ps2_smoke` (harness), `build_ps2_release`
(shipping).

### 5.4 Release pipeline — `tools/make_dist.sh`

1. Configure + build `build_ps2_release` (**Release, `PS2_DEV_SMOKE_TEST=OFF`**) in the
   persistent container.
2. `mips64r5900el-ps2-elf-strip -o dist/FALLOUT/FALLOUT.ELF build_ps2_release/fallout1-ce.elf`.
3. Verify bundle: `FALLOUT.ELF`, `SYSTEM.CNF`, `README_PS2.txt` present and non-empty;
   **no `SUITE_FINISHED` strings** in the ELF (smoke harness absent); `SYSTEM.CNF` contains
   `BOOT2 = cdrom0:\FALLOUT.ELF;1`.
4. Print tree + SHA-256 of the stripped ELF.

Reference rc1 numbers: unstripped 6 837 564 B → stripped **3 881 628 B**; SHA-256
`3911a492…76fb475`. The release zip was assembled with `python3 -m zipfile` (host has no
`zip`), `FALLOUT/` at the archive root (extracts to `mass:/FALLOUT/`), and published with
`gh release create ps2-v1.0.0-rc1 --prerelease` (tag prefixed with `ps2-` to keep the upstream
PC tags `v1.0.0`/`v1.1.0` untouched).

---

## 6. Smoke-test harness & validation environment

### 6.1 Design (`src/game/ps2_smoke.cc`, `ps2_smoke.h`, dev-only)

Compiled with `PS2_DEV_SMOKE_TEST=ON`. After engine init, `gnw_main()` hands control to the
harness: the intro movies and main menu are skipped and a scripted sequence exercises the whole
UI hierarchy. It logs `[ps2][smoke]` lines to the EE console (PCSX2 emulog) and terminates the
ELF with `exit()`, so the runner needs no interaction.

**Frontend phase** (real main-menu loop driven with synthetic key taps): CREDITS screen,
LOAD GAME with an empty save list, NEW GAME reaching the character generator (premade cycling
+ clean cancel), EXIT. **The intro movie is intentionally not exercised.**

**In-game states (1–13):**

| # | State | What it covers |
|---|---|---|
| 1 | map render | dude present, frames advance, script VM stability |
| 2 | click-to-move | neighbour-tile clicks through the real SDL path; catches movement stutters |
| 3 | inventory | open (I) / close (Escape) |
| 4 | Pip-Boy | open (P) / close (Escape) |
| 5 | options | pause menu + PREFERENCES: the four sliders (master/music/SFX/…) |
| 6 | character sheet | open (C) / close |
| 7 | Skilldex | open (S) / close |
| 8 | map streaming | `JUNKENT.MAP` timed stream-out/in |
| 9 | save/load | OSK save-name typing into slot 1, save, load, wipe (F6 path) |
| 10 | world tour | warps through the heaviest maps with heap plateau/ceiling checks (F7) |
| 11 | dialogue | warps to a voiced-head NPC map; talking-head + voice-over counters (F7) |
| 12 | combat | starts a real melee via the crosshair click, drives turns (F7) |
| 13 | pad hotkeys | R1 context (attack vs end-combat), F8 dispatch; then the terminal verdict state `exit()`s the ELF |

Start map `V13ENT.MAP`; stream map `JUNKENT.MAP`.

**Timing / safety constants:** watchdog `SMOKE_WATCHDOG_MS = 300 000`; stall detector threshold
`SMOKE_STALL_MS = 1500` (worst frame gap, reported in the verdict); tour settle 1000 ms,
plateau 2048 KiB, ceiling 20480 KiB; dialogue settle 800 ms / max 22 s / 32 candidates / 3
tries; combat settle 900 ms / max 20 s / 2500 ms drive / 5 clicks; hotkeys detect 700 ms /
close 800 ms / max 6 s; OSK script 64 events × 200 ms; frontend step 20 s; OSK deadline 60 s.

**Verdict format:**

```
summary: frontend(credits=… load=… newgame=…) map=… walk=… inventory=ok pipboy=ok
         options=ok charsheet=ok skilldex=ok streaming=… save-load=… world-tour=…
         dialogue=… combat=… hotkeys=…
SUITE_FINISHED_SUCCESS: All game subsystems verified!          (or)
SUITE_FINISHED_WITH_ERRORS: N failure(s)
```

**Smoke-only code paths** (all under `PS2_DEV_SMOKE_TEST`): takeover in `game/main.cc`;
synthetic pad injection (`ps2_input_smoke_*`); save layer forced to host (`SLOT01…`);
`ps2_mem`/engine-allocator counters; fs I/O counters; profiling in art/map/memory/pipboy/
lip_sync/object/scripts/intextra/intrpret; dialogue/save hooks in `loadsave.cc`.

### 6.2 Runner — `tools/run_smoke.sh [timeout_seconds, default 240]`

```
1. docker start fps2-build; cmake configure+build build_ps2_smoke
   (-DPS2_DEV_SMOKE_TEST=ON) and build fallout1-ce.elf
2. ensure build_ps2_smoke/local_assets → ../local_assets symlink
   (created from inside the container: root-owned tree; PCSX2 does not resolve "..",
   and host: = the ELF's directory, so the data must be reachable from the build dir)
3. flatpak kill net.pcsx2.PCSX2; back up local_assets/DATA/MAPS (the suite writes map
   state) to /tmp/opencode/smoke_maps_backup
4. record the current emulog line count (START_LINE), launch
   ~/.local/bin/pcsx2-qt build_ps2_smoke/fallout1-ce.elf in the background
5. poll the emulog every 2 s: look for SUITE_FINISHED* or "[ps2] boot failed";
   if the log shrinks (PCSX2 truncated/reopened it), reset START_LINE
6. flatpak kill net.pcsx2.PCSX2; restore DATA/MAPS
7. check the host save slot local_assets/saves/SLOT01: SAVE.DAT non-empty, *.SAV present,
   icon.sys magic "PS2D", icon.icn non-empty → SAVE_LOAD_FILES_HOST_OK
8. print filtered report (smoke lines + osk/storage/fs + MAP/WORLD MAP lines)
9. verdict: SUCCESS → exit 0; else FAILED → 1; TIMEOUT → 2
```

**Environment** (may need adjusting per machine):

| Variable / path | Value |
|---|---|
| `EMULOG` | `$HOME/.var/app/net.pcsx2.PCSX2/config/PCSX2/logs/emulog.txt` (truncated on each PCSX2 start) |
| `PCSX2_BIN` | `$HOME/.local/bin/pcsx2-qt` |
| `FLATPAK_ID` | `net.pcsx2.PCSX2` |
| `SMOKE_BUILD_CONTAINER` | `fps2-build` |

**PCSX2 host filesystem:** `host:` maps to the ELF's directory; relative paths with `..` are
not resolved (hence the `local_assets` symlink). This is a real display session — "headless"
here means *unattended* (no interaction), not "no video output".

**Known PCSX2 quirks (baked into the design):**

| Quirk | Mitigation |
|---|---|
| IOP HLE breaks the EE→IOP DMA route of `SifExecModuleBuffer` (module loads, never runs) | audio loads `audsrv.irx` from file first; CMake copies it next to the ELF |
| A failing `open()` at runtime corrupts the hostfs HLE (SIF RPC state → crash ~100 ms later) | no direct ALL-CAPS probe opens in the resolution path (§4.6) |
| `host:` exists in every PCSX2 boot | storage bring-up skips USB/MX4SIO tiers (guards) |
| The emulog is truncated/reopened on PCSX2 start | runner detects a shrinking log and resets its start offset |

**Reference run (F9 suite, rc1 code):** ≈139 s, 1078 frames presented, worst frame gap
304 ms, watchdog 300 s — verdict `SUITE_FINISHED_SUCCESS` (log under `/tmp/opencode/`).

---

## 7. Porting checklist for fallout2-ce (sibling folder)

### 7.1 Reuse verbatim

Copy wholesale into a `ps2` branch of fallout2-ce and adjust only names/paths listed in §7.2:

- `src/platform/ps2/` — the entire folder (platform layer is engine-agnostic except
  `platform_compat_ps2.cc`, which needs the same engine API contract).
- `cmake/toolchain-ps2.cmake` — unchanged.
- `os/ps2/build/Dockerfile` — unchanged.
- `tools/run_smoke.sh`, `tools/make_dist.sh`, `tools/check_no_assets.sh` — adjust names
  (`fallout2-ce.elf`, slot dir) as noted.
- `dist/FALLOUT/` skeleton (`SYSTEM.CNF` boots `FALLOUT.ELF`).

### 7.2 Adaptation points (exact)

| Area | In Fallout 1 port | For fallout2-ce |
|---|---|---|
| Data root probes | `fs_validate_root()` requires `MASTER.DAT` + `CRITTER.DAT` | verify FO2 ships both (plus `patch000.dat`); keep the two-file rule, adjust names if needed |
| Data folder names | `mass:/FALLOUT/`, `hdd0:/FALLOUT/`, `mass:/FALLOUT/audsrv.irx` | use `FALLOUT2/` so both games can coexist on one USB stick |
| Save slot dirs | `FALLOUT1_S01…S10`; icon.sys title "Fallout" / "- Slot NN" | `FALLOUT2_S01…S10`; title "Fallout 2" |
| Save remap prefix | engine writes `data\SAVEGAME\SLOTxx` | same engine layout (verify in FO2 `loadsave.cc`; FO2 CE uses the same `SAVEGAME` folder) |
| Smoke maps | `V13ENT.MAP` → `JUNKENT.MAP` | pick FO2 equivalents (e.g. starting map + a heavy map) |
| Smoke dialogue/combat staging | voiced-head map + melee start | FO2 equivalents; keep the state machine |
| Engine fixes to re-apply | db.cc aggregation/slurp, intrpret `Program` alignment, scripts/object id bitmap, map refresh cadence, movie_lib yields | locate each corresponding file in FO2 CE and re-apply the same `__PS2__` patches (FO2 CE shares the lineage) |
| CMake | PS2 block sources/options/libs | copy block; rename output to `fallout2-ce` |
| Memory | caps & smoke limits (F7) | FO2 is heavier (bigger worldmap/cities): re-run the F7 audit, re-tune `SMOKE_TOUR_*` limits |
| SDL subset | 750-line `SDL.h` + shim | build FO2 first and diff compiler errors; extend the shim for any missing call (check `SDL_GetWindowSurface`, icon/clipboard APIs are not needed by FO1) |

### 7.3 Ordered bring-up

1. Branch `ps2` in the fallout2-ce repo; copy §7.1 files.
2. Apply the CMake PS2 block + exclusions (`winmain.cc`, `platform_compat.cc`, SDL2).
3. Copy `local_assets/` FO2 data; build with the Docker image.
4. First target: banner + `boot ok` + `fs: data root …` + reaching `gnw_main` (no smoke).
5. Bring up video (first present log + no crash), then input, then saves (host fallback).
6. Port the smoke harness: start with frontend + states 1–4, then the F7 states.
7. Audio last (it degrades cleanly): verify `rom0:LIBSD` + `host:audsrv.irx` path under PCSX2.
8. Run `tools/run_smoke.sh`, chase `SUITE_FINISHED_SUCCESS`, then `make_dist.sh`.

### 7.4 Risks / open items (inherited from FO1)

- **Real-hardware USB/BDM/MX4SIO path is untested on silicon** — PCSX2 always exposes `host:`,
  which skips the whole tiered bring-up. Validate on a console (raw-ELF boot via uLaunchELF,
  OPL, MX4SIO) before promising USB support.
- **Non-preemptive EE kernel** — any new busy-wait must yield, or the mixer (and thereby the
  movie player) dies. Grep for spin loops when porting new code.
- **PCSX2 hostfs intolerance** — never add failed `open()` probes in hot paths.
- **Memory** — FO2's heavier maps/art deserve a fresh watermark audit before raising any cap.

---

## 8. Appendices

### A. Constants quick reference

| Constant | Value | File |
|---|---|---|
| Screen in/out | 640×480 in → 640×448 out, NTSC 60 Hz interlaced | `video_gs.c` |
| Palette entries | 256 (CT32, alpha clamp 0x80) | `video_gs.c` |
| Audio chunk / ring | ≤2048 samples (~46 ms @22050) / 4 chunks | `audio_audsrv.c` |
| Audio worker stack | 16 KiB | `audio_audsrv.c` |
| Audio pump clamp | 8 ms | `audio_audsrv.c` |
| Pad deadzone / speed | 0.22 / 450 px/s / smoothing 0.35 / wheel 14 st/s | `input_pad.c` |
| OSK grid / geometry | 10×4, cell 52×28, panel 560×178 @ (40,284) | `ps2_osk.c` |
| OSK auto-repeat | 400 ms delay / 110 ms rate / deadzone 56 | `ps2_osk.c` |
| FS roots / root len / path len | 12 / 40 / 256 | `fs.c` |
| DAT stdio buffer | 64 KiB | `fs.c` |
| Storage dir max | 64 | `ps2_storage.c` |
| Save slots / icon sizes | 10 / icon.sys 964 B / icon.icn 928 B | `ps2_save.h` |
| DB write buf | 64 KiB → 1 MiB | `plib/db/db.cc` |
| DB decode cache | 1 MiB, 1 slot | `plib/db/db.cc` |
| Event queue / timers | 64 events / 16 slots | `sdl/sdl_shim.c` |
| obj id bitmap | 32 768 bits, rebuild every 128 calls | `scripts.cc` |
| Timer clock | 147 456 000 Hz | `ps2_time.c` |
| Smoke watchdog / stall | 300 000 ms / 1500 ms | `ps2_smoke.cc` |

### B. Log prefixes (for emulog grep)

| Prefix | Source |
|---|---|
| `[ps2] boot ok (F1), timebase …` | `boot.c` |
| `[ps2] storage: …` | `ps2_storage.c` (`ELF argv0`, `ELF directory`, tier loads, `mass: ready/not available`) |
| `[ps2] fs: …` / `[ps2] storage: asset root detected at …` | `fs.c` |
| `[ps2] save: …` | `ps2_save.c` (device/fallback, metadata, wipe) |
| `[ps2] pad: …` | `input_pad.c` |
| `[ps2] osk: …` | `ps2_osk.c` |
| `[ps2] audio: …` | `audio_audsrv.c` (module loads, audsrv_init, format) |
| `[ps2] video: …` / `[ps2] present phases …` | `video_gs.c` |
| `[ps2][smoke] …` + `SUITE_FINISHED_*` | `ps2_smoke.cc` (verdict) |

### C. Command cheat sheet

```sh
# build (Docker)
docker run --rm -v "$(pwd):/src" -w /src fallout-ce-ps2-build sh -c \
  "cmake -B build_ps2 -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-ps2.cmake \
         -DCMAKE_BUILD_TYPE=Release && cmake --build build_ps2 -j\$(nproc)"

# smoke (builds smoke ELF, boots PCSX2, greps verdict, exits 0/1/2)
tools/run_smoke.sh 300

# release bundle (strip + verify) and zip
tools/make_dist.sh
python3 -m zipfile -c dist/fallout1-ce-ps2-vX.Y.Z.zip dist/FALLOUT

# emulog tail
tail -n +1 ~/.var/app/net.pcsx2.PCSX2/config/PCSX2/logs/emulog.txt | grep "\[ps2\]"
```

### D. Legal hygiene

`tools/check_no_assets.sh` refuses to track game assets by extension
(`.dat .frm .acm .mve .map .pro .int .msg .sve .cfg`) or directory (`local_assets/`, `data/`,
`mods/`, `saves/`, `build_ps2/`). Run it before every commit that touches those areas; CI can
call it directly. Game data lives only in the gitignored `local_assets/`.
