/*
 * Platform bootstrap for the Fallout PS2 port.
 *
 * Runs before the engine starts. F1 version: timing + banner. Later phases
 * plug in here, in this order:
 *
 *   F3: input   -> libpad init
 *   F4: I/O     -> IOP reset (iomanX/fileXio/ps2fs), module loading from the
 *                  boot device, path probing (host: -> mass: -> hdd0:)
 *   F5: audio   -> audsrv init
 */

#include "ps2_time.h"
#include "ps2_input.h"

#include <stdio.h>

int ps2_boot(void)
{
    ps2_time_init();
    ps2_input_init();

    printf("[ps2] boot ok (F1), timebase %u ms\n", (unsigned)ps2_time_ms());

    return 0;
}
