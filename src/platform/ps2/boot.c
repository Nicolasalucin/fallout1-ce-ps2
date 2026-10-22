/*
 * Platform bootstrap for the Fallout PS2 port.
 *
 * Order is deliberate:
 *
 *   1. time/memory/exception  -> diagnostics exist before any I/O
 *   2. IOP reset + base stack -> clean FILEIO/MCMAN/PADMAN on hardware
 *   3. USB storage            -> mass: / mass0: / mass1:
 *   4. pad                    -> after the reset, never on a dead driver
 *   5. debug log              -> no stale fd from the pre-reset IOP
 *   6. VFS + save probing     -> fatal screen if the game data is missing
 *   7. stdout redirection     -> all later printf reaches the stick/card
 *
 * The debug log is opened only after the IOP reset and the USB bring-up. Any
 * PS2_LOG() before that is buffered in RAM and replayed once a destination
 * exists; that is why an early failure still leaves a useful debug.log.
 */

#include "ps2_time.h"
#include "ps2_input.h"
#include "ps2_fs.h"
#include "ps2_save.h"
#include "ps2_storage.h"
#include "ps2_iop.h"
#include "ps2_exception.h"

#include "ps2_debug.h"
#include "ps2_mem.h"

#include <stdint.h>
#include <stdio.h>

/* Absolute linker symbols (see ps2sdk/ee/startup/linkfile and crt0.c).
 * _stack_size is not a C variable: take its address to read the value.
 * _stack is the linkfile's -1 sentinel; crt0 derives the live top from
 * _stack_size and the end of RAM (ps2_mem_stack_report() prints the bounds). */
extern char _end;
extern char _stack;
extern char _stack_size;

static void boot_log_link_layout(void)
{
    PS2_LOG("[ps2] boot: _end=0x%08x _stack=0x%08x _stack_size=0x%08x",
        (unsigned)(uintptr_t)&_end,
        (unsigned)(uintptr_t)&_stack,
        (unsigned)(uintptr_t)&_stack_size);
    printf("[ps2] boot: _end=0x%08x _stack=0x%08x _stack_size=0x%08x\n",
        (unsigned)(uintptr_t)&_end,
        (unsigned)(uintptr_t)&_stack,
        (unsigned)(uintptr_t)&_stack_size);
}

static void boot_log_fs_roots(void)
{
    int count = ps2_fs_root_count();
    int i;

    PS2_LOG("[ps2] boot: VFS roots = %d", count);
    for (i = 0; i < count; i++) {
        PS2_LOG("[ps2] boot: root[%d] = '%s'", i, ps2_fs_root_path(i));
    }

    /* The shipped media is uppercase, the engine asks for lowercase paths.
     * Log both spellings so a hardware report proves the FAT32 case-insensitive
     * resolver is doing its job. */
    {
        static const char* const probes[] = {
            "DATA/",
            "data/",
            "DATA/MAPS/",
            "data/maps/",
        };
        int p;

        for (p = 0; p < (int)(sizeof(probes) / sizeof(probes[0])); p++) {
            char resolved[256];

            resolved[0] = '\0';
            if (ps2_fs_resolve(probes[p], resolved, sizeof(resolved), 0) == 0) {
                PS2_LOG("[ps2] boot: resolve '%s' -> '%s'", probes[p], resolved);
            } else {
                PS2_LOG("[ps2] boot: resolve '%s' -> NOT FOUND", probes[p]);
            }
        }
    }
}

int ps2_boot(const char* argv0)
{
    ps2_time_init();
    ps2_exception_step("boot-time", (unsigned)ps2_time_ms());

    ps2_mem_init();                 /* heap baseline + stack canary/high-water */
    ps2_exception_init();           /* R5900 vectors before any I/O */
    ps2_exception_step("boot-mem", 0);

    ps2_iop_bringup();              /* clean IOP reset + ioman/fileio/mc/pad */
    ps2_exception_step("boot-iop", (unsigned)ps2_iop_did_reset());

    ps2_storage_init(argv0);        /* USB/BDM/MX4SIO + active mass: polling */
    ps2_exception_step("boot-storage", 0);

    ps2_input_init();               /* pad drivers are reloaded after reset */
    ps2_exception_step("boot-input", 0);

    ps2_debug_init();               /* safe now: the new IOP owns the medium */
    ps2_exception_step("boot-debug", (unsigned)(ps2_debug_path()[0] != '\0'));

    ps2_fs_init();                  /* fatal screen + clean exit if no data */
    ps2_save_init();
    ps2_exception_step("boot-fs", (unsigned)ps2_fs_root_count());

    /* Choose the final log path first, then redirect stdout/stderr to it. */
    ps2_debug_reprobe();
    ps2_init_debug_log();
    PS2_LOG("[ps2] boot: debug log active='%s'", ps2_debug_path());

    boot_log_link_layout();
    boot_log_fs_roots();
    ps2_irx_alignment_report();
    ps2_mem_stack_report();

    PS2_LOG("[ps2] boot: heap free=%d KB used=%d KB sbrk=+%d KB",
        ps2_mem_free_kb(), ps2_mem_used_kb(), ps2_mem_sbrk_kb());
    PS2_LOG("[ps2] boot: boot ok, timebase %u ms", (unsigned)ps2_time_ms());

    printf("[ps2] boot ok, timebase %u ms\n", (unsigned)ps2_time_ms());
    ps2_exception_step("boot-done", 0);

    return 0;
}
