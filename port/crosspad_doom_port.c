// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file crosspad_doom_port.c
 * @brief doomgeneric's platform functions and its sound module, on the
 *        app's host calls (crosspad_doom_host.h). C, built with doomgeneric:
 *        its `boolean` and structs stay on this side.
 */

#include <string.h>

#include "crosspad_doom_host.h"

#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "doomtype.h"
#include "i_sound.h"
#include "i_video.h"
#include "m_controls.h"
#include "m_misc.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"

/* ── doomgeneric ─────────────────────────────────────────────────────── */

void DG_Init(void)
{
    /* Confirming is the encoder click; its prompts say so (d_englsh.h). */
    key_menu_confirm = KEY_ENTER;
    /* The encoder turns the weapons in the game (crosspad_doom_host.h). */
    key_prevweapon = CPDOOM_KEY_PREV_WEAPON;
    key_nextweapon = CPDOOM_KEY_NEXT_WEAPON;
}

void DG_DrawFrame(void)
{
    static uint8_t palette[256 * 3];
    for (int i = 0; i < 256; ++i) {
        palette[i * 3 + 0] = colors[i].r;
        palette[i * 3 + 1] = colors[i].g;
        palette[i * 3 + 2] = colors[i].b;
    }
    cpdoom_menu_active(menuactive ? 1 : 0);
    cpdoom_frame((const uint8_t*)DG_ScreenBuffer, palette);
}

void DG_SleepMs(uint32_t ms) { cpdoom_sleep_ms(ms); }

uint32_t DG_GetTicksMs(void) { return cpdoom_ticks_ms(); }

int DG_GetKey(int* pressed, unsigned char* key) { return cpdoom_next_key(pressed, key); }

void DG_SetWindowTitle(const char* title) { (void)title; }

void DG_Quit(void) { cpdoom_quit(); }

void DG_Fatal(const char* message) { cpdoom_fatal(message); }

const char* DG_DataDir(void) { return cpdoom_data_dir(); }

/* ── Sound effects ───────────────────────────────────────────────────── */

/* m_config.c binds these under FEATURE_SOUND; there is no libsamplerate. */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

/* A DMX sound lump: format 3, rate, length, then the samples between 16
 * bytes of padding at each end. */
enum { kDmxHeader = 8, kDmxPad = 16 };

static boolean s_sfxPrefix;

static boolean Sfx_Init(boolean use_sfx_prefix)
{
    s_sfxPrefix = use_sfx_prefix;
    return true;
}

static void Sfx_Shutdown(void) {}

static int Sfx_GetLumpNum(sfxinfo_t* sfx)
{
    char name[9];
    if (sfx->link != NULL) sfx = sfx->link;
    if (s_sfxPrefix) M_snprintf(name, sizeof(name), "ds%s", sfx->name);
    else M_StringCopy(name, sfx->name, sizeof(name));
    return W_GetNumForName(name);
}

static void Sfx_Update(void) {}

static void Sfx_UpdateParams(int channel, int vol, int sep) { cpdoom_sfx_params(channel, vol, sep); }

static int Sfx_Start(sfxinfo_t* sfx, int channel, int vol, int sep)
{
    if (channel < 0 || channel >= CPDOOM_SFX_CHANNELS) return -1;
    if (sfx->lumpnum < 0) sfx->lumpnum = Sfx_GetLumpNum(sfx);
    const int lump = sfx->lumpnum;
    const int lumpLen = W_LumpLength(lump);
    /* PU_STATIC: the lump is never purged, so the samples outlive the call. */
    const uint8_t* data = W_CacheLumpNum(lump, PU_STATIC);
    if (lumpLen < kDmxHeader || data[0] != 0x03 || data[1] != 0x00) return -1;
    const uint32_t rate = (uint32_t)data[2] | ((uint32_t)data[3] << 8);
    const uint32_t length = (uint32_t)data[4] | ((uint32_t)data[5] << 8) | ((uint32_t)data[6] << 16) |
                            ((uint32_t)data[7] << 24);
    if (length > (uint32_t)lumpLen - kDmxHeader || length <= 2 * kDmxPad || rate == 0) return -1;
    cpdoom_sfx_start(channel, data + kDmxHeader + kDmxPad, length - 2 * kDmxPad, rate, vol, sep);
    return channel;
}

static void Sfx_Stop(int channel) { cpdoom_sfx_stop(channel); }

static boolean Sfx_IsPlaying(int channel) { return cpdoom_sfx_playing(channel) ? true : false; }

static void Sfx_Precache(sfxinfo_t* sounds, int num_sounds)
{
    (void)sounds;
    (void)num_sounds;
}

static snddevice_t s_sfxDevices[] = {
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
    s_sfxDevices,       arrlen(s_sfxDevices), Sfx_Init, Sfx_Shutdown, Sfx_GetLumpNum, Sfx_Update,
    Sfx_UpdateParams,   Sfx_Start,            Sfx_Stop, Sfx_IsPlaying, Sfx_Precache,
};

/* Music: Chocolate Doom's OPL player (third_party/doomgeneric/i_oplmusic.c,
 * DG_music_module) on the OPL driver of port/crosspad_opl.c. */
