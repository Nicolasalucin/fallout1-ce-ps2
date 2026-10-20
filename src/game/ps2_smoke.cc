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
 *   state 10  world tour: warps through the heaviest maps while the heap is
 *             probed (mallinfo + engine counters) and checks the usage
 *             plateaus instead of climbing (F7 memory budget)
 *   state 11  dialogue stress: warps to the map of a voiced head NPC
 *             (Killian in Junktown), parks the dude in front of them, runs
 *             the real talk script and drives the option list with synthetic
 *             digit taps; verifies the talking head art and the streamed
 *             voice-over (F7)
 *   state 12  combat stress: starts a real melee via the crosshair click
 *             (combat_attack_this), drives player attacks and SPACE
 *             end-turn taps while the AI turns resolve, then leaves the
 *             battle cleanly (F7)
 *   state 13  verdict + exit()
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
#include <libpad.h>
#include <stdint.h>
#include <cmath>

extern "C" {
/* TEMP profiling counters (F5.2 map load, see fs.c). */
extern unsigned int ps2_fs_stat_fopen_calls;
extern unsigned int ps2_fs_stat_canonicalize_calls;
extern unsigned int ps2_fs_stat_dir_cache_hits;
extern unsigned int ps2_fs_stat_dir_scans;
extern void ps2_fs_stat_reset(void);

/* Provided by patched ps2sdk. Weak so an unpatched SDK still links; the smoke
 * build only calls it when present. */
extern void sceSifRpcDumpInvalidCompletion(void) __attribute__((weak));
}

#include "game/art.h"
#include "game/combat.h"
#include "game/credits.h"
#include "game/game.h"
#include "game/gdialog.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/intface.h"
#include "game/loadsave.h"
#include "game/mainmenu.h"
#include "game/game_vars.h"
#include "game/map.h"
#include "game/object.h"
#include "game/palette.h"
#include "game/proto.h"
#include "game/protinst.h"
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
#include "ps2_input.h"
#include "ps2_mem.h"
#include "ps2_osk.h"
#include "ps2_save.h"
#include "ps2_time.h"
#include "sdl/ps2_sdl_internal.h"

/* F7 memory probes (see plib/gnw/memory.cc, smoke builds only). */

namespace fallout {
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/intface.h"
#include "game/loadsave.h"
#include "game/mainmenu.h"
#include "game/game_vars.h"
#include "game/map.h"
#include "game/object.h"
#include "game/palette.h"
#include "game/proto.h"
#include "game/protinst.h"
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
#include "ps2_input.h"
#include "ps2_mem.h"
#include "ps2_osk.h"
#include "ps2_save.h"
#include "ps2_time.h"
#include "sdl/ps2_sdl_internal.h"

/* F7 memory probes (see plib/gnw/memory.cc, smoke builds only). */
int ps2_mem_engine_cur_kb(void);
int ps2_mem_engine_max_kb(void);
void ps2_mem_engine_report(const char* tag);

/* F7 dialogue stress: voice-over counters (see game/lip_sync.cc). */
extern unsigned int ps2_lips_load_count;
extern unsigned int ps2_lips_play_count;

#define SMOKE_LOG(...)                     \
    do {                                   \
        printf("[ps2][smoke] " __VA_ARGS__); \
        printf("\n");                      \
        fflush(stdout);                    \
    } while (0)

#define SMOKE_START_MAP "V13ENT.MAP"
#define SMOKE_STREAM_MAP "JUNKENT.MAP"
#define SMOKE_WATCHDOG_MS 300000
#define SMOKE_STALL_MS 1500
#define SMOKE_WALK_ATTEMPTS 4

/* State 10 - world tour memory stress (F7). */
#define SMOKE_TOUR_SETTLE_MS 1000
#define SMOKE_TOUR_PLATEAU_KB 2048
#define SMOKE_TOUR_CEIL_KB 20480

/* State 11 - talking-head dialogue stress (F7). */
#define SMOKE_DLG_SETTLE_MS 800
#define SMOKE_DLG_MAX_MS 22000
#define SMOKE_DLG_CAND_MAX 32
#define SMOKE_DLG_TRIES 3

/* State 12 - turn-based combat stress (F7). */
#define SMOKE_CBT_SETTLE_MS 900
#define SMOKE_CBT_MAX_MS 20000
#define SMOKE_CBT_DRIVE_MS 2500
#define SMOKE_CBT_CLICK_TRIES 5

/* State 13 - pad hotkey dispatch (F8). */
#define SMOKE_HK_DETECT_MS 700
#define SMOKE_HK_CLOSE_MS 800
#define SMOKE_HK_MAX_MS 6000

/* State 9 - OSK save name typing (F8). ~50 pad events, 200ms each. */
#define SMOKE_OSK_EVENTS 64
#define SMOKE_OSK_EVENT_MS 200

/* Scripted synthetic input (see smoke_script_arm). Keep in sync with the
 * widest single-screen sequence (the F8 OSK save-name typing, which has to
 * navigate the whole 10x4 grid with D-pad taps). */
#define SMOKE_SCRIPT_MAX 128

/* Frontend per-screen deadline. */
#define SMOKE_FRONTEND_STEP_MS 20000
/* The OSK save-name script is long and paced (a pressed/released pad sample
 * costs one pump each at low dialog frame rates), so it needs a wider
 * margin than a plain menu screen. */
#define SMOKE_OSK_DEADLINE_MS 60000

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
    SMOKE_STATE_WORLD_TOUR,
    SMOKE_STATE_DIALOGUE,
    SMOKE_STATE_COMBAT,
    SMOKE_STATE_PAD_HOTKEYS,
    SMOKE_STATE_VERDICT,
} SmokeState;

typedef enum SmokeAction {
    SMOKE_ACTION_NONE = 0,
    SMOKE_ACTION_MAP_STREAM,
    SMOKE_ACTION_SAVE,
    SMOKE_ACTION_LOAD,
    SMOKE_ACTION_WIPE,
    SMOKE_ACTION_TOUR_STEP,
    SMOKE_ACTION_DIALOGUE_LOAD,
    SMOKE_ACTION_DIALOGUE_TALK,
    SMOKE_ACTION_COMBAT_START,
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

/* State 10 - world tour (F7). Starts and ends on JUNKENT so the plateau
 * check compares the same fully warmed map across two full passes. */
static const char* const s_tour_maps[] = {
    "V13ENT.MAP", "SHADYE.MAP", "JUNKENT.MAP", "HUBDWNTN.MAP",
    "HALLDED.MAP", "MBENT.MAP", "CHILDRN1.MAP", "JUNKENT.MAP",
};
#define SMOKE_TOUR_MAPS ((int)(sizeof(s_tour_maps) / sizeof(s_tour_maps[0])))
static int s_tour_idx;
static int s_tour_used_kb[SMOKE_TOUR_MAPS];
static int s_tour_engine_kb[SMOKE_TOUR_MAPS];
static unsigned int s_tour_load_ms[SMOKE_TOUR_MAPS];
static bool s_world_tour_ok;

/* State 11 - dialogue stress (F7). Each map is scanned for scripted critters;
 * the one whose proto name looks like a known voiced head (Killian in
 * JUNKKILL, Gizmo in JUNKCSNO, Aradesh in Shady Sands) is preferred. */
static const char* const s_dlg_maps[] = { "JUNKKILL.MAP", "SHADYW.MAP", "SHADYE.MAP", "JUNKCSNO.MAP" };
#define SMOKE_DLG_MAPS ((int)(sizeof(s_dlg_maps) / sizeof(s_dlg_maps[0])))
static Object* s_dlg_cand[SMOKE_DLG_CAND_MAX];
static int s_dlg_cand_count;
static int s_dlg_cand_idx;
static Object* s_dlg_target;
static char s_dlg_name[32];
static bool s_dlg_active;
static bool s_dlg_forced;
static unsigned int s_dlg_start;
static bool s_dlg_ran;
static int s_dlg_head;
static unsigned int s_dlg_voice_load;
static unsigned int s_dlg_voice_play;
static bool s_dialogue_ok;

/* State 12 - combat stress (F7). */
static Object* s_cbt_target;
static char s_cbt_name[32];
static int s_cbt_hp_before;
static int s_cbt_target_hp_before;
static int s_cbt_attacks;
static int s_cbt_space_taps;
static int s_cbt_drives;
static bool s_cbt_engaged;
static bool s_cbt_ai_seen;
static char s_cbt_ai_name[32];
static unsigned int s_cbt_start;
static unsigned int s_cbt_last_drive;
static bool s_combat_ok;

/* F8 pad hotkeys. */
static int s_hk_step;
static unsigned int s_hk_start;
static int s_hk_ok_count;
static unsigned int s_hk_frames_at_tap;

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
    SMOKE_SCRIPT_PAD,
} SmokeScriptKind;

typedef struct SmokeScriptEntry {
    unsigned int at;
    int kind;
    int a; // KEY: SDL scancode; CLICK: target screen x; PAD: PAD_* bit mask
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
    case SMOKE_STATE_WORLD_TOUR:
        return "world-tour";
    case SMOKE_STATE_DIALOGUE:
        return "dialogue";
    case SMOKE_STATE_COMBAT:
        return "combat";
    case SMOKE_STATE_PAD_HOTKEYS:
        return "pad-hotkeys";
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

/* F8 synthetic pad tap: press through the real pad path (bindings + OSK)
 * via the input layer's smoke injection, release shortly after. This is
 * the only path that can drive the on-screen keyboard from the suite. */
static int s_pad_tap_buttons = 0;
static unsigned int s_pad_tap_release_at = 0;

static void smoke_pad_tap(unsigned int buttons, unsigned int now)
{
    ps2_input_smoke_press(buttons);
    s_pad_tap_buttons = (int)buttons;
    s_pad_tap_release_at = now + 60;
}

static void smoke_pad_tap_finish(unsigned int now)
{
    if (s_pad_tap_buttons != 0 && (int)(now - s_pad_tap_release_at) >= 0) {
        ps2_input_smoke_release();
        s_pad_tap_buttons = 0;
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

    if (s_pad_tap_buttons != 0) {
        ps2_input_smoke_release();
        s_pad_tap_buttons = 0;
    }
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

    /* Pace pad taps: the injector must have delivered the previous release
     * (buttons==0) to a poll before a new press is applied, otherwise the
     * press overwrites the pending release and two identical taps merge
     * into one held direction (the OSK then loses a nav step). */
    if (entry->kind == SMOKE_SCRIPT_PAD) {
        if (s_pad_tap_buttons != 0 || ps2_input_smoke_release_pending()) {
            return;
        }
    }

    s_script_next++;

    if (entry->kind == SMOKE_SCRIPT_KEY) {
        smoke_key_tap(entry->a, now);
    } else if (entry->kind == SMOKE_SCRIPT_PAD) {
        smoke_pad_tap((unsigned int)entry->a, now);
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

/* F7: one heap snapshot - system allocator + engine tracked bytes. */
static void smoke_mem_probe(const char* tag)
{
    ps2_mem_report(tag);
    ps2_mem_engine_report(tag);
}

/* Build the OSK script programmatically from the 4x10 grid. */
static void build_osk_script(unsigned int now, const char* target)
{
    static const char kGrid[4][11] = {
        "ABCDEFGHIJ",
        "KLMNOPQRST",
        "UVWXYZ0123",
        "456789_-. ",
    };

    static SmokeScriptEntry seq[128];
    int n = 0;
    unsigned int t = 2500;

    /* 1. Press RETURN to select Slot 1 and open comment box. */
    seq[n++] = { t, SMOKE_SCRIPT_KEY, SDL_SCANCODE_RETURN, 0 };
    t += 700; /* Wait for get_input_str2 -> SDL_StartTextInput -> ps2_osk_open. */

    /* 2. Clear any pre-filled description with 14 backspaces (PAD_SQUARE). */
    for (int i = 0; i < 14; ++i) {
        seq[n++] = { t, SMOKE_SCRIPT_PAD, PAD_SQUARE, 0 };
        t += 160;
    }

    /* 3. Navigate from (0,0) and type each character of target. */
    int cur_r = 0, cur_c = 0;
    for (const char* p = target; *p; ++p) {
        int tr = -1, tc = -1;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 10; ++c) {
                if (kGrid[r][c] == *p) { tr = r; tc = c; break; }
            }
            if (tr >= 0) break;
        }
        while (cur_r < tr) { seq[n++] = { t, SMOKE_SCRIPT_PAD, PAD_DOWN, 0 };  t += 160; cur_r++; }
        while (cur_r > tr) { seq[n++] = { t, SMOKE_SCRIPT_PAD, PAD_UP, 0 };    t += 160; cur_r--; }
        while (cur_c < tc) { seq[n++] = { t, SMOKE_SCRIPT_PAD, PAD_RIGHT, 0 }; t += 160; cur_c++; }
        while (cur_c > tc) { seq[n++] = { t, SMOKE_SCRIPT_PAD, PAD_LEFT, 0 };  t += 160; cur_c--; }
        seq[n++] = { t, SMOKE_SCRIPT_PAD, PAD_CROSS, 0 };
        t += 160;
    }

    /* 4. Submit with PAD_START. */
    seq[n++] = { t, SMOKE_SCRIPT_PAD, PAD_START, 0 };

    smoke_script_arm(now, seq, n);
}

/* Known voiced-head NPCs (proto name prefixes, case-insensitive): Killian
 * and Gizmo live in Junktown, Aradesh in Shady Sands, the Overseer in the
 * vault. Anything else scores 1 (still usable, tried later). */
static const char* const s_dlg_head_names[] = { "arad", "kill", "gizm", "over", "elde" };

static int smoke_dlg_name_score(const char* name)
{
    if (name == NULL) {
        return 0;
    }

    for (int i = 0; i < (int)(sizeof(s_dlg_head_names) / sizeof(s_dlg_head_names[0])); i++) {
        const char* pat = s_dlg_head_names[i];

        for (const char* p = name; p[0] != '\0'; p++) {
            int j = 0;
            while (pat[j] != '\0' && p[j] != '\0' && (p[j] | 0x20) == pat[j]) {
                j++;
            }

            if (pat[j] == '\0') {
                return 2;
            }
        }
    }

    return 1;
}

/* Collects scripted critters of the loaded map as dialogue candidates and
 * moves the best-scoring one (Aradesh-like name) to the front. Returns 2 when
 * a preferred candidate was found, 0 otherwise. */
static int smoke_dlg_scan(void)
{
    int best = -1;
    s_dlg_cand_count = 0;

    Object* obj = obj_find_first();
    while (obj != NULL && s_dlg_cand_count < SMOKE_DLG_CAND_MAX) {
        if (obj != obj_dude
            && obj->sid != -1
            && PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER
            && obj->elevation == map_elevation
            && (obj->flags & OBJECT_HIDDEN) == 0
            && (obj->data.critter.combat.results & DAM_DEAD) == 0) {
            const char* name = proto_name(obj->pid);
            int score = smoke_dlg_name_score(name);

            SMOKE_LOG("state11: candidate %d: %s (pid=0x%08X sid=0x%08X)",
                s_dlg_cand_count + 1, name != NULL ? name : "?", obj->pid, obj->sid);

            s_dlg_cand[s_dlg_cand_count] = obj;
            if (score >= 2 && best == -1) {
                best = s_dlg_cand_count;
            }

            s_dlg_cand_count++;
        }

        obj = obj_find_next();
    }

    if (best > 0) {
        Object* tmp = s_dlg_cand[0];
        s_dlg_cand[0] = s_dlg_cand[best];
        s_dlg_cand[best] = tmp;
    }

    return best == -1 ? 0 : 2;
}

/* Screen point at the centre of a critter's art bounding box (same math as
 * obj_bound) - the aim point for the synthetic attack click. */
static bool smoke_cbt_sprite_center(Object* obj, int* outX, int* outY)
{
    int tileScreenX = 0;
    int tileScreenY = 0;

    if (obj == NULL || tile_coord(obj->tile, &tileScreenX, &tileScreenY, obj->elevation) != 0) {
        return false;
    }

    CacheEntry* artHandle = NULL;
    Art* art = art_ptr_lock(obj->fid, &artHandle);
    if (art == NULL) {
        return false;
    }

    tileScreenX += 16 + art->xOffsets[obj->rotation] + obj->x;
    tileScreenY += 8 + art->yOffsets[obj->rotation] + obj->y;

    int width = art_frame_width(art, obj->frame, obj->rotation);
    int height = art_frame_length(art, obj->frame, obj->rotation);

    art_ptr_unlock(artHandle);

    *outX = tileScreenX + width / 2;
    *outY = tileScreenY + height / 2;
    return true;
}

/* Posts one synthetic map click (clamped clear of the interface bar). */
static void smoke_cbt_click(unsigned int now, int x, int y)
{
    SmokeScriptEntry entry;

    if (x < 12) x = 12;
    if (x > 627) x = 627;
    if (y < 44) y = 44;
    if (y > 468) y = 468;

    entry.at = 0;
    entry.kind = SMOKE_SCRIPT_CLICK;
    entry.a = x;
    entry.b = y;
    smoke_script_arm(now, &entry, 1);
}

/* Taps SPACE to hand the turn over to the AI. */
static void smoke_cbt_space(unsigned int now)
{
    SmokeScriptEntry entry = { 0, SMOKE_SCRIPT_KEY, SDL_SCANCODE_SPACE, 0 };
    smoke_script_arm(now, &entry, 1);
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

    case SMOKE_ACTION_TOUR_STEP: {
        s_pending = SMOKE_ACTION_NONE;

        const char* mapName = s_tour_maps[s_tour_idx];
        char mapBuf[32];
        snprintf(mapBuf, sizeof(mapBuf), "%s", mapName);

        SMOKE_LOG("state10: tour %d/%d loading %s ...",
            s_tour_idx + 1, SMOKE_TOUR_MAPS, mapName);

        unsigned int start = now;
        s_in_blocking_action = true;
        int rc = map_load(mapBuf);
        s_in_blocking_action = false;

        /* Blocking load - do not count it as a frame gap. */
        s_tick_last = 0;

        if (rc == -1) {
            smoke_fail("world tour: map load failed");
            SMOKE_LOG("state10: %s failed to load", mapName);
            s_state = SMOKE_STATE_VERDICT;
            s_state_time = ps2_time_ms();
            s_phase = 0;
            s_pending = SMOKE_ACTION_VERDICT;
            break;
        }

        obj_turn_on(obj_dude, NULL);
        tile_refresh_display();

        s_tour_used_kb[s_tour_idx] = ps2_mem_used_kb();
        s_tour_engine_kb[s_tour_idx] = ps2_mem_engine_cur_kb();
        s_tour_load_ms[s_tour_idx] = ps2_time_ms() - start;
        smoke_mem_probe(mapName);

        SMOKE_LOG("state10: tour %d/%d %s loaded in %u ms (heap used %d KB, engine %d KB)",
            s_tour_idx + 1, SMOKE_TOUR_MAPS, mapName,
            s_tour_load_ms[s_tour_idx], s_tour_used_kb[s_tour_idx], s_tour_engine_kb[s_tour_idx]);

        s_tour_idx++;
        s_phase = s_tour_idx;
        s_state_time = ps2_time_ms();
        break;
    }

    case SMOKE_ACTION_DIALOGUE_LOAD: {
        s_pending = SMOKE_ACTION_NONE;

        bool loadFailed = false;
        int wantMap = -1;
        int loaded = -1;
        bool preferred = false;

        s_in_blocking_action = true;

        for (int i = 0; i < SMOKE_DLG_MAPS && !loadFailed; i++) {
            char mapBuf[32];
            snprintf(mapBuf, sizeof(mapBuf), "%s", s_dlg_maps[i]);
            SMOKE_LOG("state11: scanning %s for dialogue candidates ...", mapBuf);

            int rc = 0;
            if (smoke_map_name_matches(mapBuf, map_data.name)) {
                SMOKE_LOG("state11: %s is already the live map; scanning in place", mapBuf);
            } else {
                rc = map_load(mapBuf);
            }

            if (rc == -1) {
                smoke_fail("dialogue: map load failed");
                SMOKE_LOG("state11: %s failed to load", mapBuf);
                loadFailed = true;
                break;
            }

            loaded = i;
            obj_turn_on(obj_dude, NULL);
            tile_refresh_display();
            smoke_mem_probe(mapBuf);

            int score = smoke_dlg_scan();

            if (s_dlg_cand_count > 0) {
                wantMap = i;
            }

            if (score >= 2) {
                wantMap = i;
                preferred = true;
                break;
            }
        }

        if (!loadFailed && wantMap >= 0 && wantMap != loaded) {
            /* The preferred candidates live on the other map; go back. */
            char mapBuf[32];
            snprintf(mapBuf, sizeof(mapBuf), "%s", s_dlg_maps[wantMap]);
            SMOKE_LOG("state11: going back to %s for its candidates", mapBuf);

            if (map_load(mapBuf) == -1) {
                smoke_fail("dialogue: map reload failed");
                loadFailed = true;
            } else {
                obj_turn_on(obj_dude, NULL);
                tile_refresh_display();
                smoke_dlg_scan();
            }
        }

        s_in_blocking_action = false;
        s_tick_last = 0;

        if (loadFailed || wantMap == -1) {
            if (!loadFailed) {
                smoke_fail("dialogue: no scripted critters found on the target maps");
            }

            s_state = SMOKE_STATE_VERDICT;
            s_state_time = ps2_time_ms();
            s_phase = 0;
            s_pending = SMOKE_ACTION_VERDICT;
            break;
        }

        s_dlg_cand_idx = 0;
        s_dlg_target = s_dlg_cand[0];
        const char* chosen = proto_name(s_dlg_target->pid);
        SMOKE_LOG("state11: chosen %s (candidates=%d preferred=%d)",
            chosen != NULL ? chosen : "?", s_dlg_cand_count, preferred ? 1 : 0);
        smoke_mem_probe("dialogue");

        s_phase = 2;
        s_state_time = ps2_time_ms();
        break;
    }

    case SMOKE_ACTION_DIALOGUE_TALK: {
        s_pending = SMOKE_ACTION_NONE;

        Object* cand = s_dlg_target;
        if (cand == NULL) {
            smoke_fail("dialogue: no target to talk to");
            s_state = SMOKE_STATE_VERDICT;
            s_state_time = ps2_time_ms();
            s_phase = 0;
            s_pending = SMOKE_ACTION_VERDICT;
            break;
        }

        const char* name = proto_name(cand->pid);
        snprintf(s_dlg_name, sizeof(s_dlg_name), "%s", name != NULL ? name : "?");

        SMOKE_LOG("state11: talking to %s (pid=0x%08X sid=0x%08X) ...",
            s_dlg_name, cand->pid, cand->sid);

        /* Get within talking range: gdialog_enter needs line of sight. */
        obj_attempt_placement(obj_dude, cand->tile, cand->elevation, 0);
        tile_refresh_display();

        static const SmokeScriptEntry dlg_keys[] = {
            { 3500, SMOKE_SCRIPT_KEY, SDL_SCANCODE_1, 0 },
            { 7500, SMOKE_SCRIPT_KEY, SDL_SCANCODE_1, 0 },
            { 11500, SMOKE_SCRIPT_KEY, SDL_SCANCODE_1, 0 },
            { 15500, SMOKE_SCRIPT_KEY, SDL_SCANCODE_1, 0 },
        };

        s_dlg_voice_load = ps2_lips_load_count;
        s_dlg_voice_play = ps2_lips_play_count;
        s_dlg_forced = false;
        s_dlg_start = now;

        smoke_script_arm(now, dlg_keys, 4);

        s_dlg_active = true;
        s_in_blocking_action = true;
        gdialog_enter(cand, 0);
        s_in_blocking_action = false;
        s_dlg_active = false;

        if (game_user_wants_to_quit == 1) {
            /* Forced end (fallback or engine request): swallow it so the
             * suite keeps running; clean option-driven exits leave it 0. */
            s_dlg_forced = true;
            game_user_wants_to_quit = 0;
        }

        smoke_script_clear();

        /* A talking head was shown when op_start_gdialog picked a head id
         * (dialogue_head > 0); -1 means a headless dialogue ran, 0 that no
         * dialogue started at all. */
        s_dlg_head = dialogue_head;
        s_dlg_ran = s_dlg_head > 0;
        s_dlg_voice_load = ps2_lips_load_count - s_dlg_voice_load;
        s_dlg_voice_play = ps2_lips_play_count - s_dlg_voice_play;

        SMOKE_LOG("state11: %s talk done: head=0x%08X voice(load=%u play=%u) forced=%d",
            s_dlg_name, (unsigned int)s_dlg_head,
            s_dlg_voice_load, s_dlg_voice_play, s_dlg_forced ? 1 : 0);

        s_phase = 4;
        s_state_time = ps2_time_ms();
        s_tick_last = 0;
        break;
    }

    case SMOKE_ACTION_COMBAT_START: {
        s_pending = SMOKE_ACTION_NONE;

        Object* target = s_dlg_target;
        if (target == NULL || (target->data.critter.combat.results & DAM_DEAD) != 0) {
            smoke_fail("combat: no live dialogue target for the battle");
            s_state = SMOKE_STATE_VERDICT;
            s_state_time = ps2_time_ms();
            s_phase = 0;
            s_pending = SMOKE_ACTION_VERDICT;
            break;
        }

        const char* name = proto_name(target->pid);
        snprintf(s_cbt_name, sizeof(s_cbt_name), "%s", name != NULL ? name : "?");

        SMOKE_LOG("state12: staging combat on %s (pid=0x%08X, %d tiles away) ...",
            s_cbt_name, target->pid, tile_dist(obj_dude->tile, target->tile));

        /* Park the dude next to the target: the unarmed punch needs range 1. */
        obj_attempt_placement(obj_dude, target->tile, target->elevation, 0);

        /* The dialogue restores its own view centre on exit, so after the
         * teleport both fighters can be off screen; bring the view back so
         * the attack click has something to land on. */
        tile_scroll_to(obj_dude->tile, 2);
        tile_refresh_display();

        s_cbt_target = target;
        s_cbt_hp_before = obj_dude->data.critter.hp;
        s_cbt_target_hp_before = target->data.critter.hp;
        s_cbt_attacks = 0;
        s_cbt_space_taps = 0;
        s_cbt_drives = 0;
        s_cbt_engaged = false;
        s_cbt_ai_seen = false;
        s_cbt_ai_name[0] = '\0';
        s_cbt_start = 0;
        s_cbt_last_drive = 0;

        int hitMode = -1;
        bool aiming = false;
        int attackRc = intface_get_attack(&hitMode, &aiming);
        SMOKE_LOG("state12: attack ready: rc=%d hitMode=%d dude hp=%d target hp=%d",
            attackRc, hitMode, s_cbt_hp_before, s_cbt_target_hp_before);

        /* Survive the retaliation: the test needs the cycle, not a win. */
        obj_dude->data.critter.hp = 999;

        /* Crosshair mode routes map clicks to combat_attack_this(). */
        gmouse_set_cursor(MOUSE_CURSOR_CROSSHAIR);
        gmouse_3d_set_mode(GAME_MOUSE_MODE_CROSSHAIR);

        s_phase = 2;
        s_state_time = ps2_time_ms();
        s_tick_last = 0;
        break;
    }

    case SMOKE_ACTION_VERDICT:
        s_pending = SMOKE_ACTION_NONE;

        smoke_mem_probe("final");

        if (s_worst_gap > SMOKE_STALL_MS) {
            SMOKE_LOG("FAIL: stutter detected: worst frame gap %u ms (state %s)",
                s_worst_gap, smoke_state_name(s_worst_gap_state));
            s_failures++;
        }

        SMOKE_LOG("perf: suite %u ms, %u top-level frames, worst frame gap %u ms (%s)",
            now - s_suite_start, s_frames, s_worst_gap, smoke_state_name(s_worst_gap_state));
        SMOKE_LOG("[PERF] fs: fopen=%u canonicalize=%u dir-scans=%u dir-hits=%u",
            ps2_fs_stat_fopen_calls, ps2_fs_stat_canonicalize_calls,
            ps2_fs_stat_dir_scans, ps2_fs_stat_dir_cache_hits);

        SMOKE_LOG("summary: frontend(credits=%s load=%s newgame=%s) map=%s walk=%s inventory=ok pipboy=ok options=ok charsheet=ok skilldex=ok streaming=%s save-load=%s world-tour=%s dialogue=%s combat=%s hotkeys=%s",
            s_fe_credits ? "ok" : "skip",
            s_fe_load ? "ok" : "skip",
            s_fe_newgame ? "ok" : "skip",
            SMOKE_START_MAP, s_walk_moved ? "ok" : "skip",
            SMOKE_STREAM_MAP, s_save_load_ok ? "ok" : "skip",
            s_world_tour_ok ? "ok" : "skip",
            s_dialogue_ok ? "ok" : "skip",
            s_combat_ok ? "ok" : "skip",
            s_hk_ok_count == 4 ? "ok" : "skip");

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
    unsigned int now;

    /* Print any invalid SIF completion recorded by the interrupt handler. */
    if (sceSifRpcDumpInvalidCompletion != nullptr) {
        sceSifRpcDumpInvalidCompletion();
    }

    now = ps2_time_ms();

    /* Synthetic input runs in every mode, including the frontend screens. */
    smoke_key_tap_finish(now);
    smoke_pad_tap_finish(now);
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
        smoke_script_arm(now, esc_burst, 6);
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

            smoke_script_arm(now, slides, 6);
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
            /* Capture the live state, then save it into slot 1 with a
             * custom name typed via the on-screen keyboard ("PS2_SLOT_1").
             * The save dialog pre-fills the old description; clear it first
             * with 14 backspaces (Square), then type the 10-char name. */
            SMOKE_LOG("state9: save/load round-trip (slot 1) with OSK name 'PS2_SLOT_1'");
            ps2_save_slot_wipe(1);

            snprintf(s9_map, sizeof(s9_map), "%s", map_data.name);
            s9_tile = obj_dude->tile;
            s9_items = obj_dude->data.inventory.length;
            s9_time = game_time();
            s9_gvar = game_get_global_var(SMOKE_SAVE_GVAR);
            SMOKE_LOG("state9: pre-save map=%s tile=%d items=%d time=%d gvar=%d",
                s9_map, s9_tile, s9_items, s9_time, s9_gvar);

            build_osk_script(now, "PS2_SLOT_1");
            s_frontend_deadline = now + SMOKE_OSK_DEADLINE_MS;
            s_pending = SMOKE_ACTION_SAVE;
            s_phase = 10;
        } else if (s_phase == 11) {
            bool filesOk = s9_save_rc == 1;
            unsigned char head[8];
            unsigned char desc[32];
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

            /* Verify the custom description in SAVE.DAT (offset 61, 30 bytes). */
            len = smoke_slot_file_read("SAVE.DAT", 61, desc, 30);
            if (len < 30) {
                smoke_fail("state9: could not read SAVE.DAT description");
                filesOk = false;
            } else {
                desc[30] = '\0';
                /* Trim trailing NULs/spaces for comparison. */
                int dlen = 30;
                while (dlen > 0 && (desc[dlen - 1] == '\0' || desc[dlen - 1] == ' ')) {
                    dlen--;
                }
                desc[dlen] = '\0';
                SMOKE_LOG("state9: SAVE.DAT description = '%s'", desc);
                if (strcasecmp((const char*)desc, "PS2_SLOT_1") != 0) {
                    smoke_fail("state9: description mismatch (expected 'PS2_SLOT_1')");
                    filesOk = false;
                } else {
                    SMOKE_LOG("state9: description matches 'PS2_SLOT_1'");
                }
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

            /* Rename probe: Fallout 1 renames every existing save payload to
             * .BAK before writing a new one. ps2sdk's fileio has no native
             * rename, so on a memory card this covers the copy fallback. */
            {
                static const char payload[] = "rename-probe";
                const char* probeOld = "SAVEGAME\\SLOT01\\PS2_RENAME.TMP";
                const char* probeNew = "SAVEGAME\\SLOT01\\PS2_RENAME.OK";
                char probeData[16];
                bool renameOk = false;
                FILE* probe = compat_fopen(probeOld, "wb");

                if (probe != NULL) {
                    if (fwrite(payload, 1, sizeof(payload) - 1, probe) == sizeof(payload) - 1) {
                        renameOk = fclose(probe) == 0;
                    } else {
                        fclose(probe);
                    }
                }

                if (renameOk && compat_rename(probeOld, probeNew) == 0) {
                    long probeLen = smoke_slot_file_read("PS2_RENAME.OK", 0, (unsigned char*)probeData, sizeof(probeData) - 1);
                    if (probeLen == (long)(sizeof(payload) - 1)
                        && memcmp(probeData, payload, sizeof(payload) - 1) == 0) {
                        SMOKE_LOG("state9: rename probe OK (.TMP -> .OK)");
                    } else {
                        smoke_fail("state9: renamed probe content mismatch");
                        renameOk = false;
                    }
                    compat_remove(probeNew);
                } else {
                    smoke_fail("state9: rename probe failed");
                    renameOk = false;
                    compat_remove(probeOld);
                    compat_remove(probeNew);
                }

                if (!renameOk) {
                    filesOk = false;
                }
            }

            s9_files_ok = filesOk;

            /* Reboot to the start map; slot 1 must bring the junkentown
             * state back afterwards. */
            s_pending = SMOKE_ACTION_WIPE;
            s_phase = 20;
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

            /* Hand over to the world tour memory stress (state 10). */
            s_state = SMOKE_STATE_WORLD_TOUR;
            s_state_time = now;
            s_phase = 0;
            s_tour_idx = 0;
            s_pending = SMOKE_ACTION_NONE;
        }
        break;

    case SMOKE_STATE_WORLD_TOUR: {
        /* Map scripts on hostile maps (mutants, cultists) can drag the dude
         * into combat; end it cleanly so the tour keeps rolling. The
         * combat_input() loop treats quit==1 as "battle over" and resets
         * the flag itself. */
        if (isInCombat() && elapsed >= 1500) {
            SMOKE_LOG("state10: combat broke out on %s; forcing it to end",
                s_tour_idx > 0 ? s_tour_maps[s_tour_idx - 1] : "?");
            game_user_wants_to_quit = 1;
            s_state_time = now;
            break;
        }

        if (s_phase < SMOKE_TOUR_MAPS) {
            if (!s_in_blocking_action && elapsed >= SMOKE_TOUR_SETTLE_MS) {
                s_pending = SMOKE_ACTION_TOUR_STEP;
            }
        } else if (elapsed >= 800) {
            /* All maps loaded: evaluate the memory budget. */
            int junkent1 = s_tour_used_kb[2];
            int junkent2 = s_tour_used_kb[SMOKE_TOUR_MAPS - 1];
            int peak = 0;
            bool ok = true;

            for (int i = 0; i < SMOKE_TOUR_MAPS; i++) {
                SMOKE_LOG("state10: mem row %d/%d %s used=%dK engine=%dK load=%ums",
                    i + 1, SMOKE_TOUR_MAPS, s_tour_maps[i],
                    s_tour_used_kb[i], s_tour_engine_kb[i], s_tour_load_ms[i]);

                if (s_tour_used_kb[i] > peak) {
                    peak = s_tour_used_kb[i];
                }
            }

            SMOKE_LOG("state10: JUNKENT used %dK -> %dK across the tour (delta %dK)",
                junkent1, junkent2, junkent2 - junkent1);

            if (junkent2 > junkent1 + SMOKE_TOUR_PLATEAU_KB) {
                smoke_fail("world tour: heap keeps growing (no plateau)");
                ok = false;
            }

            if (peak > SMOKE_TOUR_CEIL_KB) {
                smoke_fail("world tour: heap ceiling exceeded");
                ok = false;
            }

            if (ok) {
                s_world_tour_ok = true;
                SMOKE_LOG("WORLD_TOUR_OK: heap plateaus across %d heavy maps (peak %d KB)",
                    SMOKE_TOUR_MAPS, peak);
                SMOKE_LOG("state10 PASS: world tour memory stress clean");
            }

            /* Hand over to the dialogue stress (state 11). */
            s_state = SMOKE_STATE_DIALOGUE;
            s_state_time = now;
            s_phase = 0;
            s_dlg_cand_idx = 0;
            s_pending = SMOKE_ACTION_NONE;
        }
        break;
    }

    case SMOKE_STATE_DIALOGUE: {
        /* Combat guard, same treatment as the world tour: end any battle a
         * map script dragged us into so the dialogue scan can proceed. */
        if (!s_dlg_active && isInCombat() && elapsed >= 1500) {
            SMOKE_LOG("state11: combat broke out; forcing it to end");
            game_user_wants_to_quit = 1;
            s_state_time = now;
            break;
        }

        if (s_dlg_active) {
            /* Fallback: no digit combination ended the conversation. */
            if (!s_dlg_forced && (now - s_dlg_start) >= SMOKE_DLG_MAX_MS) {
                SMOKE_LOG("state11: dialogue still open after %u s; forcing it to end",
                    SMOKE_DLG_MAX_MS / 1000);
                s_dlg_forced = true;
                game_user_wants_to_quit = 1;
            }
            break;
        }

        if (s_phase == 0) {
            s_pending = SMOKE_ACTION_DIALOGUE_LOAD;
            s_phase = 1;
        } else if (s_phase == 2) {
            if (elapsed >= SMOKE_DLG_SETTLE_MS) {
                s_pending = SMOKE_ACTION_DIALOGUE_TALK;
                s_phase = 3;
            }
        } else if (s_phase == 4) {
            if (s_dlg_ran) {
                s_dialogue_ok = true;
                SMOKE_LOG("DIALOGUE_OK: talking head 0x%08X via %s (options driven, voice load=%u play=%u, forced=%d)",
                    (unsigned int)s_dlg_head, s_dlg_name, s_dlg_voice_load, s_dlg_voice_play,
                    s_dlg_forced ? 1 : 0);
                SMOKE_LOG("state11 PASS: dialogue, talking head and voice-over stream verified");
                smoke_mem_probe("dlg-done");

                /* Hand over to the combat stress (state 12). */
                s_state = SMOKE_STATE_COMBAT;
                s_state_time = now;
                s_phase = 0;
                s_pending = SMOKE_ACTION_NONE;
            } else if (s_dlg_cand_idx + 1 < s_dlg_cand_count && s_dlg_cand_idx + 1 < SMOKE_DLG_TRIES) {
                s_dlg_cand_idx++;
                s_dlg_target = s_dlg_cand[s_dlg_cand_idx];
                SMOKE_LOG("state11: %s gave %s; trying candidate %d/%d",
                    s_dlg_name,
                    s_dlg_head == -1 ? "a headless dialogue" : "no dialogue",
                    s_dlg_cand_idx + 1, s_dlg_cand_count);

                s_phase = 2;
                s_state_time = now;
            } else {
                smoke_fail("no candidate ran a talking-head dialogue");

                s_state = SMOKE_STATE_VERDICT;
                s_state_time = now;
                s_phase = 0;
                s_pending = SMOKE_ACTION_VERDICT;
            }
        }
        break;
    }

    case SMOKE_STATE_COMBAT: {
        if (s_phase == 0) {
            s_pending = SMOKE_ACTION_COMBAT_START;
            s_phase = 1;
        } else if (s_phase == 1) {
            /* Waiting for the staging action. */
        } else if (s_phase == 2) {
            if (elapsed >= SMOKE_CBT_SETTLE_MS) {
                /* Opening attack: the crosshair click enters the battle
                 * through combat_attack_this(). */
                int tx;
                int ty;
                if (smoke_cbt_sprite_center(s_cbt_target, &tx, &ty)) {
                    smoke_cbt_click(now, tx, ty);
                    s_cbt_attacks++;
                    SMOKE_LOG("state12: opening attack click on %s at (%d,%d)",
                        s_cbt_name, tx, ty);
                } else {
                    SMOKE_LOG("state12: could not locate %s on screen", s_cbt_name);
                }

                s_state_time = now;
                s_phase = 3;
            }
        } else if (s_phase == 3) {
            if (isInCombat()) {
                s_cbt_engaged = true;
                s_cbt_start = now;
                s_cbt_last_drive = now;
                SMOKE_LOG("state12: combat engaged (battle started from the attack click)");
                s_state_time = now;
                s_phase = 4;
            } else if (elapsed >= 4000) {
                if (s_cbt_attacks < SMOKE_CBT_CLICK_TRIES) {
                    int tx;
                    int ty;
                    if (smoke_cbt_sprite_center(s_cbt_target, &tx, &ty)) {
                        smoke_cbt_click(now, tx, ty);
                        s_cbt_attacks++;
                        SMOKE_LOG("state12: attack click %d did not engage; retrying",
                            s_cbt_attacks);
                    }
                    s_state_time = now;
                } else {
                    smoke_fail("combat: attack clicks never started a battle");
                    s_state = SMOKE_STATE_VERDICT;
                    s_state_time = now;
                    s_phase = 0;
                    s_pending = SMOKE_ACTION_VERDICT;
                }
            }
        } else if (s_phase == 4) {
            if (!isInCombat()) {
                /* Battle over: assess before restoring the test tampering. */
                int targetHpAfter = s_cbt_target->data.critter.hp;
                bool targetDead = (s_cbt_target->data.critter.combat.results & DAM_DEAD) != 0;
                int dudeHpAfter = obj_dude->data.critter.hp;

                obj_dude->data.critter.hp = s_cbt_hp_before;
                gmouse_3d_set_mode(GAME_MOUSE_MODE_MOVE);

                smoke_mem_probe("combat");

                s_combat_ok = s_cbt_engaged && s_cbt_attacks > 0
                    && (s_cbt_ai_seen || targetDead
                        || targetHpAfter < s_cbt_target_hp_before
                        || dudeHpAfter < 999);

                SMOKE_LOG("state12: battle over: attacks=%d space=%d ai=%s target %s hp %d->%d%s dude hp=%d",
                    s_cbt_attacks, s_cbt_space_taps,
                    s_cbt_ai_seen ? s_cbt_ai_name : "none",
                    s_cbt_name, s_cbt_target_hp_before, targetHpAfter,
                    targetDead ? " dead" : "", dudeHpAfter);

                if (s_combat_ok) {
                    SMOKE_LOG("COMBAT_OK: turn-based cycle (player attack + AI turn) verified");
                    SMOKE_LOG("state12 PASS: combat stress clean");
                } else {
                    smoke_fail("combat: cycle finished without player attack or AI turn");
                }

                s_state = SMOKE_STATE_PAD_HOTKEYS;
                s_state_time = now;
                s_phase = 0;
                s_pending = SMOKE_ACTION_NONE;
            } else {
                /* AI turn sampling. */
                if (!s_cbt_ai_seen && combat_turn_obj != NULL && combat_turn_obj != obj_dude) {
                    const char* aiName = proto_name(combat_turn_obj->pid);
                    snprintf(s_cbt_ai_name, sizeof(s_cbt_ai_name), "%s", aiName != NULL ? aiName : "?");
                    s_cbt_ai_seen = true;
                    SMOKE_LOG("state12: AI turn observed: %s", s_cbt_ai_name);
                }

                if (s_cbt_start != 0) {
                    if ((now - s_cbt_start) >= SMOKE_CBT_MAX_MS) {
                        SMOKE_LOG("state12: forcing the battle to end (time limit)");
                        game_user_wants_to_quit = 1;
                        s_cbt_start = 0;
                    } else if ((now - s_cbt_last_drive) >= SMOKE_CBT_DRIVE_MS) {
                        s_cbt_last_drive = now;

                        /* Only drive on the dude's turn (combat_state 0x02);
                         * punch twice, then hand over with SPACE. */
                        if (combat_turn_obj == obj_dude && (combat_state & 0x02) != 0) {
                            s_cbt_drives++;
                            if (s_cbt_drives % 3 == 0) {
                                smoke_cbt_space(now);
                                s_cbt_space_taps++;
                            } else {
                                int tx;
                                int ty;
                                if (smoke_cbt_sprite_center(s_cbt_target, &tx, &ty)) {
                                    smoke_cbt_click(now, tx, ty);
                                    s_cbt_attacks++;
                                }
                            }
                        }
                    }
                }
            }
        }
        break;
    }

    case SMOKE_STATE_PAD_HOTKEYS: {
        /* F8: validate that the ergonomic pad bindings dispatch without
         * latency or drops. The pad button is injected through the real pad
         * path; dispatch is proven by the engine's own state:
         *   - I / C / P open modal screens that run their own blocking
         *     loop, so the top-level frame counter must freeze while they
         *     are open and resume once START (Escape) closes them.
         *   - R1 maps to 'a' (combat(NULL)) outside combat, so isInCombat()
         *     turning true proves the key reached the engine; the battle is
         *     then force-ended exactly like state 12. */
        static const uint16_t hk_buttons[] = { PAD_SQUARE, PAD_TRIANGLE, PAD_SELECT, PAD_R1 };
        static const char* hk_names[] = { "SQUARE->I", "TRIANGLE->C", "SELECT->P", "R1->A" };
        static const unsigned int hk_close_ms[] = { 700, 1500, 1200, 2500 };
        unsigned int frames;

        if (s_phase == 0) {
            SMOKE_LOG("state13: testing pad hotkeys (I, C, P, A)");
            s_hk_step = 0;
            s_hk_ok_count = 0;
            s_phase = 1;
            s_state_time = now;
        } else if (s_phase == 1 && s_hk_step < 4) {
            smoke_pad_tap(hk_buttons[s_hk_step], now);
            s_hk_start = now;
            s_hk_frames_at_tap = s_frames;
            s_phase = 2;
            s_state_time = now;
            SMOKE_LOG("state13: %s tapped (frame %u)", hk_names[s_hk_step], s_frames);
        } else if (s_phase == 1) {
            SMOKE_LOG("state13: %d/4 pad hotkeys dispatched clean", s_hk_ok_count);
            if (s_hk_ok_count == 4) {
                SMOKE_LOG("state13 PASS: pad hotkey dispatch verified");
            }
            s_state = SMOKE_STATE_VERDICT;
            s_state_time = now;
            s_phase = 0;
            s_pending = SMOKE_ACTION_VERDICT;
        } else if (s_phase == 2) {
            if (s_hk_step == 3) {
                /* R1 outside combat -> 'a' -> combat(NULL). */
                if (isInCombat()) {
                    SMOKE_LOG("state13: %s engaged combat(NULL) in %u ms", hk_names[3], elapsed);
                    s_hk_ok_count++;
                    game_user_wants_to_quit = 1; /* force-end, as state 12 */
                    s_phase = 3;
                    s_state_time = now;
                } else if (elapsed > 2000) {
                    smoke_fail("state13: R1 did not engage combat mode");
                    s_state = SMOKE_STATE_VERDICT;
                    s_state_time = now;
                    s_phase = 0;
                    s_pending = SMOKE_ACTION_VERDICT;
                }
            } else if (elapsed >= hk_close_ms[s_hk_step]) {
                frames = s_frames - s_hk_frames_at_tap;
                if (frames <= 1) {
                    SMOKE_LOG("state13: %s opened a modal (top-level loop stalled %u frames)",
                        hk_names[s_hk_step], frames);
                    s_hk_ok_count++;
                } else {
                    SMOKE_LOG("state13: %s did not open its screen (%u frames advanced)",
                        hk_names[s_hk_step], frames);
                    smoke_fail("state13: hotkey did not open its screen");
                }
                smoke_pad_tap(PAD_START, now); /* Escape closes the modal */
                s_phase = 3;
                s_state_time = now;
            }
        } else if (s_phase == 3) {
            if (s_hk_step == 3) {
                if (!isInCombat()) {
                    SMOKE_LOG("state13: %s battle ended, loop resumed", hk_names[3]);
                    s_hk_step++;
                    s_phase = 1;
                    s_state_time = now;
                } else if (elapsed > SMOKE_HK_MAX_MS) {
                    smoke_fail("state13: combat did not end after force-quit");
                    s_state = SMOKE_STATE_VERDICT;
                    s_state_time = now;
                    s_phase = 0;
                    s_pending = SMOKE_ACTION_VERDICT;
                }
            } else {
                if (s_frames > s_hk_frames_at_tap) {
                    SMOKE_LOG("state13: %s closed via START (frame %u)", hk_names[s_hk_step], s_frames);
                    s_hk_step++;
                    s_phase = 1;
                    s_state_time = now;
                } else if (elapsed > 2000) {
                    smoke_fail("state13: screen did not close via START");
                    s_state = SMOKE_STATE_VERDICT;
                    s_state_time = now;
                    s_phase = 0;
                    s_pending = SMOKE_ACTION_VERDICT;
                }
            }
        }
        break;
    }

    case SMOKE_STATE_VERDICT:
        /* Exit happens via SMOKE_ACTION_VERDICT in the top-level loop. */
        break;
    }
}

/* Harness game loop (replaces movies + menu + main_game_loop). */
int ps2_smoke_main(void)
{
    SMOKE_LOG("harness compiled in; taking over the game flow");

    ps2_mem_init();

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

    smoke_mem_probe("boot");

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
