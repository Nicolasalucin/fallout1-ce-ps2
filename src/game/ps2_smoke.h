#ifndef FALLOUT_GAME_PS2_SMOKE_H_
#define FALLOUT_GAME_PS2_SMOKE_H_

/*
 * Automated in-engine smoke test harness (dev builds only).
 *
 * Enabled with -DPS2_DEV_SMOKE_TEST=ON (tools/run_smoke.sh does this). When
 * compiled in, gnw_main() hands control to the harness right after engine
 * init: the intro movies and the main menu are skipped, a premade dude is
 * booted straight into the starting map and a scripted sequence exercises
 * the map, click-to-move, the inventory, the Pip-Boy, map streaming and
 * (once F6 lands) the save/load round-trip. The harness logs [ps2][smoke]
 * lines to the EE console and terminates the ELF with exit(), so
 * tools/run_smoke.sh can grep the verdict out of the emulator log without
 * any user interaction. A stall detector reports the worst frame gap, so
 * freezes (like movement stutters) fail the suite instead of passing
 * unnoticed.
 */

#ifdef PS2_DEV_SMOKE_TEST

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the SDL shim on every event pump. Advances the state machine:
 * posts synthetic input and advances timers only - heavy engine work is
 * deferred to the harness' own game loop. */
void ps2_smoke_tick(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace fallout {

/* Replaces the movies + main menu flow when the harness is compiled in.
 * Never returns. */
int ps2_smoke_main(void);

} // namespace fallout
#endif

#endif /* PS2_DEV_SMOKE_TEST */

#endif /* FALLOUT_GAME_PS2_SMOKE_H_ */
