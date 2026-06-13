
#include "mcugdx.h"
#include "doomgeneric/doomgeneric.h"
#include "doomgeneric/w_file.h"
#include "doomgeneric/i_sound.h"
#include "doomgeneric/m_misc.h"
#include "doomgeneric/w_wad.h"
#include "doomgeneric/z_zone.h"
#include "doomgeneric/deh_str.h"
#include "doomgeneric/doomtype.h"
#include "doomgeneric/doomkeys.h"

#include "crosspad_stm32.h"

#define TAG "DOOM"

// CrossPad rev2 LCD: ILI9341, 320x240, SPI. Native panel 240x320 portrait; we
// run LANDSCAPE for 320x240. Pins from pinout_v2.h:
//   DATA0/MOSI=47, PCLK/SCK=14, CS=13, DC=21, RST=18.
// Backlight is NOT a GPIO here — the STM32 co-processor drives it (see app_main).
mcugdx_display_config_t display_config = {
		.driver = MCUGDX_ILI9341,
		.native_width = 240,
		.native_height = 320,
		.mosi = 47,
		.sck = 14,
		.dc = 21,
		.cs = 13,
		.reset = 18};

typedef struct {
	wad_file_t wad;
	mcugdx_file_handle_t handle;
} rofs_wad_file_t;

extern wad_file_class_t rofs_wad_file;

static wad_file_t *doom_rofs_open_file(char *path) {
	if (!mcugdx_rofs.exists(path)) return NULL;

	rofs_wad_file_t *result;
	result = mcugdx_mem_alloc(sizeof(rofs_wad_file_t), MCUGDX_MEM_EXTERNAL);
	result->handle = mcugdx_rofs.open(path);
	result->wad.file_class = &rofs_wad_file;
	result->wad.mapped = NULL;
	result->wad.length = mcugdx_rofs.length(result->handle);
	return &result->wad;
}

static void doom_rofs_close_file(wad_file_t *wad) {
	rofs_wad_file_t *rofs_wad;
	rofs_wad = (rofs_wad_file_t *) wad;
	mcugdx_mem_free(rofs_wad);
}

size_t doom_rofs_read(wad_file_t *wad, unsigned int offset,
					  void *buffer, size_t buffer_len) {
	rofs_wad_file_t *rofs_wad = (rofs_wad_file_t *) wad;
	return mcugdx_rofs.read(rofs_wad->handle, offset, buffer, buffer_len);
}

wad_file_class_t rofs_wad_file = {
		doom_rofs_open_file,
		doom_rofs_close_file,
		doom_rofs_read,
};

boolean doom_init_sound(boolean _use_sfx_prefix) {
	// Audio disabled on CrossPad for now (no I2S init). Returning false makes
	// DOOM treat the sound device as absent, so it never calls start/stop and
	// we never touch the uninitialised mcugdx audio backend.
	(void)_use_sfx_prefix;
	return false;
}

void doom_shut_down_sound(void) {
	// no-op
}

void get_sfx_lump_num(sfxinfo_t *sfx, char *buf, size_t buf_len) {

	if (sfx->link != NULL) {
		sfx = sfx->link;
	}
	M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
}

int doom_get_sfx_lump_num(sfxinfo_t *sfx) {
	char namebuf[9];
	get_sfx_lump_num(sfx, namebuf, sizeof(namebuf));
	return W_GetNumForName(namebuf);
}

// Audio is disabled on CrossPad for the first milestone (doom_init_sound returns
// false). These sound-module entry points are kept so DG_sound_module stays a
// valid table, but they are no-ops — they don't touch the mcugdx audio backend
// (whose mcugdx_sound_t layout changed and which we never initialise here).
void doom_update_sound(void) {
}

void doom_update_sound_params(int handle, int vol, int sep) {
	(void)handle; (void)vol; (void)sep;
}

int doom_start_sound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
	(void)sfxinfo; (void)channel; (void)vol; (void)sep;
	return -1;
}

void doom_stop_sound(int handle) {
	(void)handle;
}

boolean doom_sound_is_playing(int handle) {
	(void)handle;
	return false;
}

void doom_precache_sounds(sfxinfo_t *sounds, int num_sounds) {
	(void)sounds; (void)num_sounds;
}

static snddevice_t doom_sound_devices[] = {
		SNDDEVICE_SB,
		SNDDEVICE_PAS,
		SNDDEVICE_GUS,
		SNDDEVICE_WAVEBLASTER,
		SNDDEVICE_SOUNDCANVAS,
		SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
		doom_sound_devices,
		arrlen(doom_sound_devices),
		doom_init_sound,
		doom_shut_down_sound,
		doom_get_sfx_lump_num,
		doom_update_sound,
		doom_update_sound_params,
		doom_start_sound,
		doom_stop_sound,
		doom_sound_is_playing,
		doom_precache_sounds,
};

void DG_Init() {
	DG_ScreenBuffer = (pixel_t *) (mcugdx_display_frame_buffer() + mcugdx_display_width() * 20);
}

void DG_SetWindowTitle(const char *title) {
}

int frames = 0;
double last_frame_time = -1;
void DG_DrawFrame(void) {
	mcugdx_display_show();

	frames++;
	if (frames > 60 * 5) {
		frames = 0;
		if (last_frame_time != -1) {
			double frame_time = (mcugdx_time() - last_frame_time) * 1000;
			mcugdx_log(TAG, "Frame time: %f ms, %f fps", frame_time, 1000 / frame_time);
		}
	}
	last_frame_time = mcugdx_time();
}

uint32_t DG_GetTicksMs() {
	uint32_t ticks = (uint32_t) (mcugdx_time() * 1000);
	return ticks;
}

void DG_SleepMs(uint32_t ms) {
	mcugdx_sleep(ms);
}

// CrossPad 4x4 pad grid -> DOOM keys. Pad index is row-major, top-left = 0:
//    0  1  2  3        0  = ESC (menu)     3  = FIRE
//    4  5  6  7        1  = forward        11 = USE (open)
//    8  9 10 11        8  = turn left      12 = ENTER (confirm)
//   12 13 14 15        9  = back          10 = turn right
// Unmapped pads are ignored. 0 means "no key".
static const unsigned char pad_to_doomkey[16] = {
		[0]  = KEY_ESCAPE,
		[1]  = KEY_UPARROW,
		[3]  = KEY_FIRE,
		[8]  = KEY_LEFTARROW,
		[9]  = KEY_DOWNARROW,
		[10] = KEY_RIGHTARROW,
		[11] = KEY_USE,
		[12] = KEY_ENTER,
};

static uint16_t g_pads_reported = 0;

// doomgeneric drains this in a loop each tick until it returns 0. We poll the
// STM32 pad bitmap once per drain cycle (when the reported state has caught up),
// then emit one pad transition per call as a press/release event.
int DG_GetKey(int *pressed, unsigned char *doomKey) {
	static uint16_t current = 0;
	if (current == g_pads_reported) {
		current = crosspad_stm32_read_pads();
	}
	uint16_t diff = current ^ g_pads_reported;
	for (int i = 0; i < 16; i++) {
		uint16_t bit = (uint16_t)(1u << i);
		if (!(diff & bit)) continue;
		g_pads_reported ^= bit;             // consume this transition
		unsigned char k = pad_to_doomkey[i];
		if (!k) continue;                   // unmapped pad — swallow silently
		*pressed = (current & bit) ? -1 : 0;
		*doomKey = k;
		return -1;
	}
	return 0;
}
void app_main() {
	mcugdx_init();
	mcugdx_display_init(&display_config);
	mcugdx_display_set_orientation(MCUGDX_LANDSCAPE);
	mcugdx_rofs_init();

	// CrossPad input + LCD backlight live on the STM32 co-processor (I2C @0x42).
	crosspad_stm32_init();
	crosspad_stm32_set_brightness(90);

	// No audio for now: CrossPad audio is dual ES8388 over I2S, and the upstream
	// I2S pins (bclk=47/ws=21) collide with the LCD (MOSI=47/DC=21). doom_init_sound
	// returns false so DOOM runs silent. (TODO: I2S on free pins + ES8388 bring-up.)

	char *args[] = {"doomgeneric", "-iwad", "Doom1.WAD", "-mmap"};
	doomgeneric_Create(4, args);
	mcugdx_log(TAG, "Game created");
	while (true) {
		doomgeneric_Tick();
	}
}