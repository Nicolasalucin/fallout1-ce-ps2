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
 */

#include "ps2_time.h"
#include "ps2_input.h"
#include "ps2_fs.h"
#include "ps2_save.h"
#include "ps2_storage.h"

#include <stdio.h>

int ps2_boot(const char* argv0)
{
    ps2_time_init();
    ps2_input_init();
    ps2_storage_init(argv0);
    ps2_fs_init();
    ps2_save_init();

    printf("[ps2] boot ok (F1), timebase %u ms\n", (unsigned)ps2_time_ms());

    return 0;
}
