/*
 * SDL2 subset implementation for the Fallout PS2 port.
 *
 * The engine (fallout1-ce) talks to SDL2 in a well-defined, small subset
 * (see ../sdl/SDL.h). This file implements that subset on top of native PS2
 * services, so every upstream engine file stays unmodified:
 *
 *   - Surfaces are real 8-bit indexed buffers, exactly like on desktop.
 *   - CREATE-RGBSURFACE-WITHFORMAT STORAGE TRICK: the engine creates a
 *     "RGB888 streaming texture surface" only to blit the 8-bit screen into
 *     it before upload. Storing that surface as 8bpp indexed data too makes
 *     the blit a plain memory copy and lets the GS hardware do the palette
 *     lookup through its CLUT (no CPU color conversion anywhere).
 *   - Presentation (SDL_RenderPresent) forwards the texture surface
 *     directly to the video backend (gsKit, GS_PSM_T8 + GS_PSM_CT32).
 *   - The event queue, mouse state and timers are driven by the input layer
 *     (input_pad.c) and pumped from SDL_PollEvent/SDL_PumpEvents.
 *   - Audio entry points are stubs in F1; the real implementation feeds ACM
 *     decoded PCM into audsrv (F5).
 *
 * There is no SDL library linked: these symbols ARE the implementation.
 */

#include "SDL.h"
#include "ps2_sdl_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps2_input.h"
#include "ps2_time.h"
#include "ps2_video.h"

/* ---------------------------------------------------------------------- */
/* Subsystem init state                                                    */
/* ---------------------------------------------------------------------- */

static Uint32 s_init_flags;

int SDL_InitSubSystem(Uint32 flags)
{
    s_init_flags |= flags;
    return 0;
}

Uint32 SDL_WasInit(Uint32 flags)
{
    return s_init_flags & flags;
}

void SDL_QuitSubSystem(Uint32 flags)
{
    s_init_flags &= ~flags;
}

void SDL_Quit(void)
{
    s_init_flags = 0;
}

SDL_bool SDL_SetHint(const char* name, const char* value)
{
    (void)name;
    (void)value;
    return SDL_TRUE;
}

Uint32 SDL_GetTicks(void)
{
    return ps2_time_ms();
}

void SDL_Delay(Uint32 ms)
{
    uint64_t target = ps2_time_us() + (uint64_t)ms * 1000;
    while (ps2_time_us() < target) {
        /* spin on the cycle counter; used only for frame pacing/idle */
    }
}

/* ---------------------------------------------------------------------- */
/* Opaque object definitions                                               */
/* ---------------------------------------------------------------------- */

struct SDL_Window {
    char title[256];
};

struct SDL_Renderer {
    int logical_w;
    int logical_h;
};

struct SDL_Texture {
    Uint32 format;
    int w;
    int h;
    const void* pixels;
    int pitch;
};

struct SDL_Cursor {
    int dummy;
};

struct SDL_AudioStream {
    Uint16 src_format;
    Uint8 src_channels;
    int src_rate;
    Uint16 dst_format;
    Uint8 dst_channels;
    int dst_rate;
};

/* ---------------------------------------------------------------------- */
/* Surfaces                                                                */
/* ---------------------------------------------------------------------- */

static void surface_destroy(SDL_Surface* surface)
{
    if (surface == NULL) {
        return;
    }

    if (surface->format != NULL) {
        SDL_Palette* palette = surface->format->palette;
        if (palette != NULL) {
            free(palette->colors);
            free(palette);
        }
        free(surface->format);
    }

    free(surface->pixels);
    free(surface);
}

static SDL_Surface* surface_create(int width, int height, int depth)
{
    if (width <= 0 || height <= 0) {
        return NULL;
    }

    SDL_Surface* surface = (SDL_Surface*)calloc(1, sizeof(SDL_Surface));
    SDL_PixelFormat* format = (SDL_PixelFormat*)calloc(1, sizeof(SDL_PixelFormat));
    if (surface == NULL || format == NULL) {
        free(surface);
        free(format);
        return NULL;
    }

    surface->format = format;
    surface->w = width;
    surface->h = height;
    surface->refcount = 1;

    if (depth == 8) {
        surface->pitch = width;
        surface->pixels = calloc(1, (size_t)width * height);

        format->BitsPerPixel = 8;
        format->BytesPerPixel = 1;

        SDL_Palette* palette = (SDL_Palette*)calloc(1, sizeof(SDL_Palette));
        if (palette != NULL) {
            palette->ncolors = 256;
            palette->colors = (SDL_Color*)calloc(256, sizeof(SDL_Color));
            palette->refcount = 1;
        }
        format->palette = palette;
    } else {
        int bytes_per_pixel = depth / 8;
        if (bytes_per_pixel < 1) {
            bytes_per_pixel = 1;
        }
        surface->pitch = width * bytes_per_pixel;
        surface->pixels = calloc(1, (size_t)surface->pitch * height);
        format->BitsPerPixel = (Uint8)depth;
        format->BytesPerPixel = (Uint8)bytes_per_pixel;
    }

    if (surface->pixels == NULL) {
        surface_destroy(surface);
        return NULL;
    }

    return surface;
}

SDL_Surface* SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    (void)flags;
    (void)Rmask;
    (void)Gmask;
    (void)Bmask;
    (void)Amask;

    return surface_create(width, height, depth);
}

/*
 * The engine only calls this to create its "texture surface" from the
 * SDL_PIXELFORMAT_RGB888 format reported by SDL_QueryTexture. That surface
 * is stored as 8bpp indexed data on this port: the engine blits the 8-bit
 * screen into it and the GS CLUT performs the palette lookup in hardware.
 */
SDL_Surface* SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int width, int height, int depth, Uint32 format)
{
    (void)flags;
    (void)depth;
    (void)format;

    return surface_create(width, height, 8);
}

void SDL_FreeSurface(SDL_Surface* surface)
{
    surface_destroy(surface);
}

int SDL_LockSurface(SDL_Surface* surface)
{
    (void)surface;
    return 0;
}

void SDL_UnlockSurface(SDL_Surface* surface)
{
    (void)surface;
}

/*
 * Rectangle blit with clipping. The only formats the engine produces are
 * 8bpp paletted surfaces (plus 8->32 expansion kept as a safety net for the
 * MVE movie code paths).
 */
static int blit_rect(SDL_Surface* src, const SDL_Rect* srcrect, SDL_Surface* dst, const SDL_Rect* dstrect)
{
    int sx = 0;
    int sy = 0;
    int sw = src->w;
    int sh = src->h;
    int dx = 0;
    int dy = 0;

    if (srcrect != NULL) {
        sx = srcrect->x;
        sy = srcrect->y;
        sw = srcrect->w;
        sh = srcrect->h;
    }

    if (dstrect != NULL) {
        dx = dstrect->x;
        dy = dstrect->y;
    }

    /* Clip source against the source surface. */
    if (sx < 0) { dx -= sx; sw += sx; sx = 0; }
    if (sy < 0) { dy -= sy; sh += sy; sy = 0; }
    if (sx + sw > src->w) sw = src->w - sx;
    if (sy + sh > src->h) sh = src->h - sy;

    /* Clip against the destination surface. */
    if (dx < 0) { sx -= dx; sw += dx; dx = 0; }
    if (dy < 0) { sy -= dy; sh += dy; dy = 0; }
    if (dx + sw > dst->w) sw = dst->w - dx;
    if (dy + sh > dst->h) sh = dst->h - dy;

    if (sw <= 0 || sh <= 0) {
        return 0;
    }

    int src_bpp = src->format->BitsPerPixel;
    int dst_bpp = dst->format->BitsPerPixel;

    if (src_bpp == 8 && dst_bpp == 8) {
        for (int y = 0; y < sh; y++) {
            memcpy((Uint8*)dst->pixels + (size_t)(dy + y) * dst->pitch + dx,
                (const Uint8*)src->pixels + (size_t)(sy + y) * src->pitch + sx,
                (size_t)sw);
        }
        return 0;
    }

    if (src_bpp == 8 && dst_bpp == 32) {
        const SDL_Palette* palette = src->format->palette;
        if (palette == NULL) {
            return -1;
        }
        for (int y = 0; y < sh; y++) {
            const Uint8* src_row = (const Uint8*)src->pixels + (size_t)(sy + y) * src->pitch + sx;
            Uint32* dst_row = (Uint32*)((Uint8*)dst->pixels + (size_t)(dy + y) * dst->pitch) + dx;
            for (int x = 0; x < sw; x++) {
                const SDL_Color* c = &palette->colors[src_row[x]];
                dst_row[x] = ((Uint32)c->a << 24) | ((Uint32)c->r << 16) | ((Uint32)c->g << 8) | c->b;
            }
        }
        return 0;
    }

    if (src_bpp == 32 && dst_bpp == 32) {
        for (int y = 0; y < sh; y++) {
            memcpy((Uint8*)dst->pixels + (size_t)(dy + y) * dst->pitch + (size_t)dx * 4,
                (const Uint8*)src->pixels + (size_t)(sy + y) * src->pitch + (size_t)sx * 4,
                (size_t)sw * 4);
        }
        return 0;
    }

    printf("[sdl] unsupported blit %d bpp -> %d bpp\n", src_bpp, dst_bpp);
    return -1;
}

int SDL_BlitSurface(SDL_Surface* src, const SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect)
{
    if (src == NULL || dst == NULL) {
        return -1;
    }

    return blit_rect(src, srcrect, dst, dstrect);
}

/*
 * The engine maintains a single logical game palette (the screen palette);
 * every SDL_SetPaletteColors call animates that palette (full sets and fade
 * steps alike), and movie frames share it via SDL_SetSurfacePalette. We
 * mirror every update into a global palette and re-upload the CLUT once per
 * frame while it is dirty.
 */
static SDL_Color s_game_palette[256];
static int s_palette_dirty = 1;

int SDL_SetPaletteColors(SDL_Palette* palette, const SDL_Color* colors, int firstcolor, int ncolors)
{
    if (palette == NULL || colors == NULL || firstcolor < 0 || ncolors < 0 || firstcolor + ncolors > palette->ncolors) {
        return -1;
    }

    memcpy(&palette->colors[firstcolor], colors, (size_t)ncolors * sizeof(SDL_Color));

    if (firstcolor + ncolors <= 256) {
        memcpy(&s_game_palette[firstcolor], colors, (size_t)ncolors * sizeof(SDL_Color));
        s_palette_dirty = 1;
    }

    return 0;
}

int SDL_SetSurfacePalette(SDL_Surface* surface, SDL_Palette* palette)
{
    if (surface == NULL || surface->format == NULL) {
        return -1;
    }

    surface->format->palette = palette;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Window / renderer / texture                                             */
/* ---------------------------------------------------------------------- */

static SDL_Texture* s_last_texture;

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)flags;

    SDL_Window* window = (SDL_Window*)calloc(1, sizeof(SDL_Window));
    if (window != NULL && title != NULL) {
        snprintf(window->title, sizeof(window->title), "%s", title);
    }
    return window;
}

void SDL_DestroyWindow(SDL_Window* window)
{
    free(window);
}

void SDL_SetWindowTitle(SDL_Window* window, const char* title)
{
    if (window != NULL && title != NULL) {
        snprintf(window->title, sizeof(window->title), "%s", title);
    }
}

SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, Uint32 flags)
{
    (void)window;
    (void)index;
    (void)flags;

    ps2_video_init();

    return (SDL_Renderer*)calloc(1, sizeof(SDL_Renderer));
}

void SDL_DestroyRenderer(SDL_Renderer* renderer)
{
    free(renderer);
    ps2_video_shutdown();
}

int SDL_RenderSetLogicalSize(SDL_Renderer* renderer, int w, int h)
{
    if (renderer != NULL) {
        renderer->logical_w = w;
        renderer->logical_h = h;
    }
    return 0;
}

SDL_Texture* SDL_CreateTexture(SDL_Renderer* renderer, Uint32 format, int access, int w, int h)
{
    (void)renderer;
    (void)access;

    SDL_Texture* texture = (SDL_Texture*)calloc(1, sizeof(SDL_Texture));
    if (texture != NULL) {
        texture->format = format;
        texture->w = w;
        texture->h = h;
    }
    return texture;
}

void SDL_DestroyTexture(SDL_Texture* texture)
{
    if (texture == s_last_texture) {
        s_last_texture = NULL;
    }
    free(texture);
}

int SDL_QueryTexture(SDL_Texture* texture, Uint32* format, int* access, int* w, int* h)
{
    if (texture == NULL) {
        return -1;
    }

    if (format != NULL) {
        *format = SDL_PIXELFORMAT_RGB888;
    }
    if (access != NULL) {
        *access = SDL_TEXTUREACCESS_STREAMING;
    }
    if (w != NULL) {
        *w = texture->w;
    }
    if (h != NULL) {
        *h = texture->h;
    }
    return 0;
}

int SDL_UpdateTexture(SDL_Texture* texture, const SDL_Rect* rect, const void* pixels, int pitch)
{
    (void)rect;

    if (texture == NULL) {
        return -1;
    }

    texture->pixels = pixels;
    texture->pitch = pitch;
    s_last_texture = texture;
    return 0;
}

int SDL_RenderClear(SDL_Renderer* renderer)
{
    (void)renderer;
    return 0;
}

int SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect)
{
    (void)renderer;
    (void)texture;
    (void)srcrect;
    (void)dstrect;
    return 0;
}

void SDL_RenderPresent(SDL_Renderer* renderer)
{
    (void)renderer;

    if (s_palette_dirty) {
        Uint32 rgba[256];
        for (int i = 0; i < 256; i++) {
            rgba[i] = ((Uint32)s_game_palette[i].a << 24)
                | ((Uint32)s_game_palette[i].b << 16)
                | ((Uint32)s_game_palette[i].g << 8)
                | (Uint32)s_game_palette[i].r;
        }
        ps2_video_set_palette(rgba);
        s_palette_dirty = 0;
    }

    if (s_last_texture != NULL && s_last_texture->pixels != NULL) {
        ps2_video_present(s_last_texture->pixels, s_last_texture->w, s_last_texture->h, s_last_texture->pitch);
    }
}

/* ---------------------------------------------------------------------- */
/* Cursor                                                                  */
/* ---------------------------------------------------------------------- */

static int s_cursor_visible = 1;
static SDL_Cursor s_system_cursor = { 0 };

int SDL_ShowCursor(int toggle)
{
    int previous = s_cursor_visible;
    if (toggle >= 0) {
        s_cursor_visible = toggle;
    }
    return previous;
}

SDL_Cursor* SDL_CreateSystemCursor(int id)
{
    (void)id;
    return &s_system_cursor;
}

SDL_Cursor* SDL_GetCursor(void)
{
    return &s_system_cursor;
}

void SDL_SetCursor(SDL_Cursor* cursor)
{
    (void)cursor;
}

void SDL_FreeCursor(SDL_Cursor* cursor)
{
    /* Only the static system cursor exists; nothing to free. */
    (void)cursor;
}

/* ---------------------------------------------------------------------- */
/* Events                                                                  */
/* ---------------------------------------------------------------------- */

#define EVENT_QUEUE_SIZE 64

static SDL_Event s_event_queue[EVENT_QUEUE_SIZE];
static int s_event_head;
static int s_event_count;
static int s_startup_events_posted;

/* Mouse state, shared between the event builders below and
 * SDL_GetRelativeMouseState/SDL_PollEvent consumers. */
static Sint32 s_mouse_xrel;
static Sint32 s_mouse_yrel;
static Sint32 s_mouse_x;
static Sint32 s_mouse_y;
static Uint32 s_mouse_buttons;

void ps2_sdl_push_event(const SDL_Event* event)
{
    SDL_Event e = *event;

    if (s_event_count == EVENT_QUEUE_SIZE) {
        /* Drop the oldest event; newest input must keep flowing. */
        s_event_head = (s_event_head + 1) % EVENT_QUEUE_SIZE;
        s_event_count--;
    }

    s_event_queue[(s_event_head + s_event_count) % EVENT_QUEUE_SIZE] = e;
    s_event_count++;
}

static int event_pop(SDL_Event* event)
{
    if (s_event_count <= 0) {
        return 0;
    }

    *event = s_event_queue[s_event_head];
    s_event_head = (s_event_head + 1) % EVENT_QUEUE_SIZE;
    s_event_count--;
    return 1;
}

void ps2_sdl_post_key(int scancode, int down)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = (SDL_Scancode)scancode;
    ps2_sdl_push_event(&e);
}

void ps2_sdl_post_mouse_move(int dx, int dy)
{
    if (dx == 0 && dy == 0) {
        return;
    }

    s_mouse_xrel += dx;
    s_mouse_yrel += dy;
    s_mouse_x += dx;
    s_mouse_y += dy;

    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_MOUSEMOTION;
    e.motion.x = s_mouse_x;
    e.motion.y = s_mouse_y;
    e.motion.xrel = dx;
    e.motion.yrel = dy;
    ps2_sdl_push_event(&e);
}

void ps2_sdl_post_mouse_button(int sdl_button, int down)
{
    if (sdl_button < 1 || sdl_button > 32) {
        return;
    }

    if (down) {
        s_mouse_buttons |= SDL_BUTTON(sdl_button);
    } else {
        s_mouse_buttons &= ~(Uint32)SDL_BUTTON(sdl_button);
    }

    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.button = (Uint8)sdl_button;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    ps2_sdl_push_event(&e);
}

void ps2_sdl_post_mouse_wheel(int dx, int dy)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_MOUSEWHEEL;
    e.wheel.x = dx;
    e.wheel.y = dy;
    ps2_sdl_push_event(&e);
}

void ps2_sdl_post_window_event(int window_event)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_WINDOWEVENT;
    e.window.event = (Uint8)window_event;
    ps2_sdl_push_event(&e);
}

/* ---------------------------------------------------------------------- */
/* Mouse / keyboard state                                                  */
/* ---------------------------------------------------------------------- */

static int s_text_input_active;

Uint32 SDL_GetRelativeMouseState(int* x, int* y)
{
    if (x != NULL) {
        *x = s_mouse_xrel;
    }
    if (y != NULL) {
        *y = s_mouse_yrel;
    }

    s_mouse_xrel = 0;
    s_mouse_yrel = 0;

    return s_mouse_buttons;
}

SDL_Keymod SDL_GetModState(void)
{
    /* PS2 has no lock keys; the pad never reports modifiers. */
    return KMOD_NONE;
}

int SDL_SetRelativeMouseMode(SDL_bool enabled)
{
    (void)enabled;
    return 0;
}

void SDL_StartTextInput(void)
{
    s_text_input_active = 1;
}

void SDL_StopTextInput(void)
{
    s_text_input_active = 0;
}

/* ---------------------------------------------------------------------- */
/* Timers                                                                  */
/* ---------------------------------------------------------------------- */

#define TIMER_SLOTS 16

typedef struct TimerSlot {
    SDL_TimerID id;
    Uint32 interval;
    Uint32 next_time;
    Uint32 (*callback)(Uint32 interval, void* param);
    void* param;
    int active;
} TimerSlot;

static TimerSlot s_timers[TIMER_SLOTS];
static SDL_TimerID s_next_timer_id = 1;

SDL_TimerID SDL_AddTimer(Uint32 interval, Uint32 (*callback)(Uint32 interval, void* param), void* param)
{
    if (interval == 0) {
        interval = 1;
    }

    for (int i = 0; i < TIMER_SLOTS; i++) {
        if (!s_timers[i].active) {
            s_timers[i].id = s_next_timer_id++;
            s_timers[i].interval = interval;
            s_timers[i].callback = callback;
            s_timers[i].param = param;
            s_timers[i].next_time = ps2_time_ms() + interval;
            s_timers[i].active = 1;
            return s_timers[i].id;
        }
    }

    return 0;
}

SDL_bool SDL_RemoveTimer(SDL_TimerID id)
{
    for (int i = 0; i < TIMER_SLOTS; i++) {
        if (s_timers[i].active && s_timers[i].id == id) {
            s_timers[i].active = 0;
            return SDL_TRUE;
        }
    }

    return SDL_FALSE;
}

/* Timers run on the main thread when pumping events (see note in SDL.h). */
static void pump_timers(void)
{
    Uint32 now = ps2_time_ms();

    for (int i = 0; i < TIMER_SLOTS; i++) {
        if (s_timers[i].active && (Sint32)(now - s_timers[i].next_time) >= 0) {
            Uint32 interval = s_timers[i].interval;
            Uint32 (*callback)(Uint32, void*) = s_timers[i].callback;
            void* param = s_timers[i].param;

            Uint32 next = callback(interval, param);

            /* The callback may have removed itself. */
            if (s_timers[i].active) {
                if (next == 0) {
                    s_timers[i].active = 0;
                } else {
                    s_timers[i].interval = next;
                    s_timers[i].next_time = now + next;
                }
            }
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Pumping                                                                 */
/* ---------------------------------------------------------------------- */

static void sdl_pump(void)
{
    if (!s_startup_events_posted) {
        s_startup_events_posted = 1;

        /* The engine treats the window as inactive until it receives a
         * focus event; fake it so input processing is enabled from boot. */
        ps2_sdl_post_window_event(SDL_WINDOWEVENT_FOCUS_GAINED);
        ps2_sdl_post_window_event(SDL_WINDOWEVENT_EXPOSED);
    }

    ps2_input_poll();
    pump_timers();
}

void SDL_PumpEvents(void)
{
    sdl_pump();
}

int SDL_PollEvent(SDL_Event* event)
{
    sdl_pump();

    if (event == NULL) {
        return s_event_count > 0;
    }

    return event_pop(event);
}

void SDL_FlushEvents(Uint32 minType, Uint32 maxType)
{
    SDL_Event kept[EVENT_QUEUE_SIZE];
    int kept_count = 0;

    for (int i = 0; i < s_event_count; i++) {
        SDL_Event* e = &s_event_queue[(s_event_head + i) % EVENT_QUEUE_SIZE];
        if (e->type < minType || e->type >= maxType) {
            kept[kept_count++] = *e;
        }
    }

    memcpy(s_event_queue, kept, (size_t)kept_count * sizeof(SDL_Event));
    s_event_head = 0;
    s_event_count = kept_count;
}

/* ---------------------------------------------------------------------- */
/* String helpers (used by platform_compat.cc)                             */
/* ---------------------------------------------------------------------- */

int SDL_strcasecmp(const char* str1, const char* str2)
{
    while (*str1 != '\0' && *str2 != '\0') {
        int c1 = tolower((unsigned char)*str1);
        int c2 = tolower((unsigned char)*str2);
        if (c1 != c2) {
            return c1 - c2;
        }
        str1++;
        str2++;
    }
    return tolower((unsigned char)*str1) - tolower((unsigned char)*str2);
}

int SDL_strncasecmp(const char* str1, const char* str2, size_t len)
{
    while (len > 0 && *str1 != '\0' && *str2 != '\0') {
        int c1 = tolower((unsigned char)*str1);
        int c2 = tolower((unsigned char)*str2);
        if (c1 != c2) {
            return c1 - c2;
        }
        str1++;
        str2++;
        len--;
    }
    if (len == 0) {
        return 0;
    }
    return tolower((unsigned char)*str1) - tolower((unsigned char)*str2);
}

char* SDL_strupr(char* str)
{
    for (char* p = str; *p != '\0'; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
    return str;
}

char* SDL_strlwr(char* str)
{
    for (char* p = str; *p != '\0'; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
    return str;
}

char* SDL_itoa(int value, char* str, int radix)
{
    if (radix == 10) {
        sprintf(str, "%d", value);
    } else if (radix == 16) {
        sprintf(str, "%x", value);
    } else if (radix == 8) {
        sprintf(str, "%o", value);
    } else {
        /* Generic radix conversion. */
        char* p = str;
        char tmp[36];
        int i = 0;
        unsigned int u = (value < 0 && radix == 10) ? (unsigned int)-value : (unsigned int)value;
        if (value == 0) {
            tmp[i++] = '0';
        }
        while (u != 0 && i < (int)sizeof(tmp)) {
            int digit = u % radix;
            tmp[i++] = (digit < 10) ? (char)('0' + digit) : (char)('a' + digit - 10);
            u /= radix;
        }
        if (value < 0 && radix == 10) {
            *p++ = '-';
        }
        while (i > 0) {
            *p++ = tmp[--i];
        }
        *p = '\0';
    }
    return str;
}

char* SDL_strdup(const char* str)
{
    size_t len = strlen(str) + 1;
    char* copy = (char*)malloc(len);
    if (copy != NULL) {
        memcpy(copy, str, len);
    }
    return copy;
}

void SDL_free(void* mem)
{
    free(mem);
}

/* ---------------------------------------------------------------------- */
/* Logging / dialogs                                                       */
/* ---------------------------------------------------------------------- */

void SDL_LogMessageV(int category, int priority, const char* fmt, va_list ap)
{
    (void)category;
    (void)priority;

    vprintf(fmt, ap);
    putchar('\n');
}

void SDL_ShowSimpleMessageBox(Uint32 flags, const char* title, const char* message, SDL_Window* window)
{
    (void)flags;
    (void)window;

    printf("[messagebox] %s: %s\n", title != NULL ? title : "", message != NULL ? message : "");
}

/* ---------------------------------------------------------------------- */
/* Audio (F1 stubs; audsrv streaming implementation lands in F5)           */
/* ---------------------------------------------------------------------- */

SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired, SDL_AudioSpec* obtained, int allowed_changes)
{
    (void)device;
    (void)iscapture;
    (void)allowed_changes;

    if (desired == NULL) {
        return 0;
    }

    if (obtained != NULL) {
        *obtained = *desired;
    }

    return 1;
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev)
{
    (void)dev;
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on)
{
    (void)dev;
    (void)pause_on;
}

SDL_AudioStream* SDL_NewAudioStream(Uint16 src_format, Uint8 src_channels, int src_rate, Uint16 dst_format, Uint8 dst_channels, int dst_rate)
{
    SDL_AudioStream* stream = (SDL_AudioStream*)calloc(1, sizeof(SDL_AudioStream));
    if (stream != NULL) {
        stream->src_format = src_format;
        stream->src_channels = src_channels;
        stream->src_rate = src_rate;
        stream->dst_format = dst_format;
        stream->dst_channels = dst_channels;
        stream->dst_rate = dst_rate;
    }
    return stream;
}

int SDL_AudioStreamPut(SDL_AudioStream* stream, const void* buf, int len)
{
    (void)stream;
    (void)buf;
    (void)len;
    return 0;
}

int SDL_AudioStreamGet(SDL_AudioStream* stream, void* buf, int len)
{
    (void)stream;
    (void)buf;
    (void)len;
    return 0;
}

void SDL_FreeAudioStream(SDL_AudioStream* stream)
{
    free(stream);
}

void SDL_MixAudioFormat(Uint8* dst, const Uint8* src, SDL_AudioFormat format, Uint32 len, int volume)
{
    if (volume == 0 || dst == NULL || src == NULL) {
        return;
    }

    if (volume > SDL_MIX_MAXVOLUME) {
        volume = SDL_MIX_MAXVOLUME;
    }

    if (format == AUDIO_S16SYS) {
        Sint16* d = (Sint16*)dst;
        const Sint16* s = (const Sint16*)src;
        Uint32 count = len / 2;
        for (Uint32 i = 0; i < count; i++) {
            Sint32 mixed = (Sint32)d[i] + (Sint32)(((Sint32)s[i] * volume) / SDL_MIX_MAXVOLUME);
            if (mixed > 32767) {
                mixed = 32767;
            } else if (mixed < -32768) {
                mixed = -32768;
            }
            d[i] = (Sint16)mixed;
        }
    } else if (format == AUDIO_U8) {
        Uint8* d = dst;
        const Uint8* s = src;
        for (Uint32 i = 0; i < len; i++) {
            Sint32 mixed = (Sint32)d[i] + (((Sint32)s[i] - 128) * volume) / SDL_MIX_MAXVOLUME;
            if (mixed > 255) {
                mixed = 255;
            } else if (mixed < 0) {
                mixed = 0;
            }
            d[i] = (Uint8)mixed;
        }
    } else {
        /* Unknown format: straight copy without mixing. */
        memcpy(dst, src, len);
    }
}
