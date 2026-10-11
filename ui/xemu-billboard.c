/*
 * Sega Chihiro Master Billboard / Secondary Window Display
 *
 * Implements the auxiliary spectator/attract display for Sega Chihiro
 * arcade cabinets (such as Mobile Suit Gundam: 0079 Card Builder).
 * Provides multi-monitor second-window display with stitched panoramic
 * (16:9) video playback.
 *
 * Copyright (c) 2026 xemu team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "ui/xemu-billboard.h"
#include "ui/xemu-settings.h"
#include "hw/xbox/chihiro/chihiro-cabinet.h"
#include <math.h>
#include <SDL3/SDL.h>
#include <glib.h>

#ifdef _WIN32
#include <windows.h>
#define popen _popen
#define pclose _pclose
#endif

#define BILLBOARD_WIDTH 832
#define BILLBOARD_HEIGHT 480
#define BILLBOARD_TITLE "xemu - Chihiro Master Billboard Display"

static SDL_Window *s_billboard_window = NULL;
static SDL_Renderer *s_billboard_renderer = NULL;
static SDL_Texture *s_billboard_texture = NULL;

static bool s_is_active = false;
static bool s_is_fullscreen = false;

static uint32_t *s_frame_buffer = NULL;
static QemuMutex s_frame_mutex;
static bool s_frame_ready = false;
static bool s_mutex_inited = false;

/* Video streaming thread */
static QemuThread s_stream_thread;
static bool s_stream_running = false;
static bool s_stream_stop = false;
static int s_active_event_id = 2; /* Default attract event (E02..E17) */

static void render_standby_pattern(uint32_t *buf, int w, int h)
{
    /* Render a clean arcade spectator test card / standby graphic */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* Dark slate tech background with subtle grid */
            uint8_t r = 16, g = 20, b = 28;

            /* Grid lines */
            if ((x % 32 == 0) || (y % 32 == 0)) {
                r += 12;
                g += 16;
                b += 24;
            }

            /* Center 16:9 active display frame border */
            if (x == 4 || x == w - 5 || y == 4 || y == h - 5) {
                r = 40;
                g = 120;
                b = 220;
            }

            /* Arcade banner block across the center */
            if (y >= 190 && y <= 290) {
                float intensity = 1.0f - (fabsf((float)y - 240.0f) / 50.0f);
                r = (uint8_t)(20 + 20 * intensity);
                g = (uint8_t)(40 + 60 * intensity);
                b = (uint8_t)(80 + 120 * intensity);

                /* Side markers */
                if (x < 30 || x > w - 31) {
                    r = 255;
                    g = 180;
                    b = 30;
                }
            }

            buf[y * w + x] = (255u << 24) | ((uint32_t)r << 16) |
                             ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

static bool find_movie_file(int event_id, char *out_path, size_t max_len)
{
    char filename[32];
    snprintf(filename, sizeof(filename), "E%02d.mp4", event_id);

    /* 1. Check user configured directory */
    if (g_config.chihiro.billboard.movie_dir &&
        g_config.chihiro.billboard.movie_dir[0]) {
        snprintf(out_path, max_len, "%s/%s",
                 g_config.chihiro.billboard.movie_dir, filename);
        if (g_file_test(out_path, G_FILE_TEST_EXISTS)) {
            return true;
        }
    }

    /* 2. Check Z:/roms/Chihiro-NetBoot/movies */
    snprintf(out_path, max_len, "Z:/roms/Chihiro-NetBoot/movies/%s", filename);
    if (g_file_test(out_path, G_FILE_TEST_EXISTS)) {
        return true;
    }

    /* 3. Check local movies/ folder */
    snprintf(out_path, max_len, "movies/%s", filename);
    if (g_file_test(out_path, G_FILE_TEST_EXISTS)) {
        return true;
    }

    return false;
}

static void *billboard_stream_worker(void *arg)
{
    size_t frame_bytes = BILLBOARD_WIDTH * BILLBOARD_HEIGHT * 4;
    uint8_t *pipe_buf = g_malloc0(frame_bytes);

    while (!qatomic_read(&s_stream_stop)) {
        char movie_path[1024];
        int event = s_active_event_id;

        if (!find_movie_file(event, movie_path, sizeof(movie_path))) {
            /* If movie file is not found, sleep briefly and keep standby
             * pattern */
            g_usleep(250000);
            continue;
        }

        /* Pipe frames directly from ffmpeg decoded at real-time 30 fps */
        char cmd[1280];
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -re -i \"%s\" -f rawvideo -pix_fmt rgba -s %dx%d -an "
                 "-loglevel quiet -",
                 movie_path, BILLBOARD_WIDTH, BILLBOARD_HEIGHT);

        FILE *p = popen(cmd, "rb");
        if (!p) {
            g_usleep(500000);
            continue;
        }

        while (!qatomic_read(&s_stream_stop)) {
            size_t bytes_read = fread(pipe_buf, 1, frame_bytes, p);
            if (bytes_read != frame_bytes) {
                break; /* End of stream */
            }

            qemu_mutex_lock(&s_frame_mutex);
            if (s_frame_buffer) {
                memcpy(s_frame_buffer, pipe_buf, frame_bytes);
                s_frame_ready = true;
            }
            qemu_mutex_unlock(&s_frame_mutex);
        }

        pclose(p);

        /* Loop through attract events (E02 through E17) */
        s_active_event_id++;
        if (s_active_event_id > 17) {
            s_active_event_id = 2;
        }
    }

    g_free(pipe_buf);
    return NULL;
}

void xemu_billboard_init(void)
{
    if (s_billboard_window) {
        return;
    }

    bool should_enable =
        g_config.chihiro.billboard.enable || chihiro_cabinet_has_billboard();
    if (!should_enable) {
        return;
    }

    if (!s_mutex_inited) {
        qemu_mutex_init(&s_frame_mutex);
        s_mutex_inited = true;
    }

    if (!s_frame_buffer) {
        s_frame_buffer =
            g_malloc0(BILLBOARD_WIDTH * BILLBOARD_HEIGHT * sizeof(uint32_t));
        render_standby_pattern(s_frame_buffer, BILLBOARD_WIDTH,
                               BILLBOARD_HEIGHT);
    }

    int width = BILLBOARD_WIDTH;
    int height = BILLBOARD_HEIGHT;
    SDL_WindowFlags window_flags =
        (SDL_WindowFlags)(SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

    s_is_fullscreen = g_config.chihiro.billboard.fullscreen;
    if (s_is_fullscreen) {
        window_flags = (SDL_WindowFlags)(window_flags | SDL_WINDOW_FULLSCREEN);
    }

    s_billboard_window =
        SDL_CreateWindow(BILLBOARD_TITLE, width, height, window_flags);
    if (!s_billboard_window) {
        fprintf(stderr, "Failed to create Billboard window: %s\n",
                SDL_GetError());
        return;
    }

    /* Position on target display if available */
    int num_displays = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&num_displays);
    int target_monitor = g_config.chihiro.billboard.monitor;

    if (displays && num_displays > 1) {
        int idx = (target_monitor >= 0 && target_monitor < num_displays) ?
                      target_monitor :
                      1;
        SDL_Rect bounds;
        if (SDL_GetDisplayBounds(displays[idx], &bounds)) {
            SDL_SetWindowPosition(s_billboard_window,
                                  bounds.x + (bounds.w - width) / 2,
                                  bounds.y + (bounds.h - height) / 2);
        }
    }
    if (displays) {
        SDL_free(displays);
    }

    s_billboard_renderer = SDL_CreateRenderer(s_billboard_window, NULL);
    if (!s_billboard_renderer) {
        fprintf(stderr, "Failed to create Billboard renderer: %s\n",
                SDL_GetError());
        SDL_DestroyWindow(s_billboard_window);
        s_billboard_window = NULL;
        return;
    }

    s_billboard_texture = SDL_CreateTexture(
        s_billboard_renderer, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING, BILLBOARD_WIDTH, BILLBOARD_HEIGHT);

    if (s_billboard_texture && s_frame_buffer) {
        SDL_UpdateTexture(s_billboard_texture, NULL, s_frame_buffer,
                          BILLBOARD_WIDTH * 4);
    }

    s_is_active = true;

    /* Start background playback thread */
    s_stream_stop = false;
    qemu_thread_create(&s_stream_thread, "billboard-stream",
                       billboard_stream_worker, NULL, QEMU_THREAD_JOINABLE);
    s_stream_running = true;

    fprintf(stderr,
            "Chihiro Billboard Display initialized successfully (%dx%d).\n",
            BILLBOARD_WIDTH, BILLBOARD_HEIGHT);
}

void xemu_billboard_cleanup(void)
{
    if (s_stream_running) {
        qatomic_set(&s_stream_stop, true);
        qemu_thread_join(&s_stream_thread);
        s_stream_running = false;
    }

    if (s_billboard_texture) {
        SDL_DestroyTexture(s_billboard_texture);
        s_billboard_texture = NULL;
    }

    if (s_billboard_renderer) {
        SDL_DestroyRenderer(s_billboard_renderer);
        s_billboard_renderer = NULL;
    }

    if (s_billboard_window) {
        SDL_DestroyWindow(s_billboard_window);
        s_billboard_window = NULL;
    }

    if (s_frame_buffer) {
        g_free(s_frame_buffer);
        s_frame_buffer = NULL;
    }

    if (s_mutex_inited) {
        qemu_mutex_destroy(&s_frame_mutex);
        s_mutex_inited = false;
    }

    s_is_active = false;
}

void xemu_billboard_update(void)
{
    if (!s_is_active || !s_billboard_window || !s_billboard_renderer) {
        return;
    }

    if (s_frame_ready && s_billboard_texture) {
        qemu_mutex_lock(&s_frame_mutex);
        if (s_frame_buffer) {
            SDL_UpdateTexture(s_billboard_texture, NULL, s_frame_buffer,
                              BILLBOARD_WIDTH * 4);
        }
        s_frame_ready = false;
        qemu_mutex_unlock(&s_frame_mutex);
    }

    SDL_SetRenderDrawColor(s_billboard_renderer, 10, 14, 20, 255);
    SDL_RenderClear(s_billboard_renderer);

    if (s_billboard_texture) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSizeInPixels(s_billboard_window, &win_w, &win_h);

        if (win_w > 0 && win_h > 0) {
            float target_aspect =
                (float)BILLBOARD_WIDTH / (float)BILLBOARD_HEIGHT;
            float win_aspect = (float)win_w / (float)win_h;
            SDL_FRect dst;

            if (win_aspect > target_aspect) {
                dst.h = (float)win_h;
                dst.w = dst.h * target_aspect;
                dst.x = ((float)win_w - dst.w) * 0.5f;
                dst.y = 0.0f;
            } else {
                dst.w = (float)win_w;
                dst.h = dst.w / target_aspect;
                dst.x = 0.0f;
                dst.y = ((float)win_h - dst.h) * 0.5f;
            }

            SDL_RenderTexture(s_billboard_renderer, s_billboard_texture, NULL,
                              &dst);
        }
    }

    SDL_RenderPresent(s_billboard_renderer);
}

bool xemu_billboard_handle_event(const SDL_Event *ev)
{
    if (!s_billboard_window) {
        return false;
    }

    uint32_t billboard_id = SDL_GetWindowID(s_billboard_window);

    /* Check if event is targeted at the billboard window */
    if (ev->type >= SDL_EVENT_WINDOW_FIRST &&
        ev->type <= SDL_EVENT_WINDOW_LAST) {
        if (ev->window.windowID != billboard_id) {
            return false;
        }

        switch (ev->type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            SDL_HideWindow(s_billboard_window);
            s_is_active = false;
            return true;
        case SDL_EVENT_WINDOW_SHOWN:
            s_is_active = true;
            return true;
        default:
            return true;
        }
    }

    if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.windowID == billboard_id) {
        if (ev->key.scancode == SDL_SCANCODE_F11) {
            s_is_fullscreen = !s_is_fullscreen;
            SDL_SetWindowFullscreen(s_billboard_window, s_is_fullscreen);
            return true;
        }
        return true;
    }

    return false;
}

void xemu_billboard_play_event(int event_id)
{
    if (event_id >= 2 && event_id <= 17) {
        s_active_event_id = event_id;
    }
}

bool xemu_billboard_is_active(void)
{
    return s_is_active;
}

void xemu_billboard_set_active(bool active)
{
    s_is_active = active;
    if (s_billboard_window) {
        if (active) {
            SDL_ShowWindow(s_billboard_window);
        } else {
            SDL_HideWindow(s_billboard_window);
        }
    }
}
