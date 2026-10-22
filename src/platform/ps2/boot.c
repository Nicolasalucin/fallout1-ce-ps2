/*
 * Platform bootstrap for the Fallout PS2 port.
 *
 * Runs before the engine starts. F1 version: timing + banner. Later phases
 * plug in here, in this order:
 *
 *   F3: input   -> libpad init
 *   F4: I/O     -> device probing + path resolution
 *   F5: audio   -> audsrv init (lazily, when the engine opens the mixer)
 *   F9: storage -> USB/BDM IOP drivers for raw-ELF boots + ELF-relative
 *                  asset discovery seeded from argv[0]
 *
 * v1.0.1: ps2_debug_init() runs first so a hardware freeze still leaves a
 * debug.log with the boot device, VFS roots and IRX alignment audit.
 */

#include "ps2_time.h"
#include "ps2_input.h"
#include "ps2_fs.h"
#include "ps2_save.h"
#include "ps2_storage.h"

#include "ps2_debug.h"
#include "ps2_mem.h"

#include <stdio.h>

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
    ps2_debug_init();      /* 1. Inicializa buffer early + sonda candidatas (solo TTY) */
    ps2_mem_init();

    ps2_input_init();
    ps2_storage_init(argv0);   /* 2. USB up + active polling mass: (6.0 s) */

    /* ============================================================
     * AQUI: mass: YA EXISTE. Sondeo de assets y saves.
     * ============================================================ */
    ps2_fs_init();
    ps2_save_init();

    /* ============================================================
     * AHORA SI: redireccion segura. mass: esta montado -> no ENODEV.
     * ============================================================ */
    ps2_init_debug_log();

    ps2_debug_reprobe();
    PS2_LOG("[ps2] boot: debug log active='%s'", ps2_debug_path());

    boot_log_fs_roots();

    ps2_irx_alignment_report();

    PS2_LOG("[ps2] boot: heap free=%d KB used=%d KB", ps2_mem_free_kb(), ps2_mem_used_kb());
    PS2_LOG("[ps2] boot: boot ok (F1), timebase %u ms", (unsigned)ps2_time_ms());

    printf("[ps2] boot ok (F1), timebase %u ms\n", (unsigned)ps2_time_ms());

    return 0;
}
