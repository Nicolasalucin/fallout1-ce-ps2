#ifndef PS2_INPUT_H_
#define PS2_INPUT_H_

/*
 * Input backend contract for the Fallout PS2 port.
 *
 * The DualShock 2 pad is translated into synthetic SDL2 mouse/keyboard
 * events (see sdl/ps2_sdl_internal.h for the posting helpers). The engine's
 * own input logic (kb.cc, input.cc, mouse.cc) consumes these events
 * untouched.
 */

#ifdef __cplusplus
extern "C" {
#endif

void ps2_input_init(void);

/* Called by the SDL shim on every event pump (at most a few times a frame;
 * the pad is only actually polled once per frame internally). */
void ps2_input_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* PS2_INPUT_H_ */
