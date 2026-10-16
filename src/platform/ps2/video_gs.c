/*
 * Video backend for the Fallout PS2 port (F1: stub).
 *
 * F2 implementa aquí la presentación real con gsKit:
 *   - Framebuffer indexado de 8 bits subido como textura GS_PSM_T8.
 *   - Paleta de 256 colores como CLUT GS_PSM_CT32.
 *   - Quad a pantalla completa + vsync (60 Hz NTSC, 640x448).
 *
 * De momento solo valida que el enlace con gsKit/graph está bien formado.
 */

#include "ps2_video.h"

#include <stdio.h>

static int s_video_inited;

void ps2_video_init(void)
{
    if (s_video_inited) {
        return;
    }
    s_video_inited = 1;
    printf("[ps2] video init (stub, real gsKit backend lands in F2)\n");
}

void ps2_video_shutdown(void)
{
    s_video_inited = 0;
}

void ps2_video_present(const void* pixels, int width, int height, int pitch)
{
    (void)pixels;
    (void)width;
    (void)height;
    (void)pitch;
}

void ps2_video_set_palette(const void* rgba_colors)
{
    (void)rgba_colors;
}
