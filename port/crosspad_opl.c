//
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2026 CrossPad
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//     The OPL driver of the CrossPad port: Chocolate Doom 2.1.0's
//     opl_sdl.c with the SDL taken out. DBOPL is rendered by the app's
//     audio node on the audio thread (cpdoom_music_render), which also
//     advances the OPL clock and runs the music player's callbacks; the
//     mutexes are the app's (crosspad_doom_host.h).
//

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "crosspad_doom_host.h"

#include "dbopl.h"
#include "opl.h"
#include "opl_internal.h"
#include "opl_queue.h"

typedef struct
{
    unsigned int rate;        // Number of times the timer is advanced per sec.
    unsigned int enabled;     // Non-zero if timer is enabled.
    unsigned int value;       // Last value that was set.
    uint64_t expire_time;     // Calculated time that timer will expire.
} opl_timer_t;

// When the callback mutex is locked using OPL_Lock, callback functions
// are not invoked.
static void *callback_mutex = NULL;

// Queue of callbacks waiting to be invoked.
static opl_callback_queue_t *callback_queue;

// Mutex used to control access to the callback queue.
static void *callback_queue_mutex = NULL;

// Current time, in us since startup:
static uint64_t current_time;

// If non-zero, playback is currently paused.
static int opl_paused;

// Time offset (in us) due to the fact that callbacks
// were previously paused.
static uint64_t pause_offset;

// OPL software emulator structure.
static Chip opl_chip;

// Temporary mixing buffer used by the mixing callback.
static int32_t *mix_buffer = NULL;

// Register number that was written.
static int register_num = 0;

// Timers; DBOPL does not do timer stuff itself.
static opl_timer_t timer1 = { 12500, 0, 0, 0 };
static opl_timer_t timer2 = { 3125, 0, 0, 0 };

static unsigned int mixing_freq;

// Set once the chip, the queue and the buffer exist: the audio thread
// renders only then.
static atomic_int opl_ready;

/// DBOPL's output is in int16 range and quiet beside the sound effects
/// (a few percent of full scale); this brings it up to them.
#define MUSIC_GAIN (2.0f / 32768.0f)

// Advance time by the specified number of samples, invoking any
// callback functions as appropriate.

static void AdvanceTime(unsigned int nsamples)
{
    opl_callback_t callback;
    void *callback_data;
    uint64_t us;

    cpdoom_mutex_lock(callback_queue_mutex);

    // Advance time.

    us = ((uint64_t) nsamples * OPL_SECOND) / mixing_freq;
    current_time += us;

    if (opl_paused)
    {
        pause_offset += us;
    }

    // Are there callbacks to invoke now?  Keep invoking them
    // until there are no more left.

    while (!OPL_Queue_IsEmpty(callback_queue)
        && current_time >= OPL_Queue_Peek(callback_queue) + pause_offset)
    {
        // Pop the callback from the queue to invoke it.

        if (!OPL_Queue_Pop(callback_queue, &callback, &callback_data))
        {
            break;
        }

        // The callback runs holding callback_mutex (OPL_Lock() keeps it
        // out) but not callback_queue_mutex: it may schedule the next one.

        cpdoom_mutex_unlock(callback_queue_mutex);
        cpdoom_mutex_lock(callback_mutex);
        callback(callback_data);
        cpdoom_mutex_unlock(callback_mutex);
        cpdoom_mutex_lock(callback_queue_mutex);
    }

    cpdoom_mutex_unlock(callback_queue_mutex);
}

// Call the OPL emulator code to add the specified number of frames.

static void FillBuffer(float *out, unsigned int nsamples)
{
    unsigned int i;

    Chip__GenerateBlock2(&opl_chip, nsamples, mix_buffer);

    for (i = 0; i < nsamples; ++i)
    {
        const float v = (float) mix_buffer[i] * MUSIC_GAIN;

        out[i * 2] += v;
        out[i * 2 + 1] += v;
    }
}

void cpdoom_music_render(float *out, uint32_t frames)
{
    unsigned int filled = 0;

    if (!atomic_load_explicit(&opl_ready, memory_order_acquire))
    {
        return;
    }

    // Repeatedly call the OPL emulator update function until the buffer is
    // full, stopping at every callback that is due on the way.

    while (filled < frames)
    {
        uint64_t next_callback_time;
        uint64_t nsamples;

        cpdoom_mutex_lock(callback_queue_mutex);

        if (opl_paused || OPL_Queue_IsEmpty(callback_queue))
        {
            nsamples = frames - filled;
        }
        else
        {
            next_callback_time = OPL_Queue_Peek(callback_queue) + pause_offset;

            nsamples = (next_callback_time - current_time) * mixing_freq;
            nsamples = (nsamples + OPL_SECOND - 1) / OPL_SECOND;

            if (nsamples > frames - filled)
            {
                nsamples = frames - filled;
            }
        }

        cpdoom_mutex_unlock(callback_queue_mutex);

        FillBuffer(out + filled * 2, (unsigned int) nsamples);
        filled += (unsigned int) nsamples;

        AdvanceTime((unsigned int) nsamples);
    }
}

static void OPL_CrossPad_Shutdown(void)
{
    atomic_store_explicit(&opl_ready, 0, memory_order_release);
    // The queue, the buffer and the mutexes stay: the audio thread may be
    // inside a render now, and DOOM leaves its firmware rather than this.
}

static int OPL_CrossPad_Init(unsigned int port_base)
{
    (void) port_base;

    if (atomic_load_explicit(&opl_ready, memory_order_acquire))
    {
        return 1;
    }

    mixing_freq = cpdoom_audio_rate();

    if (mixing_freq == 0)
    {
        return 0;
    }

    opl_paused = 0;
    pause_offset = 0;
    current_time = 0;

    if (callback_queue == NULL)
    {
        callback_queue = OPL_Queue_Create();
        callback_mutex = cpdoom_mutex_create();
        callback_queue_mutex = cpdoom_mutex_create();
        // One second: longer than any block the audio thread renders.
        mix_buffer = malloc(mixing_freq * sizeof(int32_t));
    }

    if (mix_buffer == NULL || callback_mutex == NULL || callback_queue_mutex == NULL)
    {
        return 0;
    }

    DBOPL_InitTables();
    Chip__Chip(&opl_chip);
    Chip__Setup(&opl_chip, mixing_freq);

    atomic_store_explicit(&opl_ready, 1, memory_order_release);

    return 1;
}

static unsigned int OPL_CrossPad_PortRead(opl_port_t port)
{
    unsigned int result = 0;

    (void) port;

    if (timer1.enabled && current_time > timer1.expire_time)
    {
        result |= 0x80;   // Either have expired
        result |= 0x40;   // Timer 1 has expired
    }

    if (timer2.enabled && current_time > timer2.expire_time)
    {
        result |= 0x80;   // Either have expired
        result |= 0x20;   // Timer 2 has expired
    }

    return result;
}

static void OPLTimer_CalculateEndTime(opl_timer_t *timer)
{
    int tics;

    // If the timer is enabled, calculate the time when the timer
    // will expire.

    if (timer->enabled)
    {
        tics = 0x100 - timer->value;
        timer->expire_time = current_time
                           + ((uint64_t) tics * OPL_SECOND) / timer->rate;
    }
}

static void WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
        case OPL_REG_TIMER1:
            timer1.value = value;
            OPLTimer_CalculateEndTime(&timer1);
            break;

        case OPL_REG_TIMER2:
            timer2.value = value;
            OPLTimer_CalculateEndTime(&timer2);
            break;

        case OPL_REG_TIMER_CTRL:
            if (value & 0x80)
            {
                timer1.enabled = 0;
                timer2.enabled = 0;
            }
            else
            {
                if ((value & 0x40) == 0)
                {
                    timer1.enabled = (value & 0x01) != 0;
                    OPLTimer_CalculateEndTime(&timer1);
                }

                if ((value & 0x20) == 0)
                {
                    timer2.enabled = (value & 0x02) != 0;
                    OPLTimer_CalculateEndTime(&timer2);
                }
            }

            break;

        default:
            Chip__WriteReg(&opl_chip, reg_num, value);
            break;
    }
}

static void OPL_CrossPad_PortWrite(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT)
    {
        register_num = value;
    }
    else if (port == OPL_DATA_PORT)
    {
        WriteRegister(register_num, value);
    }
}

static void OPL_CrossPad_SetCallback(uint64_t us, opl_callback_t callback,
                                     void *data)
{
    cpdoom_mutex_lock(callback_queue_mutex);
    OPL_Queue_Push(callback_queue, callback, data,
                   current_time - pause_offset + us);
    cpdoom_mutex_unlock(callback_queue_mutex);
}

static void OPL_CrossPad_ClearCallbacks(void)
{
    cpdoom_mutex_lock(callback_queue_mutex);
    OPL_Queue_Clear(callback_queue);
    cpdoom_mutex_unlock(callback_queue_mutex);
}

static void OPL_CrossPad_Lock(void)
{
    cpdoom_mutex_lock(callback_mutex);
}

static void OPL_CrossPad_Unlock(void)
{
    cpdoom_mutex_unlock(callback_mutex);
}

static void OPL_CrossPad_SetPaused(int paused)
{
    opl_paused = paused;
}

static void OPL_CrossPad_AdjustCallbacks(float factor)
{
    cpdoom_mutex_lock(callback_queue_mutex);
    OPL_Queue_AdjustCallbacks(callback_queue, current_time, factor);
    cpdoom_mutex_unlock(callback_queue_mutex);
}

opl_driver_t opl_crosspad_driver =
{
    "CrossPad",
    OPL_CrossPad_Init,
    OPL_CrossPad_Shutdown,
    OPL_CrossPad_PortRead,
    OPL_CrossPad_PortWrite,
    OPL_CrossPad_SetCallback,
    OPL_CrossPad_ClearCallbacks,
    OPL_CrossPad_Lock,
    OPL_CrossPad_Unlock,
    OPL_CrossPad_SetPaused,
    OPL_CrossPad_AdjustCallbacks,
};
