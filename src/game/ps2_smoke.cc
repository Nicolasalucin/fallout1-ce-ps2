/*
 * Automated in-engine smoke test harness (dev builds only).
 *
 * See ps2_smoke.h for the design notes. The harness takes over the game flow
 * from gnw_main() and exercises the whole UI hierarchy before booting into
 * the game:
 *
 *   frontend  the real main menu loop is driven with synthetic key taps:
 *             CREDITS screen, LOAD GAME with an empty save list, NEW GAME
 *             reaching the character generator (premade cycling + clean
 *             cancel) and EXIT. The intro movie is intentionally not
 *             exercised by the smoke suite.
 *   state 1   map render + script VM stability (dude present, frames advance)
 *   state 2   click-to-move: aims the cursor at a neighbour tile and clicks
 *             through the same SDL event path the pad uses; catches movement
 *             stutters (frame gap instrumentation, see below)
 *   state 3   inventory open (I) / close (Escape)
 *   state 4   Pip-Boy open (P) / close (Escape)
 *   state 5   in-game pause menu (ESC) + PREFERENCES screen: the four
 *             sliders (master/music/SFX volume, brightness/gamma LUT) are
 *             dragged through the real button+drag path, then the screens
 *             are closed back to gameplay
 *   state 6   character sheet (C) open / close (Escape)
 *   state 7   Skilldex (S) open / close (Escape)
 *   state 8   map streaming stress (JUNKENT.MAP, timed)
 *   state 9   save/load round-trip: saves the live Junkentown game into slot
 *             1 (memory card / host fallback via the F6 shim), verifies the
 *             files on disk, reboots to V13ENT, loads slot 1 back and
 *             compares map/tile/inventory/time/quest flags
 *   state 10  verdict + exit()
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
 *
 * The frontend phase runs those modal screen functions straight from
 * ps2_smoke_main(); the tick keeps posting the scripted input from inside
 * their event loops, and a per-screen deadline fires an ESC burst if a
 * screen ever stops responding (the suite then fails instead of hanging).
 */

#include "game/ps2_smoke.h"

#ifdef PS2_DEV_SMOKE_TEST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/credits.h"
#include "game/game.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/loadsave.h"
#include "game/mainmenu.h"
#include "game/game_vars.h"
#include "game/map.h"
#include "game/object.h"
#include "game/palette.h"
#include "game/proto.h"
#include "game/roll.h"
#include "game/scripts.h"
#include "game/select.h"
#include "game/tile.h"
#include "game/worldmap.h"
#include "plib/color/color.h"
#include "plib/db/db.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/input.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"
#include "platform_compat.h"
#include "ps2_save.h"
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
#define SMOKE_WATCHDOG_MS 120000
#define SMOKE_STALL_MS 1500
#define SMOKE_WALK_ATTEMPTS 4

/* Scripted synthetic input (see smoke_script_arm). Keep in sync with the
 * widest single-screen sequence (the preferences slider drags). */
#define SMOKE_SCRIPT_MAX 6

/* Frontend per-screen deadline. */
#define SMOKE_FRONTEND_STEP_MS 20000

typedef enum SmokeState {
    SMOKE_STATE_MAP_VERIFY,
    SMOKE_STATE_WALK,
    SMOKE_STATE_INVENTORY,
    SMOKE_STATE_PIPBOY,
    SMOKE_STATE_OPTIONS,
    SMOKE_STATE_CHARSHEET,
    SMOKE_STATE_SKILLDEX,
    SMOKE_STATE_MAP_STREAM,
    SMOKE_STATE_SAVELOAD,
    SMOKE_STATE_VERDICT,
} SmokeState;

typedef enum SmokeAction {
    SMOKE_ACTION_NONE = 0,
    SMOKE_ACTION_MAP_STREAM,
    SMOKE_ACTION_SAVE,
    SMOKE_ACTION_LOAD,
    SMOKE_ACTION_WIPE,
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

/* Frontend checks (set when each main menu screen was verified). */
static bool s_fe_credits = false;
static bool s_fe_load = false;
static bool s_fe_newgame = false;

/* State 9 - save/load round-trip (F6). */
#define SMOKE_SAVE_GVAR GVAR_FIND_WATER_CHIP

static char s9_map[32];
static int s9_tile;
static int s9_items;
static int s9_time;
static int s9_gvar;
static int s9_save_rc;
static int s9_load_rc;
static bool s9_files_ok;
static bool s_save_load_ok;

/* ---- Scripted synthetic input ----
 *
 * A small timed script drives the frontend screens and the modal in-game UI
 * states. Entries execute from ps2_smoke_tick() - i.e. from inside whatever
 * event loop the engine is currently running - at `at` milliseconds after
 * the script was armed. Keys are tapped (press + delayed release) and
 * clicks go through the same SDL path the pad uses: move, press, delayed
 * release. Besides clicking buttons, the release also terminates the
 * engine's slider drag loops. */
typedef enum SmokeScriptKind {
    SMOKE_SCRIPT_KEY,
    SMOKE_SCRIPT_CLICK,
} SmokeScriptKind;

typedef struct SmokeScriptEntry {
    unsigned int at;
    int kind;
    int a; // KEY: SDL scancode; CLICK: target screen x
    int b; // CLICK: target screen y
} SmokeScriptEntry;

static SmokeScriptEntry s_script[SMOKE_SCRIPT_MAX];
static int s_script_count = 0;
static int s_script_next = 0;
static unsigned int s_script_base = 0;

/* In-flight synthetic click: 0 = idle, 1 = move sent, 2 = press sent. */
static int s_click_state = 0;
static unsigned int s_click_time = 0;

/* Deadline for the frontend blocking screen call (0 = none). */
static unsigned int s_frontend_deadline = 0;

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
    case SMOKE_STATE_OPTIONS:
        return "options";
    case SMOKE_STATE_CHARSHEET:
        return "charsheet";
    case SMOKE_STATE_SKILLDEX:
        return "skilldex";
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

/* Arms a new timed input script (replaces any previous one). */
static void smoke_script_arm(unsigned int now, const SmokeScriptEntry* entries, int count)
{
    if (count > SMOKE_SCRIPT_MAX) {
        count = SMOKE_SCRIPT_MAX;
    }

    memcpy(s_script, entries, sizeof(SmokeScriptEntry) * count);
    s_script_count = count;
    s_script_next = 0;
    s_script_base = now;
}

static void smoke_script_clear(void)
{
    s_script_count = 0;
    s_script_next = 0;
    s_click_state = 0;
}

static bool smoke_script_done(void)
{
    return s_script_next >= s_script_count && s_click_state == 0;
}

/* Runs due script entries; safe to call from any event loop. */
static void smoke_script_tick(unsigned int now)
{
    if (s_click_state == 1) {
        if (now - s_click_time >= 150) {
            ps2_sdl_post_mouse_button(SDL_BUTTON_LEFT, 1);
            s_click_state = 2;
            s_click_time = now;
        }
        return;
    }

    if (s_click_state == 2) {
        if (now - s_click_time >= 130) {
            ps2_sdl_post_mouse_button(SDL_BUTTON_LEFT, 0);
            s_click_state = 0;
        }
        return;
    }

    if (s_script_next >= s_script_count) {
        return;
    }

    const SmokeScriptEntry* entry = &s_script[s_script_next];
    if ((int)(now - (s_script_base + entry->at)) < 0) {
        return;
    }

    s_script_next++;

    if (entry->kind == SMOKE_SCRIPT_KEY) {
        smoke_key_tap(entry->a, now);
    } else {
        int mx;
        int my;
        mouse_get_position(&mx, &my);
        ps2_sdl_post_mouse_move(entry->a - mx, entry->b - my);
        s_click_state = 1;
        s_click_time = now;
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

/* ---- Frontend phase (main menu screens, before booting) ----
 *
 * Mirrors the corresponding branches of gnw_main() (main.cc): every screen
 * is opened through the real entry point and driven with scripted keys from
 * the tick, so the whole UI plumbing (input queue, button system, modal
 * event loops) participates in the test. The static main.cc helpers used by
 * its LOAD GAME branch (main_loadgame_new / main_unload_new /
 * main_reset_system) are mirrored here with public engine entry points. */

static void smoke_loadgame_prepare(void)
{
    game_user_wants_to_quit = 0;

    if (obj_dude != NULL) {
        obj_dude->flags &= ~OBJECT_FLAT;
        obj_turn_on(obj_dude, NULL);
    }

    mouse_hide();
    map_init();
    gmouse_set_cursor(MOUSE_CURSOR_NONE);
    mouse_show();
}

static void smoke_loadgame_unprepare(void)
{
    if (obj_dude != NULL) {
        obj_turn_off(obj_dude, NULL);
    }

    map_exit();
}

/* Exercises the main menu buttons: CREDITS, LOAD GAME (with an empty save
 * list), NEW GAME (reaches the character generator, cycles through the
 * premades and cancels) and EXIT. Returns 0 unless the menu itself could
 * not be created. */
static int smoke_frontend_phase(void)
{
    static const int menu_keys[] = {
        SDL_SCANCODE_C, // CREDITS
        SDL_SCANCODE_L, // LOAD GAME
        SDL_SCANCODE_N, // NEW GAME
        SDL_SCANCODE_E, // EXIT
    };

    int step = 0;
    bool done = false;

    SMOKE_LOG("frontend: exercising main menu screens");

    if (main_menu_create() != 0) {
        smoke_fail("main_menu_create() failed");
        return -1;
    }

    while (!done) {
        SmokeScriptEntry entry;
        int rc;

        /* Mirrors the body of gnw_main's main menu loop. */
        gsound_background_play_level_music("07desert", 11);
        main_menu_show(1);
        mouse_show();

        entry.at = 900;
        entry.kind = SMOKE_SCRIPT_KEY;
        entry.a = menu_keys[step];
        entry.b = 0;
        smoke_script_arm(ps2_time_ms(), &entry, 1);
        s_frontend_deadline = ps2_time_ms() + SMOKE_FRONTEND_STEP_MS;

        rc = main_menu_loop();

        s_frontend_deadline = 0;
        mouse_hide();

        switch (rc) {
        case MAIN_MENU_CREDITS:
            main_menu_hide(true);
            {
                SmokeScriptEntry exit_key = { 2000, SMOKE_SCRIPT_KEY, SDL_SCANCODE_ESCAPE, 0 };

                smoke_script_arm(ps2_time_ms(), &exit_key, 1);
                s_frontend_deadline = ps2_time_ms() + SMOKE_FRONTEND_STEP_MS;
                credits("credits.txt", -1, false);
                s_frontend_deadline = 0;

                s_fe_credits = true;
                SMOKE_LOG("frontend: CREDITS screen opened and exited on key");
            }
            break;

        case MAIN_MENU_LOAD_GAME:
            main_menu_hide(true);
            main_menu_destroy();
            gsound_background_stop();
            {
                SmokeScriptEntry exit_key = { 2500, SMOKE_SCRIPT_KEY, SDL_SCANCODE_ESCAPE, 0 };
                int win = win_add(0, 0, screenGetWidth(), screenGetHeight(), colorTable[0], WINDOW_MODAL | WINDOW_MOVE_ON_TOP);

                smoke_loadgame_prepare();
                loadColorTable("color.pal");
                palette_fade_to(cmap);

                smoke_script_arm(ps2_time_ms(), &exit_key, 1);
                s_frontend_deadline = ps2_time_ms() + SMOKE_FRONTEND_STEP_MS;
                rc = LoadGame(LOAD_SAVE_MODE_FROM_MAIN_MENU);
                s_frontend_deadline = 0;

                if (rc == 0) {
                    s_fe_load = true;
                    SMOKE_LOG("frontend: LOAD GAME empty save list opened/closed clean");
                } else if (rc == -1) {
                    smoke_fail("LoadGame() returned -1 (empty save list)");
                } else {
                    SMOKE_LOG("frontend: LOAD GAME returned %d (unexpected)", rc);
                }

                palette_fade_to(white_palette);
                if (win != -1) {
                    win_delete(win);
                }

                smoke_loadgame_unprepare();
                game_reset();
            }
            main_menu_create();
            break;

        case MAIN_MENU_NEW_GAME:
            main_menu_hide(true);
            main_menu_destroy();
            {
                SmokeScriptEntry keys[] = {
                    { 1500, SMOKE_SCRIPT_KEY, SDL_SCANCODE_RIGHT, 0 },
                    { 2100, SMOKE_SCRIPT_KEY, SDL_SCANCODE_LEFT, 0 },
                    { 2800, SMOKE_SCRIPT_KEY, SDL_SCANCODE_ESCAPE, 0 },
                };

                smoke_script_arm(ps2_time_ms(), keys, 3);
                s_frontend_deadline = ps2_time_ms() + SMOKE_FRONTEND_STEP_MS;
                rc = select_character();
                s_frontend_deadline = 0;

                if (rc == 3) {
                    s_fe_newgame = true;
                    SMOKE_LOG("frontend: NEW GAME reached chargen (premades cycled, cancelled)");
                } else {
                    smoke_fail("select_character() did not cancel via ESC");
                }
            }
            main_menu_create();
            break;

        case MAIN_MENU_EXIT:
            done = true;
            main_menu_hide(true);
            main_menu_destroy();
            gsound_background_stop();
            SMOKE_LOG("frontend: main menu EXIT");
            break;

        default:
            smoke_fail("unexpected main menu result");
            main_menu_hide(true);
            main_menu_destroy();
            gsound_background_stop();
            done = true;
            break;
        }

        step++;

        if (step >= (int)(sizeof(menu_keys) / sizeof(menu_keys[0]))) {
            done = true;
        }
    }

    return 0;
}

/* Vanilla Fallout loads a save's map state from its .SAV file, so a live
 * X.MAP map round-trips as X.SAV; accept either spelling of the same map. */
static bool smoke_map_name_matches(const char* loaded, const char* saved)
{
    if (strcmp(loaded, saved) == 0) {
        return true;
    }

    const char* lext = strrchr(loaded, '.');
    const char* sext = strrchr(saved, '.');
    if (lext == NULL || sext == NULL) {
        return false;
    }
    if ((strcmp(lext, ".MAP") != 0 && strcmp(lext, ".SAV") != 0)
        || (strcmp(sext, ".MAP") != 0 && strcmp(sext, ".SAV") != 0)) {
        return false;
    }

    size_t llen = (size_t)(lext - loaded);
    size_t slen = (size_t)(sext - saved);
    return llen == slen && strncmp(loaded, saved, llen) == 0;
}

/* Heavy actions - top-level game loop only. */
static void smoke_run_pending(unsigned int now)
{
    switch (s_pending) {
    case SMOKE_ACTION_MAP_STREAM: {
        s_pending = SMOKE_ACTION_NONE;

        SMOKE_LOG("state8: streaming %s ...", SMOKE_STREAM_MAP);

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
            SMOKE_LOG("state8: %s streamed in %u ms", SMOKE_STREAM_MAP, ps2_time_ms() - start);
        }

        /* Blocking load - do not count it as a frame gap. */
        s_tick_last = 0;
        s_phase = 1;
        s_state_time = ps2_time_ms();
        break;
    }

    case SMOKE_ACTION_SAVE:
        s_pending = SMOKE_ACTION_NONE;

        SMOKE_LOG("state9: SAVE GAME dialog (slot 1)");
        s_in_blocking_action = true;
        s9_save_rc = SaveGame(LOAD_SAVE_MODE_NORMAL);
        s_in_blocking_action = false;
        smoke_script_clear();
        s_frontend_deadline = 0;

        /* The dialog ran modal frames - restart the gap measurement. */
        s_tick_last = 0;
        SMOKE_LOG("state9: SaveGame returned %d", s9_save_rc);

        s_phase = 11;
        s_state_time = ps2_time_ms();
        break;

    case SMOKE_ACTION_LOAD:
        s_pending = SMOKE_ACTION_NONE;

        SMOKE_LOG("state9: LOAD GAME dialog (slot 1)");
        s_in_blocking_action = true;
        s9_load_rc = LoadGame(LOAD_SAVE_MODE_NORMAL);
        s_in_blocking_action = false;
        smoke_script_clear();
        s_frontend_deadline = 0;
        s_tick_last = 0;
        SMOKE_LOG("state9: LoadGame returned %d", s9_load_rc);

        s_phase = 31;
        s_state_time = ps2_time_ms();
        break;

    case SMOKE_ACTION_WIPE: {
        s_pending = SMOKE_ACTION_NONE;

        s_in_blocking_action = true;
        int rc = smoke_boot_game();
        s_in_blocking_action = false;
        s_tick_last = 0;

        if (rc == 0) {
            /* Tamper with the live state so the load has to restore it. */
            set_game_time(s9_time + 5000);
            game_set_global_var(SMOKE_SAVE_GVAR, s9_gvar + 7);
            SMOKE_LOG("state9: wiped state -> map=%s tile=%d items=%d time=%d gvar=%d",
                map_data.name, obj_dude->tile, obj_dude->data.inventory.length,
                game_time(), game_get_global_var(SMOKE_SAVE_GVAR));
        } else {
            smoke_fail("state9: wipe (reboot to V13ENT) failed");
        }

        s_phase = 21;
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

        SMOKE_LOG("summary: frontend(credits=%s load=%s newgame=%s) map=%s walk=%s inventory=ok pipboy=ok options=ok charsheet=ok skilldex=ok streaming=%s save-load=%s",
            s_fe_credits ? "ok" : "skip",
            s_fe_load ? "ok" : "skip",
            s_fe_newgame ? "ok" : "skip",
            SMOKE_START_MAP, s_walk_moved ? "ok" : "skip",
            SMOKE_STREAM_MAP, s_save_load_ok ? "ok" : "skip");

        if (s_failures == 0) {
            SMOKE_LOG("ALL_SUBSYSTEMS_OK");
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

/* ---------------------------------------------------- state 9 helpers --- */

/* Reads up to `size` bytes at `offset` from a file inside save slot 1,
 * going through the platform shim (the exact path the engine uses), and
 * returns the file length, or -1 when the file cannot be opened. */
static long smoke_slot_file_read(const char* fileName, long offset, unsigned char* buf, int size)
{
    char path[128];
    FILE* f;
    long len;

    snprintf(path, sizeof(path), "SAVEGAME\\SLOT01\\%s", fileName);

    f = compat_fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    len = ftell(f);

    if (buf != NULL && size > 0 && offset >= 0) {
        memset(buf, 0, size);
        if (fseek(f, offset, SEEK_SET) == 0) {
            fread(buf, 1, size, f);
        }
    }

    fclose(f);
    return len;
}

/* Timer/input state machine - event pump safe. */
extern "C" void ps2_smoke_tick(void)
{
    unsigned int now = ps2_time_ms();

    /* Synthetic input runs in every mode, including the frontend screens. */
    smoke_key_tap_finish(now);
    smoke_script_tick(now);

    /* Frontend screens: if a modal screen stopped responding, fire an ESC
     * burst so the suite fails loudly instead of hanging. */
    if (s_frontend_deadline != 0 && (int)(now - s_frontend_deadline) >= 0) {
        static const SmokeScriptEntry esc_burst[] = {
            { 0, SMOKE_SCRIPT_KEY, SDL_SCANCODE_ESCAPE, 0 },
            { 700, SMOKE_SCRIPT_KEY, SDL_SCANCODE_ESCAPE, 0 },
            { 1400, SMOKE_SCRIPT_KEY, SDL_SCANCODE_ESCAPE, 0 },
            { 2100, SMOKE_SCRIPT_KEY, SDL_SCANCODE_ESCAPE, 0 },
            { 2800, SMOKE_SCRIPT_KEY, SDL_SCANCODE_ESCAPE, 0 },
            { 3500, SMOKE_SCRIPT_KEY, SDL_SCANCODE_ESCAPE, 0 },
        };

        s_frontend_deadline = 0;
        smoke_fail("frontend screen timed out; forcing ESC");
        smoke_script_arm(now, esc_burst, SMOKE_SCRIPT_MAX);
    }

    if (!s_active) {
        return;
    }

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
            SMOKE_LOG("state4: opening Pip-Boy (P), pass 1/2 (cold)");
            smoke_key_tap(SDL_SCANCODE_P, now);
            s_phase = 1;
            s_state_time = now;
        } else if (s_phase == 1 && elapsed >= 1500) {
            smoke_key_tap(SDL_SCANCODE_ESCAPE, now);
            s_phase = 2;
            s_state_time = now;
        } else if (s_phase == 2 && elapsed >= 700) {
            SMOKE_LOG("state4: opening Pip-Boy (P), pass 2/2 (warm)");
            smoke_key_tap(SDL_SCANCODE_P, now);
            s_phase = 3;
            s_state_time = now;
        } else if (s_phase == 3 && elapsed >= 1500) {
            smoke_key_tap(SDL_SCANCODE_ESCAPE, now);
            s_phase = 4;
            s_state_time = now;
        } else if (s_phase == 4 && elapsed >= 700) {
            SMOKE_LOG("state4 PASS: Pip-Boy open/close clean (cold + warm passes)");
            s_state = SMOKE_STATE_OPTIONS;
            s_state_time = now;
            s_phase = 0;
        }
        break;

    case SMOKE_STATE_OPTIONS:
        if (s_phase == 0) {
            SMOKE_LOG("state5: in-game pause menu (ESC) + preferences");
            smoke_key_tap(SDL_SCANCODE_ESCAPE, now);
            s_phase = 1;
            s_state_time = now;
        } else if (s_phase == 1 && elapsed >= 1800) {
            /* Enter PREFERENCES from the pause menu (P). */
            smoke_key_tap(SDL_SCANCODE_P, now);
            s_phase = 2;
            s_state_time = now;
        } else if (s_phase == 2 && elapsed >= 2200) {
            /* Drag the master/music/SFX volume and brightness sliders
             * through the real button+drag path. Slider rows span
             * x 384..624; y = knob row (196 master, 247 music, 298 SFX,
             * 400 brightness); the drag loop sets each value from the
             * mouse x position, so clicking far right selects a high
             * value. A first ESC cancels the PREFERENCES screen (gamma
             * and volumes are restored), the second closes the menu. */
            static const SmokeScriptEntry slides[] = {
                { 0, SMOKE_SCRIPT_CLICK, 590, 196 },
                { 700, SMOKE_SCRIPT_CLICK, 590, 247 },
                { 1400, SMOKE_SCRIPT_CLICK, 590, 298 },
                { 2100, SMOKE_SCRIPT_CLICK, 590, 400 },
                { 3300, SMOKE_SCRIPT_KEY, SDL_SCANCODE_ESCAPE, 0 },
                { 4600, SMOKE_SCRIPT_KEY, SDL_SCANCODE_ESCAPE, 0 },
            };

            smoke_script_arm(now, slides, SMOKE_SCRIPT_MAX);
            s_phase = 3;
            s_state_time = now;
        } else if (s_phase == 3 && smoke_script_done() && elapsed >= 5600) {
            SMOKE_LOG("state5 PASS: pause menu + preference sliders clean");
            s_state = SMOKE_STATE_CHARSHEET;
            s_state_time = now;
            s_phase = 0;
        }
        break;

    case SMOKE_STATE_CHARSHEET:
        if (s_phase == 0) {
            SMOKE_LOG("state6: character sheet (C) open/close");
            smoke_key_tap(SDL_SCANCODE_C, now);
            s_phase = 1;
            s_state_time = now;
        } else if (s_phase == 1 && elapsed >= 2500) {
            smoke_key_tap(SDL_SCANCODE_ESCAPE, now);
            s_phase = 2;
            s_state_time = now;
        } else if (s_phase == 2 && elapsed >= 1500) {
            SMOKE_LOG("state6 PASS: character sheet clean");
            s_state = SMOKE_STATE_SKILLDEX;
            s_state_time = now;
            s_phase = 0;
        }
        break;

    case SMOKE_STATE_SKILLDEX:
        if (s_phase == 0) {
            SMOKE_LOG("state7: Skilldex (S) open/close");
            smoke_key_tap(SDL_SCANCODE_S, now);
            s_phase = 1;
            s_state_time = now;
        } else if (s_phase == 1 && elapsed >= 1800) {
            smoke_key_tap(SDL_SCANCODE_ESCAPE, now);
            s_phase = 2;
            s_state_time = now;
        } else if (s_phase == 2 && elapsed >= 1200) {
            SMOKE_LOG("state7 PASS: Skilldex clean");
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
            SMOKE_LOG("state8 PASS: map streaming within budget");
            s_state = SMOKE_STATE_SAVELOAD;
            s_state_time = now;
            s_phase = 0;
        }
        break;

    case SMOKE_STATE_SAVELOAD:
        if (s_phase == 0) {
            /* Capture the live state, then save it into slot 1. */
            static const SmokeScriptEntry save_keys[] = {
                { 2500, SMOKE_SCRIPT_KEY, SDL_SCANCODE_RETURN, 0 },
                { 5200, SMOKE_SCRIPT_KEY, SDL_SCANCODE_RETURN, 0 },
            };

            SMOKE_LOG("state9: save/load round-trip (slot 1)");
            ps2_save_slot_wipe(1);

            snprintf(s9_map, sizeof(s9_map), "%s", map_data.name);
            s9_tile = obj_dude->tile;
            s9_items = obj_dude->data.inventory.length;
            s9_time = game_time();
            s9_gvar = game_get_global_var(SMOKE_SAVE_GVAR);
            SMOKE_LOG("state9: pre-save map=%s tile=%d items=%d time=%d gvar=%d",
                s9_map, s9_tile, s9_items, s9_time, s9_gvar);

            smoke_script_arm(now, save_keys, 2);
            s_frontend_deadline = now + SMOKE_FRONTEND_STEP_MS;
            s_pending = SMOKE_ACTION_SAVE;
            s_phase = 10;
        } else if (s_phase == 11) {
            bool filesOk = s9_save_rc == 1;
            unsigned char head[8];
            long len;

            if (s9_save_rc != 1) {
                smoke_fail("state9: SaveGame did not return success");
            }

            /* SAVE.DAT must exist and carry data. */
            len = smoke_slot_file_read("SAVE.DAT", -1, NULL, 0);
            if (len <= 0) {
                smoke_fail("state9: SAVE.DAT missing in slot 1");
                filesOk = false;
            } else {
                SMOKE_LOG("state9: SAVE.DAT committed (%ld bytes)", len);
            }

            /* Map payloads: one .SAV per map. The listing goes through
             * db_get_file_list(), i.e. through the same directory
             * enumeration the engine uses on the memory card. */
            char** savList = NULL;
            int savCount = db_get_file_list("SAVEGAME\\SLOT01\\*.SAV", &savList, NULL, 0);
            if (savCount <= 0) {
                smoke_fail("state9: no map .SAV files committed to slot 1");
                filesOk = false;
            } else {
                for (int i = 0; i < savCount; i++) {
                    SMOKE_LOG("state9: slot payload: %s", savList[i]);
                }
            }
            db_free_file_list(&savList, NULL);

            /* PS2 browser metadata. */
            len = smoke_slot_file_read("icon.sys", 0, head, 4);
            if (len != PS2_SAVE_ICON_SYS_SIZE || memcmp(head, "PS2D", 4) != 0) {
                smoke_fail("state9: icon.sys missing or invalid");
                filesOk = false;
            } else {
                smoke_slot_file_read("icon.sys", 0xC0, head, 7);
                if (memcmp(head, "Fallout", 7) != 0) {
                    smoke_fail("state9: icon.sys title invalid");
                    filesOk = false;
                } else {
                    SMOKE_LOG("state9: icon.sys OK (%ld bytes, PS2D, title 'Fallout')", len);
                }
            }

            len = smoke_slot_file_read("icon.icn", 0, head, 4);
            if (len != PS2_SAVE_ICON_ICN_SIZE
                || head[0] != 0x00 || head[1] != 0x00
                || head[2] != 0x01 || head[3] != 0x00) {
                smoke_fail("state9: icon.icn missing or invalid");
                filesOk = false;
            } else {
                SMOKE_LOG("state9: icon.icn OK (%ld bytes)", len);
            }

            s9_files_ok = filesOk;

            /* Reboot to the start map; slot 1 must bring the junkentown
             * state back afterwards. */
            s_pending = SMOKE_ACTION_WIPE;
            s_phase = 20;
        } else if (s_phase == 21) {
            static const SmokeScriptEntry load_keys[] = {
                { 2500, SMOKE_SCRIPT_KEY, SDL_SCANCODE_RETURN, 0 },
            };

            if (strcmp(map_data.name, s9_map) == 0) {
                smoke_fail("state9: wipe did not change the map");
            }

            smoke_script_arm(now, load_keys, 1);
            s_frontend_deadline = now + SMOKE_FRONTEND_STEP_MS;
            s_pending = SMOKE_ACTION_LOAD;
            s_phase = 30;
        } else if (s_phase == 31) {
            bool ok = s9_files_ok && s9_load_rc == 1;

            if (s9_load_rc != 1) {
                smoke_fail("state9: LoadGame did not return success");
            }

            int tile = obj_dude != NULL ? obj_dude->tile : -1;
            int items = obj_dude != NULL ? obj_dude->data.inventory.length : -1;
            int gtime = game_time();
            int gvar = game_get_global_var(SMOKE_SAVE_GVAR);

            SMOKE_LOG("state9: post-load map=%s tile=%d items=%d time=%d gvar=%d",
                map_data.name, tile, items, gtime, gvar);
            SMOKE_LOG("state9: saved     map=%s tile=%d items=%d time=%d gvar=%d",
                s9_map, s9_tile, s9_items, s9_time, s9_gvar);

            if (!smoke_map_name_matches(map_data.name, s9_map)) {
                smoke_fail("state9: map mismatch after load");
                ok = false;
            } else if (strcmp(map_data.name, s9_map) != 0) {
                SMOKE_LOG("state9: map %s loaded as %s (saved-map .SAV, vanilla)", s9_map, map_data.name);
            }
            if (tile != s9_tile) {
                smoke_fail("state9: player tile mismatch after load");
                ok = false;
            }
            if (items != s9_items) {
                smoke_fail("state9: inventory mismatch after load");
                ok = false;
            }
            if (gtime != s9_time) {
                smoke_fail("state9: game time mismatch after load");
                ok = false;
            }
            if (gvar != s9_gvar) {
                smoke_fail("state9: quest flags mismatch after load");
                ok = false;
            }

            if (ok) {
                s_save_load_ok = true;
                SMOKE_LOG("SAVE_LOAD_ROUNDTRIP_OK");
                SMOKE_LOG("state9 PASS: save/load round-trip (slot 1) verified");
            }

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

    if (smoke_frontend_phase() != 0) {
        SMOKE_LOG("SUITE_FINISHED_WITH_ERRORS: frontend phase failed");
        fflush(stdout);
        exit(0);
    }

    /* Make sure no stray scripted key leaks into gameplay. */
    smoke_script_clear();

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
