/*
 * Sega Chihiro Master Billboard / Secondary Window Display
 *
 * Implements the auxiliary spectator/attract display for Sega Chihiro
 * arcade cabinets (such as Mobile Suit Gundam: 0079 Card Builder and
 * Mobile Suit Gundam: Battle Operating Simulator).
 * Provides multi-monitor second-window display with stitched panoramic
 * (16:9) video playback, synchronized audio, and authentic arcade
 * event progression (Attract -> Coin/Card Briefing -> Sortie -> Combat HUD -> Game Over).
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
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>
#define closesocket close
#endif

#define BILLBOARD_WIDTH 832
#define BILLBOARD_HEIGHT 480
#define BILLBOARD_TITLE "xemu - Chihiro Master Billboard Display"

static SDL_Window *s_billboard_window = NULL;
static SDL_Renderer *s_billboard_renderer = NULL;
static SDL_Texture *s_billboard_texture = NULL;

static bool s_is_active = false;
static bool s_is_fullscreen = false;
static bool s_billboard_init_failed = false;

static uint32_t *s_frame_buffer = NULL;
static QemuMutex s_frame_mutex;
static bool s_frame_ready = false;
static bool s_mutex_inited = false;

/* Video and audio streaming thread */
static QemuThread s_stream_thread;
static bool s_stream_running = false;
static bool s_stream_stop = false;

/* Background UDP listener thread */
static QemuThread s_udp_thread;
static bool s_udp_running = false;

/* Arcade event state */
static int s_active_event_id = 2; /* 2 = E02 (Attract) */
static int s_requested_event_id = 2;
static bool s_event_changed = false;
static QemuMutex s_event_mutex;
static bool s_event_mutex_inited = false;

#ifdef _WIN32
static PROCESS_INFORMATION s_audio_proc;
static bool s_audio_running = false;

static void stop_audio_playback(void)
{
    if (s_audio_running && s_audio_proc.hProcess) {
        TerminateProcess(s_audio_proc.hProcess, 0);
        CloseHandle(s_audio_proc.hProcess);
        CloseHandle(s_audio_proc.hThread);
        s_audio_running = false;
        memset(&s_audio_proc, 0, sizeof(s_audio_proc));
    }
}

static void start_audio_playback(const char *path)
{
    stop_audio_playback();

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffplay -nodisp -autoexit -loglevel quiet \"%s\"", path);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    memset(&s_audio_proc, 0, sizeof(s_audio_proc));
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                       NULL, &si, &s_audio_proc)) {
        s_audio_running = true;
    }
}
#else
static pid_t s_audio_pid = -1;

static void stop_audio_playback(void)
{
    if (s_audio_pid > 0) {
        kill(s_audio_pid, SIGTERM);
        waitpid(s_audio_pid, NULL, WNOHANG);
        s_audio_pid = -1;
    }
}

static void start_audio_playback(const char *path)
{
    stop_audio_playback();
    s_audio_pid = fork();
    if (s_audio_pid == 0) {
        execlp("ffplay", "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet",
               path, (char *)NULL);
        _exit(1);
    }
}
#endif

static void render_standby_pattern(uint32_t *buf, int w, int h)
{
    /* Calculate animated sweep angle based on real time (rotates every 3 seconds) */
    int64_t now_us = g_get_real_time();
    float sweep_angle =
        fmodf((float)(now_us % 3000000) / 3000000.0f * 6.2831853f, 6.2831853f);
    int cx = w / 2;
    int cy = h / 2;

    for (int y = 0; y < h; y++) {
        int dy = y - cy;
        for (int x = 0; x < w; x++) {
            int dx = x - cx;
            int dist2 = dx * dx + dy * dy;

            /* Base dark navy slate background */
            uint8_t r = 10, g = 15, b = 24;

            /* Subtle grid lines */
            if ((x % 32 == 0) || (y % 32 == 0)) {
                r += 8;
                g += 12;
                b += 18;
            }

            /* Concentric radar range rings (r = 60, 120, 180, 220) */
            int dist = (int)sqrtf((float)dist2);
            if (abs(dist - 60) <= 1 || abs(dist - 120) <= 1 ||
                abs(dist - 180) <= 1 || abs(dist - 220) <= 1) {
                r = 20;
                g = 70;
                b = 140;
            }

            /* Center crosshairs */
            if ((abs(dx) <= 1 && abs(dy) < 225) ||
                (abs(dy) <= 1 && abs(dx) < 225)) {
                r = 25;
                g = 80;
                b = 160;
            }

            /* Rotating radar sweep beam (within 220px radius) */
            if (dist < 220 && dist > 2) {
                float angle = atan2f((float)dy, (float)dx);
                if (angle < 0.0f) {
                    angle += 6.2831853f;
                }
                float diff = sweep_angle - angle;
                if (diff < 0.0f) {
                    diff += 6.2831853f;
                }
                if (diff < 0.35f) {
                    float fade = 1.0f - (diff / 0.35f);
                    g = (uint8_t)fminf(255.0f, g + 130.0f * fade);
                    b = (uint8_t)fminf(255.0f, b + 180.0f * fade);
                }
            }

            /* Top and bottom telemetry border strips */
            if (y < 26 || y > h - 27) {
                r = 16;
                g = 24;
                b = 40;
            }
            if (y == 26 || y == h - 27) {
                r = 40;
                g = 130;
                b = 230;
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

    /* 2. Check local movies/ folder */
    snprintf(out_path, max_len, "movies/%s", filename);
    if (g_file_test(out_path, G_FILE_TEST_EXISTS)) {
        return true;
    }

    return false;
}

static void *billboard_udp_worker(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return NULL;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(30004);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(sock);
        return NULL;
    }

#ifdef _WIN32
    DWORD timeout = 500;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
               sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#endif

    char buf[256];
    while (!qatomic_read(&s_stream_stop)) {
        int len = recv(sock, buf, sizeof(buf) - 1, 0);
        if (len > 0) {
            buf[len] = '\0';
            int ev = 0;
            if (sscanf(buf, "E%d", &ev) == 1 && ev >= 2 && ev <= 17) {
                xemu_billboard_play_event(ev);
            } else if (strstr(buf, "ATTRACT")) {
                xemu_billboard_play_event(2);
            } else if (strstr(buf, "COIN") || strstr(buf, "BRIEFING")) {
                xemu_billboard_play_event(3);
            } else if (strstr(buf, "BOOT")) {
                xemu_billboard_play_event(4);
            } else if (strstr(buf, "SORTIE") || strstr(buf, "LAUNCH")) {
                xemu_billboard_play_event(5);
            } else if (strstr(buf, "ALERT")) {
                xemu_billboard_play_event(7);
            } else if (strstr(buf, "CLEAR")) {
                xemu_billboard_play_event(16);
            } else if (strstr(buf, "GAMEOVER")) {
                xemu_billboard_play_event(17);
            } else if (strstr(buf, "STANDBY")) {
                xemu_billboard_play_event(0);
            }
        }
    }

    closesocket(sock);
    return NULL;
}

static void *billboard_stream_worker(void *arg)
{
    size_t frame_bytes = BILLBOARD_WIDTH * BILLBOARD_HEIGHT * 4;
    uint8_t *pipe_buf = g_malloc0(frame_bytes);

    while (!qatomic_read(&s_stream_stop)) {
        if (!s_event_mutex_inited) {
            qemu_mutex_init(&s_event_mutex);
            s_event_mutex_inited = true;
        }

        qemu_mutex_lock(&s_event_mutex);
        int event = s_requested_event_id;
        s_active_event_id = event;
        s_event_changed = false;
        qemu_mutex_unlock(&s_event_mutex);

        /* Event 0 = Standby / Radar HUD mode (no video stream, render standby HUD) */
        if (event == 0) {
            stop_audio_playback();
            qemu_mutex_lock(&s_frame_mutex);
            if (s_frame_buffer) {
                render_standby_pattern(s_frame_buffer, BILLBOARD_WIDTH,
                                       BILLBOARD_HEIGHT);
                s_frame_ready = true;
            }
            qemu_mutex_unlock(&s_frame_mutex);

            /* Animate radar pattern smoothly at ~30 fps while checking for event changes */
            for (int i = 0; i < 30 && !qatomic_read(&s_stream_stop) &&
                            !s_event_changed;
                 i++) {
                g_usleep(33000); /* ~33ms */
                qemu_mutex_lock(&s_frame_mutex);
                if (s_frame_buffer) {
                    render_standby_pattern(s_frame_buffer, BILLBOARD_WIDTH,
                                           BILLBOARD_HEIGHT);
                    s_frame_ready = true;
                }
                qemu_mutex_unlock(&s_frame_mutex);
            }
            continue;
        }

        char movie_path[1024];
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

        /* Start synchronized audio playback */
        start_audio_playback(movie_path);

        bool natural_finish = true;
        while (!qatomic_read(&s_stream_stop)) {
            if (s_event_changed) {
                natural_finish = false;
                break; /* Immediate cutscene transition on event trigger */
            }

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

        stop_audio_playback();
        pclose(p);

        if (qatomic_read(&s_stream_stop)) {
            break;
        }

        /* Authentic Arcade Sequence Progression when cutscene finishes naturally */
        if (natural_finish) {
            qemu_mutex_lock(&s_event_mutex);
            if (!s_event_changed) {
                switch (event) {
                case 2:
                    /* Attract mode: Loop E02.mp4 continuously */
                    s_requested_event_id = 2;
                    break;
                case 3:
                    /* Coin/Card Insertion -> advance to E04 (OS Boot sequence) */
                    s_requested_event_id = 4;
                    break;
                case 4:
                    /* OS Boot -> advance to E05 (Sortie / Catapult Launch) */
                    s_requested_event_id = 5;
                    break;
                case 5:
                    /* Sortie launch finished -> cockpit handover / tactical radar HUD */
                    s_requested_event_id = 0;
                    break;
                case 6:
                case 7:
                case 8:
                case 9:
                case 10:
                case 11:
                case 12:
                case 13:
                case 14:
                case 15:
                    /* Combat dynamic alerts finished -> return to tactical radar HUD */
                    s_requested_event_id = 0;
                    break;
                case 16:
                    /* Mission Complete -> advance to E17 (Rank Assessment) */
                    s_requested_event_id = 17;
                    break;
                case 17:
                    /* Game Over / Rank Assessment finished -> return to Attract (E02) */
                    s_requested_event_id = 2;
                    break;
                default:
                    s_requested_event_id = 2;
                    break;
                }
            }
            qemu_mutex_unlock(&s_event_mutex);
        }
    }

    g_free(pipe_buf);
    return NULL;
}

void xemu_billboard_init(void)
{
    if (s_billboard_window || s_billboard_init_failed) {
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

    if (!s_event_mutex_inited) {
        qemu_mutex_init(&s_event_mutex);
        s_event_mutex_inited = true;
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
        s_billboard_init_failed = true;
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
        s_billboard_init_failed = true;
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

    /* Start background UDP event listener */
    qemu_thread_create(&s_udp_thread, "billboard-udp", billboard_udp_worker,
                       NULL, QEMU_THREAD_JOINABLE);
    s_udp_running = true;

    fprintf(stderr,
            "Chihiro Billboard Display initialized successfully (%dx%d).\n",
            BILLBOARD_WIDTH, BILLBOARD_HEIGHT);
}

void xemu_billboard_cleanup(void)
{
    qatomic_set(&s_stream_stop, true);
    stop_audio_playback();

    if (s_udp_running) {
        qemu_thread_join(&s_udp_thread);
        s_udp_running = false;
    }

    if (s_stream_running) {
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

    if (s_event_mutex_inited) {
        qemu_mutex_destroy(&s_event_mutex);
        s_event_mutex_inited = false;
    }

    s_is_active = false;
    s_billboard_init_failed = false;
}

void xemu_billboard_update(void)
{
    if (s_billboard_init_failed) {
        return;
    }

    if (!s_billboard_window) {
        if (g_config.chihiro.billboard.enable ||
            chihiro_cabinet_has_billboard()) {
            xemu_billboard_init();
        }
        return;
    }

    if (!s_is_active || !s_billboard_renderer) {
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
        switch (ev->key.scancode) {
        case SDL_SCANCODE_F1:
            xemu_billboard_play_event(2); /* Attract Mode (E02) */
            return true;
        case SDL_SCANCODE_F2:
            xemu_billboard_play_event(3); /* Coin / Briefing (E03) */
            return true;
        case SDL_SCANCODE_F3:
            xemu_billboard_play_event(4); /* OS Diagnostics Boot (E04) */
            return true;
        case SDL_SCANCODE_F4:
            xemu_billboard_play_event(5); /* Sortie Catapult Launch (E05) */
            return true;
        case SDL_SCANCODE_F5:
            xemu_billboard_play_event(7); /* Combat Ace Alert (E07) */
            return true;
        case SDL_SCANCODE_F6:
            xemu_billboard_play_event(16); /* Mission Clear / Evac (E16) */
            return true;
        case SDL_SCANCODE_F7:
            xemu_billboard_play_event(17); /* Game Over / Rank (E17) */
            return true;
        case SDL_SCANCODE_F8:
        case SDL_SCANCODE_SPACE:
            xemu_billboard_play_event(0); /* Tactical Spectator Radar HUD */
            return true;
        case SDL_SCANCODE_F11:
            s_is_fullscreen = !s_is_fullscreen;
            SDL_SetWindowFullscreen(s_billboard_window, s_is_fullscreen);
            return true;
        default:
            return true;
        }
    }

    return false;
}

void xemu_billboard_play_event(int event_id)
{
    if (event_id < 0 || event_id > 17) {
        return;
    }

    if (!s_event_mutex_inited) {
        qemu_mutex_init(&s_event_mutex);
        s_event_mutex_inited = true;
    }

    qemu_mutex_lock(&s_event_mutex);
    s_requested_event_id = event_id;
    s_event_changed = true;
    qemu_mutex_unlock(&s_event_mutex);
}

void xemu_billboard_on_coin_inserted(void)
{
    /* If currently in Attract mode (E02) or standby radar, start briefing (E03) */
    if (s_active_event_id == 2 || s_active_event_id == 0) {
        xemu_billboard_play_event(3);
    }
}

void xemu_billboard_on_card_inserted(void)
{
    /* If currently in Attract mode (E02) or standby radar, start briefing (E03) */
    if (s_active_event_id == 2 || s_active_event_id == 0) {
        xemu_billboard_play_event(3);
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
