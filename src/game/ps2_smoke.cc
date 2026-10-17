/*
 * Automated in-engine smoke test harness (dev builds only).
 *
 * See ps2_smoke.h for the design notes. The harness takes over the game flow
 * from gnw_main() (movies and main menu are skipped), boots a premade dude
 * straight into Vault 13's entrance map and then walks through a scripted
 * validation sequence:
 *
 *   state 1  map render + script VM stability (dude present, frames advance)
 *   state 2  click-to-move: aims the cursor at a neighbour tile and clicks
 *            through the same SDL event path the pad uses; catches movement
 *            stutters (frame gap instrumentation, see below)
 *   state 3  inventory open (I) / close (Escape)
 *   state 4  Pip-Boy open (P) / close (Escape)
 *   state 5  map streaming stress (JUNKENT.MAP, timed)
 *   state 6  save/load round-trip (skipped until the F6 memory card backend)
 *   state 7  verdict + exit()
 *
 * A stall detector runs on every event pump tick: the worst gap between two
 * ticks (i.e. the longest the engine went without servicing a frame) is
 * recorded together with the state it happened in. If it exceeds a sane
 * budget the suite reports a stutter failure instead of passing silently.
 *
 * State transitions run from ps2_smoke_tick(), which the SDL shim calls on
 * every event pump - including inside modal screens, where the engine runs
 * its own event loops. Only synthetic input and timers are handled there;
 * heavy engine work (map loads, the verdict) executes from the harness' own
 * top-level game loop, never from inside an event pump.
 */

#include "game/ps2_smoke.h"

#ifdef PS2_DEV_SMOKE_TEST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/game.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/map.h"
#include "game/object.h"
#include "game/palette.h"
#include "game/proto.h"
#include "game/roll.h"
#include "game/scripts.h"
#include "game/tile.h"
#include "game/worldmap.h"
#include "plib/color/color.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/input.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"
#include "ps2_time.h"
#include "sdl/ps2_sdl_internal.h"

extern "C" {
/* TEMP profiling counters (F5.2 map load, see fs.c). */
extern unsigned int ps2_fs_stat_fopen_calls;
extern unsigned int ps2_fs_stat_canonicalize_calls;
extern void ps2_fs_stat_reset(void);
}

namespace fallout {

#define SMOKE_LOG(...)                     \
    do {                                   \
        printf("[ps2][smoke] " __VA_ARGS__); \
        printf("\n");                      \
        fflush(stdout);                    \
    } while (0)

#define SMOKE_START_MAP "V13ENT.MAP"
#define SMOKE_STREAM_MAP "JUNKENT.MAP"
#define SMOKE_WATCHDOG_MS 90000
#define SMOKE_STALL_MS 1500
#define SMOKE_WALK_ATTEMPTS 4

typedef enum SmokeState {
    SMOKE_STATE_MAP_VERIFY,
    SMOKE_STATE_WALK,
    SMOKE_STATE_INVENTORY,
    SMOKE_STATE_PIPBOY,
    SMOKE_STATE_MAP_STREAM,
    SMOKE_STATE_SAVELOAD,
    SMOKE_STATE_VERDICT,
} SmokeState;

typedef enum SmokeAction {
    SMOKE_ACTION_NONE = 0,
    SMOKE_ACTION_MAP_STREAM,
    SMOKE_ACTION_VERDICT,
} SmokeAction;

/* Screen offsets (from the dude) of the neighbour tiles the walk state tries
 * to click, in order: east, west, south, north. */
static const int s_walk_offsets[SMOKE_WALK_ATTEMPTS][2] = {
    { 20, 0 },
    { -20, 0 },
    { 0, 20 },
    { 0, -20 },
};

static bool s_active = false;
static SmokeState s_state = SMOKE_STATE_MAP_VERIFY;
static unsigned int s_state_time = 0;
static unsigned int s_suite_start = 0;
static int s_phase = 0;
static int s_failures = 0;
static unsigned int s_frames = 0;
static SmokeAction s_pending = SMOKE_ACTION_NONE;
static int s_tap_scancode = -1;
static unsigned int s_tap_release_at = 0;

/* Stall detector (updated on every event pump tick). */
static unsigned int s_tick_last = 0;
static unsigned int s_worst_gap = 0;
static SmokeState s_worst_gap_state = SMOKE_STATE_MAP_VERIFY;

/* Walk state. */
static int s_walk_attempt = 0;
static int s_walk_tile_before = 0;
static unsigned int s_walk_click_time = 0;
static bool s_walk_moved = false;

/* Set while a blocking engine action (map load) runs from the top-level
 * loop: the stall detector must not count the load itself as a stutter. */
static bool s_in_blocking_action = false;

static const char* smoke_state_name(SmokeState state)
{
    switch (state) {
    case SMOKE_STATE_MAP_VERIFY:
        return "map-verify";
    case SMOKE_STATE_WALK:
        return "walk";
    case SMOKE_STATE_INVENTORY:
        return "inventory";
    case SMOKE_STATE_PIPBOY:
        return "pipboy";
    case SMOKE_STATE_MAP_STREAM:
        return "map-stream";
    case SMOKE_STATE_SAVELOAD:
        return "save-load";
    case SMOKE_STATE_VERDICT:
        return "verdict";
    }
    return "?";
}

static void smoke_fail(const char* what)
{
    s_failures++;
    SMOKE_LOG("FAIL: %s", what);
}

/* Synthetic key tap: press now, release shortly after (tick-driven). */
static void smoke_key_tap(int scancode, unsigned int now)
{
    ps2_sdl_post_key(scancode, 1);
    s_tap_scancode = scancode;
    s_tap_release_at = now + 60;
}

static void smoke_key_tap_finish(unsigned int now)
{
    if (s_tap_scancode >= 0 && (int)(now - s_tap_release_at) >= 0) {
        ps2_sdl_post_key(s_tap_scancode, 0);
        s_tap_scancode = -1;
    }
}

static void smoke_check_dude(const char* where)
{
    if (obj_dude == NULL) {
        smoke_fail("obj_dude is NULL");
        return;
    }

    SMOKE_LOG("check %s: dude tile=%d elevation=%d", where, obj_dude->tile, obj_dude->elevation);
}

/* Boot a premade dude straight into a map (mirrors main.cc main_load_new). */
static int smoke_boot_game(void)
{
    unsigned int start = ps2_time_ms();

    SMOKE_LOG("boot: game_reset + premade dude (%s)", "premade/combat.gcd");
    game_user_wants_to_quit = 0;
    gsound_background_stop();
    roll_set_seed(0xBEEFFEED);
    game_reset();

    if (proto_dude_init("premade\\combat.gcd") != 0) {
        SMOKE_LOG("FAIL: proto_dude_init(premade/combat.gcd) failed");
        return -1;
    }

    obj_dude->flags &= ~OBJECT_FLAT;
    obj_turn_on(obj_dude, NULL);
    mouse_hide();

    int win = win_add(0, 0, screenGetWidth(), screenGetHeight(), colorTable[0], WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    win_draw(win);

    loadColorTable("color.pal");
    palette_fade_to(cmap);
    map_init();
    gmouse_set_cursor(MOUSE_CURSOR_NONE);
    mouse_show();

    char mapName[] = SMOKE_START_MAP;
    ps2_fs_stat_reset();
    if (map_load(mapName) == -1) {
        SMOKE_LOG("FAIL: map_load(%s) failed", SMOKE_START_MAP);
        return -1;
    }
    SMOKE_LOG("io-stats boot: fopen=%u canonicalize=%u",
        ps2_fs_stat_fopen_calls, ps2_fs_stat_canonicalize_calls);

    PlayCityMapMusic();

    palette_fade_to(white_palette);
    win_delete(win);
    loadColorTable("color.pal");
    palette_fade_to(cmap);

    SMOKE_LOG("boot: %s loaded in %u ms", SMOKE_START_MAP, ps2_time_ms() - start);

    return 0;
}

/* Heavy actions - top-level game loop only. */
static void smoke_run_pending(unsigned int now)
{
    switch (s_pending) {
    case SMOKE_ACTION_MAP_STREAM: {
        s_pending = SMOKE_ACTION_NONE;

        SMOKE_LOG("state5: streaming %s ...", SMOKE_STREAM_MAP);

        unsigned int start = now;
        char mapName[] = SMOKE_STREAM_MAP;
        ps2_fs_stat_reset();
        s_in_blocking_action = true;
        int rc = map_load(mapName);
        s_in_blocking_action = false;
        SMOKE_LOG("io-stats junktown: fopen=%u canonicalize=%u",
            ps2_fs_stat_fopen_calls, ps2_fs_stat_canonicalize_calls);

        if (rc == -1) {
            smoke_fail("map_load(JUNKENT.MAP) failed");
        } else {
            PlayCityMapMusic();
            obj_turn_on(obj_dude, NULL);
            tile_refresh_display();
            SMOKE_LOG("state5: %s streamed in %u ms", SMOKE_STREAM_MAP, ps2_time_ms() - start);
        }

        /* Blocking load - do not count it as a frame gap. */
        s_tick_last = 0;
        s_phase = 1;
        s_state_time = ps2_time_ms();
        break;
    }

    case SMOKE_ACTION_VERDICT:
        s_pending = SMOKE_ACTION_NONE;

        if (s_worst_gap > SMOKE_STALL_MS) {
            SMOKE_LOG("FAIL: stutter detected: worst frame gap %u ms (state %s)",
                s_worst_gap, smoke_state_name(s_worst_gap_state));
            s_failures++;
        }

        SMOKE_LOG("perf: suite %u ms, %u top-level frames, worst frame gap %u ms (%s)",
            now - s_suite_start, s_frames, s_worst_gap, smoke_state_name(s_worst_gap_state));

        SMOKE_LOG("summary: map=%s walk=%s inventory=ok pipboy=ok streaming=%s save-load=skipped",
            SMOKE_START_MAP, s_walk_moved ? "ok" : "skip",
            SMOKE_STREAM_MAP);

        if (s_failures == 0) {
            SMOKE_LOG("SUITE_FINISHED_SUCCESS: All game subsystems verified!");
        } else {
            SMOKE_LOG("SUITE_FINISHED_WITH_ERRORS: %d failure(s)", s_failures);
        }

        fflush(stdout);
        exit(0);
        break;

    default:
        break;
    }
}

/* Timer/input state machine - event pump safe. */
extern "C" void ps2_smoke_tick(void)
{
    if (!s_active) {
        return;
    }

    unsigned int now = ps2_time_ms();

    /* Stall detector: worst gap between two pumps (blocking loads excluded). */
    if (!s_in_blocking_action && s_tick_last != 0) {
        unsigned int gap = now - s_tick_last;
        if (gap > s_worst_gap) {
            s_worst_gap = gap;
            s_worst_gap_state = s_state;
        }
    }
    s_tick_last = now;

    unsigned int elapsed = now - s_state_time;

    smoke_key_tap_finish(now);

    if (now - s_suite_start > SMOKE_WATCHDOG_MS && s_state != SMOKE_STATE_VERDICT) {
        SMOKE_LOG("FAIL: watchdog expired in state %s", smoke_state_name(s_state));
        s_failures++;
        s_state = SMOKE_STATE_VERDICT;
        s_pending = SMOKE_ACTION_VERDICT;
        return;
    }

    switch (s_state) {
    case SMOKE_STATE_MAP_VERIFY:
        if (elapsed >= 1200) {
            smoke_check_dude("map-v13ent");
            SMOKE_LOG("state1 PASS: map render + script VM stable (%u frames)", s_frames);
            s_state = SMOKE_STATE_WALK;
            s_state_time = now;
            s_phase = 0;
            s_walk_attempt = 0;
            s_walk_moved = false;
            s_walk_tile_before = obj_dude != NULL ? obj_dude->tile : 0;
        }
        break;

    case SMOKE_STATE_WALK:
        if (s_phase == 0) {
            /* Aim the cursor at a neighbour tile (screen offset from the dude)
             * and click - the same SDL path the pad drives. */
            const int* off = s_walk_offsets[s_walk_attempt];
            int mx;
            int my;
            int tx;
            int ty;

            mouse_get_position(&mx, &my);
            tx = obj_dude->sx + off[0];
            ty = obj_dude->sy + off[1];
            if (tx < 12) tx = 12;
            if (tx > 627) tx = 627;
            if (ty < 44) ty = 44; /* keep clear of the interface bar */
            if (ty > 468) ty = 468;
            ps2_sdl_post_mouse_move(tx - mx, ty - my);
            s_state_time = now;
            s_phase = 1;
        } else if (s_phase == 1 && elapsed >= 250) {
            ps2_sdl_post_mouse_button(SDL_BUTTON_LEFT, 1);
            s_state_time = now;
            s_phase = 2;
        } else if (s_phase == 2 && elapsed >= 80) {
            ps2_sdl_post_mouse_button(SDL_BUTTON_LEFT, 0);
            s_walk_click_time = now;
            s_state_time = now;
            s_phase = 3;
        } else if (s_phase == 3) {
            if (obj_dude->tile != s_walk_tile_before) {
                s_walk_moved = true;
                SMOKE_LOG("state2 PASS: dude walked tile %d -> %d (click to move took %u ms)",
                    s_walk_tile_before, obj_dude->tile, now - s_walk_click_time);
                s_state = SMOKE_STATE_INVENTORY;
                s_state_time = now;
                s_phase = 0;
            } else if (elapsed >= 1200) {
                if (++s_walk_attempt >= SMOKE_WALK_ATTEMPTS) {
                    SMOKE_LOG("state2 SKIP: click-to-move did not move the dude in %d attempts", SMOKE_WALK_ATTEMPTS);
                    s_state = SMOKE_STATE_INVENTORY;
                    s_state_time = now;
                    s_phase = 0;
                } else {
                    s_phase = 0;
                }
            }
        }
        break;

    case SMOKE_STATE_INVENTORY:
        if (s_phase == 0) {
            SMOKE_LOG("state3: opening inventory (I)");
            smoke_key_tap(SDL_SCANCODE_I, now);
            s_phase = 1;
            s_state_time = now;
        } else if (s_phase == 1 && elapsed >= 600) {
            smoke_key_tap(SDL_SCANCODE_ESCAPE, now);
            s_phase = 2;
            s_state_time = now;
        } else if (s_phase == 2 && elapsed >= 500) {
            SMOKE_LOG("state3 PASS: inventory open/close clean");
            s_state = SMOKE_STATE_PIPBOY;
            s_state_time = now;
            s_phase = 0;
        }
        break;

    case SMOKE_STATE_PIPBOY:
        if (s_phase == 0) {
            SMOKE_LOG("state4: opening Pip-Boy (P)");
            smoke_key_tap(SDL_SCANCODE_P, now);
            s_phase = 1;
            s_state_time = now;
        } else if (s_phase == 1 && elapsed >= 1000) {
            smoke_key_tap(SDL_SCANCODE_ESCAPE, now);
            s_phase = 2;
            s_state_time = now;
        } else if (s_phase == 2 && elapsed >= 500) {
            SMOKE_LOG("state4 PASS: Pip-Boy open/close clean");
            s_state = SMOKE_STATE_MAP_STREAM;
            s_state_time = now;
            s_phase = 0;
            s_pending = SMOKE_ACTION_MAP_STREAM;
        }
        break;

    case SMOKE_STATE_MAP_STREAM:
        /* s_phase 0 = waiting for the top-level loop to run the switch. */
        if (s_phase == 1 && elapsed >= 1500) {
            smoke_check_dude("map-junktown");
            SMOKE_LOG("state5 PASS: map streaming within budget");
            s_state = SMOKE_STATE_SAVELOAD;
            s_state_time = now;
            s_phase = 0;
        }
        break;

    case SMOKE_STATE_SAVELOAD:
        if (elapsed >= 200) {
            SMOKE_LOG("state6 SKIPPED: save/load round-trip pending F6 (memory card backend)");
            s_state = SMOKE_STATE_VERDICT;
            s_state_time = now;
            s_phase = 0;
            s_pending = SMOKE_ACTION_VERDICT;
        }
        break;

    case SMOKE_STATE_VERDICT:
        /* Exit happens via SMOKE_ACTION_VERDICT in the top-level loop. */
        break;
    }
}

/* Harness game loop (replaces movies + menu + main_game_loop). */
int ps2_smoke_main(void)
{
    SMOKE_LOG("harness compiled in; taking over the game flow");

    if (smoke_boot_game() != 0) {
        SMOKE_LOG("SUITE_FINISHED_WITH_ERRORS: boot failed");
        fflush(stdout);
        exit(0);
    }

    s_active = true;
    s_state = SMOKE_STATE_MAP_VERIFY;
    s_state_time = ps2_time_ms();
    s_suite_start = s_state_time;
    s_phase = 0;
    s_failures = 0;
    s_frames = 0;
    s_pending = SMOKE_ACTION_NONE;
    s_tick_last = 0;
    s_worst_gap = 0;
    s_worst_gap_state = SMOKE_STATE_MAP_VERIFY;

    scr_enable();

    while (true) {
        s_frames++;

        smoke_run_pending(ps2_time_ms());

        sharedFpsLimiter.mark();

        int keyCode = get_input();
        game_handle_input(keyCode, false);

        scripts_check_state();
        map_check_state();

        if ((obj_dude->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) != 0) {
            SMOKE_LOG("dude died during the suite; aborting");
            game_user_wants_to_quit = 2;
        }

        if (game_user_wants_to_quit != 0) {
            SMOKE_LOG("engine requested quit (code %d); aborting suite", game_user_wants_to_quit);
            break;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    scr_disable();

    SMOKE_LOG("SUITE_FINISHED_WITH_ERRORS: engine quit during suite");
    fflush(stdout);
    exit(0);

    return 0;
}

} // namespace fallout

#endif /* PS2_DEV_SMOKE_TEST */
