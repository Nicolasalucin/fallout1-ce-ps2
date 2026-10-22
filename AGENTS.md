# fallout1-ce-ps2 — Agent Guide

This is the complete contract for working on the Fallout 1 PS2 port. Read it
before making any change. It combines the PS2 architecture rules extracted
from **ps2-forge** (Ijtihed) with the mandatory rules of this project.

---

## 1. PS2 Architecture Rules (from ps2-forge)

### Memory & Alignment
- **DMA buffers** (EE → IOP, EE → GS, IOP → EE) must be **16-byte aligned**.
- **Scratchpad / DMA lists / gsKit packet buffers** should be **64-byte aligned**.
- Use `__attribute__((aligned(64)))` for static bounce buffers; if a pointer
  is misaligned at runtime, copy to the bounce buffer before `SifExecModuleBuffer`
  or `dmaKit_send_*`.
- The EE has **32 MB RDRAM**. Large arrays go in `.bss` (`static`), never
  `malloc`/`new` in hot paths.

### FPU / MIPS R5900
- **Single-precision (float) is fast**; double-precision (`double`) is emulated
  and extremely slow. Compile with `-mno-check-zero-division` (the PS2 has no
  integer divide trap; GCC's division-by-zero check traps in software).
- Stack size is set at link time: `-Wl,--defsym,_stack_size=0x100000`
  (1 MB). This gives the engine, VFS and IOP loaders enough room for deep
  call chains on real hardware.

### Caches
- **Data cache (D-cache)**: after modifying a buffer that the IOP/GS will read
  (or vice-versa), call `FlushCache(0)` to push dirty lines to RDRAM, then
  `SyncD()` to ensure completion before kicking DMA.
- **Instruction cache (I-cache)**: after loading an IRX module or patching
  code, call `FlushCache(2)` then `SyncD()`.
- gsKit's internal DMA helpers do this for you; only manual DMA needs explicit
  cache ops.

### I/O (Real PS2 Rules)
- **Never use `opendir()`/`readdir()`/`closedir()` on a device root**.
  On PCSX2's hostfs HLE, a failed `opendir()` on a missing device can wedge
  the RPC channel. Use **`stat()` + `dopen()`/`dclose()`** (or, on this
  ps2sdk's newlib port, `stat()` + `open()`/`close()`) — the plain directory
  open is enough to ask fileio whether a mount point exists.
- **FAT32 durability**: `fflush()` alone does **not** update the directory-entry
  size. The file is only committed when the **last descriptor is closed**.
  Use the `dup2()` close/reopen dance:
  ```c
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);
  if (fd != STDOUT_FILENO && fd != STDERR_FILENO) close(fd);
  ```
  Never leave `fd 1` or `fd 2` vacant — the next engine `open()` would reuse
  them and corrupt a game asset.
- **SIF RPC guard**: the fileio `fioRead()` completion needs both
  `_fio_intr` and `_fio_read_intr` in the IRX set. Verify with
  `mips64r5900el-ps2-elf-nm libkernel.a | grep _fio_`.

### Graphics (gsKit)
- After `gsKit_init_global()`, **never override** `gsGlobal->Mode/Width/Height/Field`;
  it desyncs the display and blanks the screen.
- VRAM allocations: use `GSKIT_ALLOC_USERBUFFER` and check for
  `GSKIT_ALLOC_ERROR` (value 0). `GS_VRAM_BLOCKSIZE_256` is deprecated and
  emits a warning.
- Text rendering uses an alpha-tested font atlas (one textured quad per glyph);
  don't draw text with per-pixel rects.
- For grids / cellular automata / software renderers: write colours into one
  RGBA buffer and blit it with `gsKit_texture_upload` + a single sprite draw,
  not thousands of rect calls.

### Build System
- One ELF per build. Link: `-lgskit_toolkit -lgskit -ldmakit -laudsrv -lpad -lm -lc`
  (order matters: gsKit before dmAkit, audsrv after pad).
- Strip the release ELF (`mips64r5900el-ps2-elf-strip`) — debug symbols are
  several MB and every byte is read from USB on real hardware.

---

## 2. Mandatory Project Rules (fallout1-ce-ps2)

### Prohibitions
- **Never guess ps2sdk/gsKit signatures**. If you need a symbol, `grep -R`
  in `$PS2SDK/ee/include` and `$PS2DEV/gsKit/include` first. If it's not
  there, it doesn't exist — don't invent a prototype.
- **No emulator workarounds in production builds**. The smoke harness
  (`PS2_DEV_SMOKE_TEST=ON`) may keep `stdout` on the console and skip USB
  polling; the release build (`PS2_DEV_SMOKE_TEST=OFF`) must run the real
  storage bring-up and debug-log redirect. Fake `mass:` entries, synthetic
  VFS roots, or `host:` fallbacks in the probe chain are forbidden in
  production.
- **No `opendir()` on device roots**. Use `stat()` + `open()`/`close()` (or
  `dopen()`/`dclose()` when the newlib port exposes them).
- **No `double` in hot paths**. All gameplay math is single-precision
  (`float`) or integer.

### I/O Requirements
- **Storage bring-up order**: `host:` first (dev/emulator), then active
  sequential polling of `mass:` → `mass0:` → `mass1:` (stop at first ready),
  then `mc0:`. The old early-return when `mass:` merely existed is removed.
- **Debug log durability**: `ps2_debug_sync()` reopens the log with `O_APPEND`,
  `dup2()`s it onto `STDOUT_FILENO`/`STDERR_FILENO`, closes the temporary
  descriptor, and checks the 4 MB cap. `O_TRUNC` is **never** used during a
  sync (it would erase the boot log every 5 s).
- **Fatal path**: before `ps2_fatal_show()` and before `ps2_exit_to_loader()`,
  call `ps2_debug_sync(); fflush(stdout); fflush(stderr);`.
- **SIF RPC**: `_fio_intr` and `_fio_read_intr` must both be present in the
  linked `libkernel.a`.

### Build & Release Gate
- The only way to ship a release is `tools/make_dist.sh` (it runs inside the
  `fo1-build` Docker container):
  1. CMake with `-DCMAKE_BUILD_TYPE=Release -DPS2_DEV_SMOKE_TEST=OFF -DPS2_STRIP_ELF=ON`
  2. Copy the stripped build artifact to `dist/FALLOUT/FALLOUT.ELF`
  3. `cmp -s` verifies byte-for-byte identity
  4. `grep -aq SUITE_FINISHED` must **fail** (no smoke strings in release)
  5. ZIP `dist/fallout1-ce-ps2-vX.Y.Z.zip` and compute SHA-256
- The release is published with `gh release create <tag> <zip> --latest` and
  the tag is pushed to `origin`.

### Smoke Validation (CI Gate)
- Run `PS2_SMOKE_FASTMEM=0 tools/run_smoke.sh 900` (fastmem OFF = strict
  MMU/TLB path). Acceptance requires:
  - `SUITE_FINISHED_SUCCESS` + `ALL_SUBSYSTEMS_OK`
  - 0 `TLB Miss`
  - 0 `ReportErrorAsync`
  - 0 unaligned-access faults
  - 0 `Bus Error`
- If any of these appear, the build is **not releasable**. Fix, rebuild,
  re-run smoke, then re-run `make_dist.sh`.

---

## 3. Codebase Layout (quick reference)

```
src/
  game/          # engine core (cache.cc, art.cc, scripts, etc.)
  platform/ps2/  # PS2 backend
    boot.c           # entry point, init order
    ps2_storage.c    # USB/MX4SIO/BDM bring-up + polling
    ps2_debug.c      # crash-resistant logger + stdout redirect
    ps2_fs.c         # case-insensitive VFS roots
    video_gs.c       # gsKit framebuffer + palette
    sdl/sdl_shim.c   # SDL_RenderPresent → ps2_video_present + ps2_debug_tick
    ps2_exit.c       # fatal screen + loader handoff
    ps2_iop.c        # IOP reset + module loading
tools/
  make_dist.sh    # release bundler (runs in container)
  run_smoke.sh    # hyper-strict PCSX2 smoke test
dist/FALLOUT/     # checked-in SYSTEM.CNF + README_PS2.txt (ELF generated)
```

---

## 4. Quick Commands

```bash
# Build smoke ELF (inside container)
docker exec fo1-build cmake -S /src -B /src/build_ps2_smoke \
  -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-ps2.cmake \
  -DCMAKE_BUILD_TYPE=Release -DPS2_DEV_SMOKE_TEST=ON -DPS2_STRIP_ELF=OFF
docker exec fo1-build cmake --build /src/build_ps2_smoke -j4

# Run hyper-strict smoke (host)
PS2_SMOKE_FASTMEM=0 tools/run_smoke.sh 900

# Build & package release (inside container, then host)
tools/make_dist.sh

# Verify release zip
sha256sum dist/fallout1-ce-ps2-v1.0.2.zip
7z x -o/tmp/check dist/fallout1-ce-ps2-v1.0.2.zip
cmp -s build_ps2_release/fallout1-ce.elf /tmp/check/FALLOUT/FALLOUT.ELF
```

---

## 5. Versioning

- Tags: `vX.Y.Z-ps2` (e.g. `v1.0.2-ps2`).
- Branch: `ps2` tracks the PS2 port; `main` is the upstream engine.
- The `README.md` and `dist/FALLOUT/README_PS2.txt` are updated as part of
  the release commit.

---

## 6. Gotchas Learned the Hard Way

| Symptom | Root Cause | Fix |
|---------|------------|-----|
| `fioRead` hangs on strict PCSX2 | `_fio_read_intr` missing from IRX set | Apply `tools/patches/ps2sdk-sifrpc-deferred.patch` |
| Boot log vanishes on freeze | `fflush()` doesn't commit FAT32 dir entry | `ps2_debug_sync()` with `dup2()` close/reopen |
| `opendir("mass:")` wedges hostfs | PCSX2 HLE bug on missing device | Probe with `stat()` + `open()`/`close()` |
| ELF > 4 MB on USB boot | Debug symbols not stripped | `PS2_STRIP_ELF=ON` in Release |
| Stack overflow in deep VFS | Default 64 KB stack too small | `-Wl,--defsym,_stack_size=0x100000` |
| Float division traps | GCC inserts `__divdi3` check | `-mno-check-zero-division` |

---

## 7. Agent Workflow (per task)

1. **Read** this `AGENTS.md` and the relevant source files.
2. **Search** (`grep`) in `$PS2SDK` / `$PS2DEV/gsKit/include` for any symbol
   you need — never guess.
3. **Edit** the minimal set of files. Keep changes consistent with surrounding
   code style.
4. **Build** the smoke ELF and run the strict smoke suite (`fastmem=0`).
5. If green, run `tools/make_dist.sh`, verify the ZIP, push tag + release.
6. If red, fix, repeat from step 4.

---

*Generated from ps2-forge technical rules + fallout1-ce-ps2 project rules.*