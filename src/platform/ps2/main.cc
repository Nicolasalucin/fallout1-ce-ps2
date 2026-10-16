/*
 * PlayStation 2 entry point (replaces src/plib/gnw/winmain.cc on this port).
 *
 * winmain.cc can't be used here because the platform must be brought up
 * (timing, input, I/O modules, audio) before the engine starts, and because
 * it defines the SDL entry point. This file re-declares the two globals that
 * winmain.cc owns (GNW95_isActive, GNW95_title) and forwards to gnw_main.
 *
 * The engine treats itself as inactive (and thus stops processing input)
 * until it receives a window focus event; on PS2 the window is always
 * focused, so we set the flag at boot (the SDL shim also posts a
 * FOCUS_GAINED event for consistency with the desktop flow).
 */

#include "plib/gnw/winmain.h"

#include <stdio.h>

#include "game/main.h"

extern "C" int ps2_boot(void);

namespace fallout {

bool GNW95_isActive = true;
char GNW95_title[256] = "Fallout";

} // namespace fallout

int main(int argc, char* argv[])
{
    if (ps2_boot() != 0) {
        printf("[ps2] boot failed\n");
        return 1;
    }

    int rc = fallout::gnw_main(argc, argv);

    printf("[ps2] engine exited with code %d\n", rc);
    return rc;
}
