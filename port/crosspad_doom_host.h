// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file crosspad_doom_host.h
 * @brief What DOOM's C side (crosspad_doom_port.c, compiled with doomgeneric)
 *        asks of the app (DoomApp.cpp). Plain C types only: doomgeneric's
 *        `boolean` never crosses this line.
 *
 * Every call comes from the DOOM task, but for the music's: the audio thread
 * renders it (cpdoom_music_render, through the app's node) and runs the OPL
 * player's callbacks there, so its mutexes are taken from both.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The frame's width and height: doomgeneric's DOOMGENERIC_RESX/RESY.
#define CPDOOM_W 320
#define CPDOOM_H 200

/// The keys the encoder sends for the weapons in the game; nothing else in
/// DOOM uses them.
#define CPDOOM_KEY_PREV_WEAPON '['
#define CPDOOM_KEY_NEXT_WEAPON ']'

uint32_t cpdoom_ticks_ms(void);
void cpdoom_sleep_ms(uint32_t ms);

/// The next key event: 1 with *pressed (1 down, 0 up) and *key a DOOM key
/// code (doomkeys.h), 0 when there is none.
int cpdoom_next_key(int* pressed, unsigned char* key);

/// A finished frame: CPDOOM_W x CPDOOM_H palette indices and the 256-entry
/// palette as r, g, b bytes.
void cpdoom_frame(const uint8_t* indices, const uint8_t* palette_rgb);

/// Whether DOOM's menu is up: the encoder walks it then, and turns the
/// weapons otherwise.
void cpdoom_menu_active(int active);

/// "<card>/crosspad/doom/" -- the WAD, the configuration and the saves.
const char* cpdoom_data_dir(void);

/// The quit confirmed in DOOM's menu. Returns only when leaving was refused.
void cpdoom_quit(void);
/// I_Error. Never returns.
void cpdoom_fatal(const char* message);

/* ── Sound effects: 8-bit unsigned mono PCM at `rate` ─────────────────── */

#define CPDOOM_SFX_CHANNELS 16

/// vol 0..127; sep 0 (left) .. 254 (right). The PCM stays valid: the port
/// caches the lump for good.
void cpdoom_sfx_start(int channel, const uint8_t* pcm, uint32_t length, uint32_t rate, int vol, int sep);
void cpdoom_sfx_params(int channel, int vol, int sep);
void cpdoom_sfx_stop(int channel);
int cpdoom_sfx_playing(int channel);

/* ── Music: an OPL2 emulated on the audio thread (crosspad_opl.c) ─────── */

/// The audio module's sample rate; 0 while there is none (no music then).
uint32_t cpdoom_audio_rate(void);

/// A mutex the DOOM task and the audio thread can both take.
void* cpdoom_mutex_create(void);
void cpdoom_mutex_lock(void* mutex);
void cpdoom_mutex_unlock(void* mutex);

/// Audio thread: adds the music to @p out (interleaved stereo); the OPL
/// player's callbacks run in here. Implemented by crosspad_opl.c.
void cpdoom_music_render(float* out, uint32_t frames);

#ifdef __cplusplus
}
#endif
