#ifndef PS2_VIDEO_H_
#define PS2_VIDEO_H_

/*
 * Video backend contract for the Fallout PS2 port.
 *
 * The engine works on a single 8-bit indexed 640x480 buffer. This module is
 * responsible for presenting it 60 times per second using the GS hardware as
 * a 256-color indexed surface (GS_PSM_T8 texture + GS_PSM_CT32 CLUT); no CPU
 * color conversion is performed.
 *
 * Called from the SDL shim (sdl_shim.c), which forwards:
 *   - SDL_CreateRenderer   -> ps2_video_init
 *   - SDL_DestroyRenderer  -> ps2_video_shutdown
 *   - SDL_RenderPresent    -> ps2_video_present
 *   - palette updates      -> ps2_video_set_palette
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ps2_video_init(void);
void ps2_video_shutdown(void);

/* Present an 8-bit indexed frame (screen buffer owned by the engine). */
void ps2_video_present(const void* pixels, int width, int height, int pitch);

/* Update the hardware CLUT. `rgba_colors` points to 256 uint32 entries in
 * GS CT32 layout: alpha<<24 | blue<<16 | green<<8 | red. */
void ps2_video_set_palette(const void* rgba_colors);

#ifdef __cplusplus
}
#endif

#endif /* PS2_VIDEO_H_ */
