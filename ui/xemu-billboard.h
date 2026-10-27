/*
 * Sega Chihiro Master Billboard / Secondary Window Display
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

#ifndef XEMU_BILLBOARD_H
#define XEMU_BILLBOARD_H

#include <stdbool.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

void xemu_billboard_init(void);
void xemu_billboard_cleanup(void);
void xemu_billboard_update(void);
bool xemu_billboard_handle_event(const SDL_Event *ev);
void xemu_billboard_play_event(int event_id);
void xemu_billboard_on_coin_inserted(void);
void xemu_billboard_on_card_inserted(void);
bool xemu_billboard_is_active(void);
void xemu_billboard_set_active(bool active);

#ifdef __cplusplus
}
#endif

#endif
