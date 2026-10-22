# Local ps2sdk patches

These patches are applied to the **installed ps2sdk**, not to the game
sources. The CMake build consumes the patched `libkernel.a` from `$PS2SDK`.

## `ps2sdk-sifrpc-deferred.patch`

Target: `ee/lib/libcdvd/sifrpc.c` in ps2sdk (function `sceSifCallRpc`).

The stock SIF RPC completion callback executes `cd->end_function` for any
RPC. When PCSX2's host filesystem HLE emits a malformed completion (corrupt
queue), `end_function` points to garbage -> unaligned jump / TLB miss -> crash.

The patch replaces the callback with a strict guard that only executes the
known internal fileio handler `_fio_intr()`. Any other pointer is ignored.

```c
extern void _fio_intr(sceSifRpcData*);   /* declared in sifrpc.h */
if (cd->end_function == _fio_intr) cd->end_function(cd);
```

- **Normal path**: fileio registers `_fio_intr`; pointer matches; handler runs.
- **Corrupt path**: `end_function` is garbage; guard fails; nothing called.
  The RPC caller eventually times out or retries, but the EE stays alive.

### Requirements

Add to `sifrpc.h` (or equivalent public header):

```c
void _fio_intr(sceSifRpcData* qd);
```

### Apply

```sh
cd "$PS2SDKSRC"
patch -p1 < /path/to/fallout1-ce-ps2/tools/patches/ps2sdk-sifrpc-deferred.patch
make -C ee/lib/libcdvd
make -C ee/lib/libcdvd release
```

### Verify

```sh
mips64r5900el-ps2-elf-nm "$PS2SDK/ee/lib/libkernel.a" | grep _fio_intr
```

### Validation

1. Build Fallout 1 with the patched SDK.
2. Run the PCSX2 hyper-strict smoke suite (`PS2_SMOKE_FASTMEM=0 tools/run_smoke.sh`).
3. Verify: `SUITE_FINISHED_SUCCESS`, 0 TLB misses, 0 `ReportErrorAsync`, 0 unaligned jumps.
4. On real PS2 Slim: insert USB stick, boot ELF, confirm `mass:/FALLOUT/debug.log`
   appears and contains the full boot trace without a freeze.