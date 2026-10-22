# Local ps2sdk patches

These patches are applied to the **installed ps2sdk**, not to the game
sources. The CMake build consumes the patched `libkernel.a` from `$PS2SDK`.

## `ps2sdk-sifrpc-deferred.patch`

Target: `ee/kernel/src/sifrpc.c` in ps2sdk.

The stock SIF RPC completion handler dereferences `request->cd` and then
`cd->hdr.pkt_addr` from interrupt context. A malformed completion (observed
with PCSX2 as `cid=8000000a`, `_fio_cd`, `pkt_addr=0x00000000`) therefore
turns into an EE TLB miss at `pc=0x80003800`. Printing the packet directly
from the handler is not safe either: `printf` takes the stdio lock and can
deadlock if the interrupted thread owns it.

The patch adds:

- a pointer-range/alignment guard for `cd`;
- a guard that verifies `pkt_addr` belongs to the SIF RPC packet table and is
  packet-aligned before `rpc_packet_free()` is called;
- a fixed 16-word snapshot of the invalid completion, recorded in the
  handler without calling libc;
- `sceSifRpcDumpInvalidCompletion()`, which prints the snapshot later from
  thread context. The smoke harness calls it from `ps2_smoke_tick()` through
  a weak symbol, so an unpatched ps2sdk still links and runs;
- a strict completion-callback whitelist for the two known fileio handlers:
  `_fio_intr()` (open/close/write/stat/etc.) and `_fio_read_intr()` (read).
  Any other `end_function` value is treated as a corrupted queue pointer and
  is not called.

The read handler is essential: `fioOpen()` uses `_fio_intr()`, but
`fioRead()` passes `_fio_read_intr()`. A whitelist that only allows
`_fio_intr()` lets `fioOpen()` succeed and then leaves the first read blocked
forever on `_fio_completion_sema` (observed as a hang inside the first
`fgets()` of `fallout.cfg`).

A malformed completion cannot simply be dropped: the caller is blocked in
`WaitSema()` on the RPC client semaphore and would sleep forever (observed as
a deterministic hang in `dopen()` during `LoadGame`). When the client pointer
itself is valid but `pkt_addr` is not, the patch now:

- recovers the allocated packet from `cd->hdr.rpc_id` in the packet table;
- treats the packet as a lost **fileio** completion: calls the known
  `_fio_intr()` kernel symbol and signals the waiting semaphore. It never
  calls `cd->end_function` through a potentially corrupt client pointer
  (that path produced `PC=0xffffffff` on PCSX2);
- if the completion no longer carries the real client pointer but `_fio_cd`
  still has an in-flight call without a packet, falls back to `_fio_cd`;
- publishes bind server fields only for the known fileio client, frees the
  recovered packet when found, and clears the client packet pointer;
- records the invalid completion for the deferred dump.

This keeps the TLB guard while ensuring a bad PCSX2 completion cannot leave
fileio blocked. Fileio is the blocking SIF client in this port, so its two
handlers are the only completion callbacks on the whitelist; any other
`end_function` value is treated as corrupt and is not called. The dump is
still printed from thread context on the next smoke tick.

### Apply

From the root of a ps2sdk checkout (or the installed SDK source tree):

```sh
cd "$PS2SDKSRC"
patch -p1 < /path/to/fallout1-ce-ps2/tools/patches/ps2sdk-sifrpc-deferred.patch
```

Rebuild and install the kernel library. The exact install target depends on
how ps2sdk was built; the important step is replacing `libkernel.a` and
`libkernel.erl` in `$PS2SDK/ee/lib`:

```sh
export PS2SDKSRC="$PWD"
make -C ee/kernel -j"$(nproc)"
make -C ee/kernel release
```

Verify that both the diagnostic symbol and both fileio handlers are present:

```sh
mips64r5900el-ps2-elf-nm "$PS2SDK/ee/lib/libkernel.a" \
    | grep -E 'sceSifRpcDumpInvalidCompletion|_fio_(read_)?intr'
mips64r5900el-ps2-elf-nm "$PS2SDK/ee/lib/libkernel.erl" \
    | grep -E 'sceSifRpcDumpInvalidCompletion|_fio_(read_)?intr'
```

### Validation

1. Build Fallout 1 with the patched SDK.
2. Run the PCSX2 hyper-strict smoke suite
   (`PS2_SMOKE_FASTMEM=0 tools/run_smoke.sh 900`).
3. Verify: `SUITE_FINISHED_SUCCESS`, 0 TLB misses, 0 `ReportErrorAsync`,
   0 unaligned jumps and no Bus Error.
4. On real PS2 Slim: insert the USB stick, boot the ELF and confirm
   `mass:/FALLOUT/debug.log` contains the full boot trace without a freeze.

The diagnostic and recovery reference libkernel's `_fio_cd` client and the
two fileio end functions. If a future ps2sdk does not export them, remove
the `extern SifRpcClientData_t _fio_cd;`, `_fio_intr()` and
`_fio_read_intr()` declarations, the fileio recovery branch and the
`_rpc_dump_client("_fio_cd", &_fio_cd);` call; the pointer guards, deferred
dump and generic packet validation remain functional.
