// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file DoomApp.cpp
 * @brief DOOM (doomgeneric) as a CrossPad app: the frame on the panel, the
 *        pads and the encoder as DOOM's keys, the sound effects and the OPL
 *        music as a node on the audio module, and the menu's Quit as the way
 *        back to the other firmware slot.
 *
 * Meant as the one app of a firmware of its own (the "doom" kit): nothing in
 * it knows the launcher. DOOM itself runs on a task of its own; what it hands
 * over crosses through a triple-buffered frame, a key queue and a command
 * ring the audio thread drains, never a lock the LVGL or audio thread waits on.
 */

#if USE_LVGL

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include <crosspad/app/AppRegistrar.hpp>
#include <crosspad/audio/IAudioModule.hpp>
#include <crosspad/audio/IAudioNode.hpp>
#include <crosspad/pad/IPadLogicHandler.hpp>
#include <crosspad/pad/PadManager.hpp>
#include <crosspad/platform/IClock.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include "crosspad-gui/components/app_icon.h"
#include "crosspad-gui/components/power_gesture.h"
#include "crosspad-gui/fw/IFwViewService.h"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad-gui/widgets/gui_helpers.h"

#include "lvgl.h"

#include "../port/crosspad_doom_host.h"

extern "C" {
#include "../third_party/doomgeneric/doomgeneric.h"
#include "../third_party/doomgeneric/doomkeys.h"
}

class App;

namespace {

constexpr const char* kPadLogicName = "Doom";
constexpr const char* kDataDir = "/crosspad/doom";
/// The IWADs looked for in kDataDir, in order: the shareware one first.
constexpr const char* kIwads[] = {"doom1.wad", "DOOM1.WAD", "Doom1.WAD", "doom.wad", "DOOM.WAD",
                                  "doom2.wad", "DOOM2.WAD"};

constexpr uint32_t kDoomStackBytes = 256 * 1024;   // the renderer recurses deep (BSP)
constexpr UBaseType_t kDoomPriority = 1;
constexpr uint32_t kFrameTimerMs = 15;
/// A tap holds the key over two of DOOM's 35 Hz tics, or a ticcmd misses it.
constexpr uint32_t kTapHoldMs = 60;
constexpr UBaseType_t kKeyQueueLen = 64;
/// DOOM's 320x200 drew on 4:3 monitors as 320x240: the frame is stretched
/// over as many of those lines as the app is given (none under a top bar).
constexpr uint32_t kTallH = 240;
constexpr size_t kMaxFramePixels = CPDOOM_W * kTallH;

/* ── Keys: the pad map (index 0 = bottom left, 15 = top right) ──────────
 *    12 13 14 15      13 = use      14 = menu (Esc)  15 = enter  16 = map
 *     8  9 10 11      10 = forward                   12 = run
 *     4  5  6  7       5 = turn left  6 = fire   7 = turn right  8 = strafe left
 *     0  1  2  3       2 = back                                  4 = strafe right
 * (numbers on the right are the pads' printed labels, index + 1) */
constexpr unsigned char kPadKeys[16] = {
    0,           KEY_DOWNARROW, 0,          KEY_STRAFE_R,   // 1..4
    KEY_LEFTARROW, KEY_FIRE,    KEY_RIGHTARROW, KEY_STRAFE_L, // 5..8
    0,           KEY_UPARROW,   0,          KEY_RSHIFT,     // 9..12
    KEY_USE,     KEY_ESCAPE,    KEY_ENTER,  KEY_TAB,        // 13..16
};

const crosspad::RgbColor kMoveColor(90, 0, 0);
const crosspad::RgbColor kActionColor(0, 20, 90);
const crosspad::RgbColor kOffColor(0, 0, 0);
const crosspad::RgbColor kHeldColor(255, 140, 0);

crosspad::RgbColor padIdleColor(uint8_t pad) {
    switch (kPadKeys[pad]) {
        case 0: return kOffColor;
        case KEY_UPARROW: case KEY_DOWNARROW: case KEY_LEFTARROW: case KEY_RIGHTARROW: case KEY_FIRE:
            return kMoveColor;
        default: return kActionColor;
    }
}

struct KeyEvent {
    uint8_t key;
    uint8_t pressed;
};

QueueHandle_t s_keys = nullptr;
/// The up half of a tap, due at `at` (DOOM task only).
struct PendingUp {
    uint8_t key = 0;
    uint32_t at = 0;
};
PendingUp s_pendingUps[8];

void postKey(uint8_t key, bool pressed) {
    if (!s_keys || key == 0) return;
    const KeyEvent ev{key, static_cast<uint8_t>(pressed ? 1 : 0)};
    xQueueSend(s_keys, &ev, 0);   // a full queue drops the key: DOOM is not reading
}

/// A press whose release follows kTapHoldMs later (the encoder, power).
constexpr uint8_t kTapFlag = 0x80;   // KeyEvent.pressed bit: release by itself
void postTap(uint8_t key) {
    if (!s_keys || key == 0) return;
    const KeyEvent ev{key, static_cast<uint8_t>(1 | kTapFlag)};
    xQueueSend(s_keys, &ev, 0);
}

/* ── The frame: DOOM task → LVGL thread, triple buffered ─────────────── */

uint16_t* s_frames[3] = {};                 // RGB565, s_outH lines
uint16_t* s_shown = nullptr;                // the image's own pixels
uint32_t s_outH = CPDOOM_H;                 // set before the DOOM task starts
constexpr unsigned kDirty = 4;
std::atomic<unsigned> s_ready{1};           // index | kDirty when unseen
unsigned s_back = 0;                        // DOOM task's
unsigned s_front = 2;                       // LVGL thread's
std::atomic<bool> s_menuActive{false};

lv_image_dsc_t s_dsc{};

/* ── Sound effects: DOOM task → audio thread through a command ring ──── */

class DoomAudioNode : public crosspad::IAudioNode {
public:
    enum class Op : uint8_t { Start, Params, Stop };
    struct Cmd {
        Op op;
        uint8_t channel;
        uint8_t vol;
        uint8_t sep;
        uint32_t gen;
        const uint8_t* pcm;
        uint32_t length;
        uint32_t rate;
    };

    const char* name() const override { return "Doom"; }
    bool isGenerator() const override { return true; }
    void onPrepare(uint32_t, uint32_t sampleRate) override { outRate_.store(sampleRate); }
    uint32_t rate() const { return outRate_.load(); }

    // DOOM task
    void start(int ch, const uint8_t* pcm, uint32_t length, uint32_t rate, int vol, int sep) {
        const uint32_t gen = ++started_[ch];
        push({Op::Start, static_cast<uint8_t>(ch), clampVol(vol), clampSep(sep), gen, pcm, length, rate});
    }
    void params(int ch, int vol, int sep) {
        push({Op::Params, static_cast<uint8_t>(ch), clampVol(vol), clampSep(sep), started_[ch], nullptr, 0, 0});
    }
    void stop(int ch) { push({Op::Stop, static_cast<uint8_t>(ch), 0, 0, started_[ch], nullptr, 0, 0}); }
    bool playing(int ch) const { return finished_[ch].load(std::memory_order_acquire) != started_[ch]; }

    // Audio thread
    void mix(float* out, uint32_t frames) override {
        drain();
        const uint32_t outRate = outRate_.load(std::memory_order_relaxed);
        if (outRate == 0) return;
        for (Voice& v : voices_) {
            if (!v.pcm) continue;
            const uint64_t step = (static_cast<uint64_t>(v.rate) << kFrac) / outRate;
            for (uint32_t i = 0; i < frames; ++i) {
                const uint32_t idx = static_cast<uint32_t>(v.pos >> kFrac);
                if (idx >= v.length) {
                    finish(v);
                    break;
                }
                const float s = (static_cast<float>(v.pcm[idx]) - 128.0f) * (1.0f / 128.0f);
                out[i * 2 + 0] += s * v.gainL;
                out[i * 2 + 1] += s * v.gainR;
                v.pos += step;
            }
        }
        cpdoom_music_render(out, frames);
    }

private:
    static constexpr unsigned kFrac = 16;
    static constexpr uint32_t kRing = 64;
    static constexpr float kLevel = 1.0f;   // SDL_mixer's full volume, as i_sdlsound plays them

    struct Voice {
        const uint8_t* pcm = nullptr;
        uint32_t length = 0;
        uint32_t rate = 0;
        uint64_t pos = 0;
        float gainL = 0.0f, gainR = 0.0f;
        uint32_t gen = 0;
        uint8_t channel = 0;
    };

    static uint8_t clampVol(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 127 ? 127 : v); }
    static uint8_t clampSep(int s) { return static_cast<uint8_t>(s < 0 ? 0 : s > 254 ? 254 : s); }

    static void gains(Voice& v, uint8_t vol, uint8_t sep) {
        // i_sdlsound's panning: left = (254 - sep) * vol / 127, right = sep * vol / 127, of 254.
        v.gainL = kLevel * static_cast<float>((254 - sep) * vol) / (127.0f * 254.0f);
        v.gainR = kLevel * static_cast<float>(sep * vol) / (127.0f * 254.0f);
    }

    void finish(Voice& v) {
        v.pcm = nullptr;
        finished_[v.channel].store(v.gen, std::memory_order_release);
    }

    void push(const Cmd& c) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= kRing) return;   // no audio thread draining
        ring_[head % kRing] = c;
        head_.store(head + 1, std::memory_order_release);
    }

    void drain() {
        uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        for (; tail != head; ++tail) {
            const Cmd& c = ring_[tail % kRing];
            Voice& v = voices_[c.channel];
            switch (c.op) {
                case Op::Start:
                    if (v.pcm) finished_[c.channel].store(v.gen, std::memory_order_release);
                    v.pcm = c.pcm;
                    v.length = c.length;
                    v.rate = c.rate;
                    v.pos = 0;
                    v.gen = c.gen;
                    v.channel = c.channel;
                    gains(v, c.vol, c.sep);
                    break;
                case Op::Params:
                    if (v.pcm && v.gen == c.gen) gains(v, c.vol, c.sep);
                    break;
                case Op::Stop:
                    if (v.pcm && v.gen == c.gen) finish(v);
                    break;
            }
        }
        tail_.store(tail, std::memory_order_release);
    }

    Cmd ring_[kRing]{};
    std::atomic<uint32_t> head_{0}, tail_{0};
    Voice voices_[CPDOOM_SFX_CHANNELS];
    uint32_t started_[CPDOOM_SFX_CHANNELS]{};                  // DOOM task's
    std::atomic<uint32_t> finished_[CPDOOM_SFX_CHANNELS]{};    // audio thread's
    std::atomic<uint32_t> outRate_{0};
};

DoomAudioNode s_audio;
bool s_audioAttached = false;

/* ── The app ──────────────────────────────────────────────────────────── */

class DoomPadLogic : public crosspad::IPadLogicHandler {
public:
    void onActivate(crosspad::PadManager& pm) override {
        for (uint8_t i = 0; i < 16; ++i) pm.setPadColor(i, padIdleColor(i));
    }
    void onDeactivate(crosspad::PadManager& pm) override {
        for (uint8_t i = 0; i < 16; ++i) {
            if (kPadKeys[i]) postKey(kPadKeys[i], false);
            pm.setPadColor(i, kOffColor);
        }
    }
    void onPadPress(crosspad::PadManager& pm, uint8_t pad, uint8_t) override {
        if (pad >= 16 || !kPadKeys[pad]) return;
        postKey(kPadKeys[pad], true);
        pm.setPadColor(pad, kHeldColor);
    }
    void onPadRelease(crosspad::PadManager& pm, uint8_t pad) override {
        if (pad >= 16 || !kPadKeys[pad]) return;
        postKey(kPadKeys[pad], false);
        pm.setPadColor(pad, padIdleColor(pad));
    }
    void onPadPressure(crosspad::PadManager&, uint8_t, uint8_t) override {}
};

std::shared_ptr<DoomPadLogic> s_padLogic;
TaskHandle_t s_doomTask = nullptr;
std::string s_dataDir;
std::string s_iwad;
std::string s_errorText;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_image = nullptr;
lv_obj_t* s_message = nullptr;
lv_group_t* s_group = nullptr;
lv_group_t* s_prevGroup = nullptr;
lv_timer_t* s_frameTimer = nullptr;
uint32_t s_backToken = 0;
bool s_fatal = false;

SemaphoreHandle_t s_quitAnswer = nullptr;

lv_indev_t* navEncoder() { return crosspad_gui::getGuiPlatform().getNavigationEncoder(); }

void showMessage(const char* text) {
    if (!s_root) return;
    if (!s_message) {
        s_message = lv_label_create(s_root);
        lv_obj_set_width(s_message, lv_pct(90));
        lv_obj_set_style_text_color(s_message, lv_color_hex(0xFF6040), 0);
        lv_obj_set_style_text_align(s_message, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s_message, LV_LABEL_LONG_WRAP);
        lv_obj_center(s_message);
    }
    lv_label_set_text(s_message, text);
}

/// Back to the other firmware slot; false when the platform refused.
bool leaveFirmware() {
    if (crosspad_gui::fwViewService().flowSwitchAndRestart(false)) return true;
    showMessage("There is no other firmware to go back to.");
    return false;
}

/* The power click opens and closes DOOM's menu -- before the encoder's edit
 * mode, which is always on here, would take it. */
bool onPowerBack(void*) {
    if (s_fatal || s_iwad.empty()) {
        leaveFirmware();
        return true;
    }
    postTap(KEY_ESCAPE);
    return true;
}

void onEncoderKey(lv_event_t* e) {
    const uint32_t k = lv_event_get_key(e);
    const bool menu = s_menuActive.load(std::memory_order_relaxed);
    if (k == LV_KEY_LEFT) postTap(menu ? KEY_UPARROW : CPDOOM_KEY_PREV_WEAPON);
    else if (k == LV_KEY_RIGHT) postTap(menu ? KEY_DOWNARROW : CPDOOM_KEY_NEXT_WEAPON);
    else if (k == LV_KEY_ENTER) postTap(menu ? KEY_ENTER : KEY_USE);
    else return;
    lv_event_stop_processing(e);
}

void frameTimer(lv_timer_t*) {
    // A long press of the encoder toggles edit mode off; DOOM wants the turns.
    if (s_group && !lv_group_get_editing(s_group)) lv_group_set_editing(s_group, true);
    if (!(s_ready.load(std::memory_order_acquire) & kDirty)) return;
    s_front = s_ready.exchange(s_front, std::memory_order_acq_rel) & ~kDirty;
    std::memcpy(s_shown, s_frames[s_front], CPDOOM_W * s_outH * sizeof(uint16_t));
    if (s_image) lv_obj_invalidate(s_image);
}

void doomTask(void*) {
    std::string iwad = s_iwad;
    char arg0[] = "doom";
    char arg1[] = "-iwad";
    char* argv[] = {arg0, arg1, iwad.data(), nullptr};
    doomgeneric_Create(3, argv);
    for (;;) doomgeneric_Tick();
}

std::string findIwad(const std::string& dir) {
    for (const char* name : kIwads) {
        const std::string path = dir + name;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            std::fclose(f);
            return path;
        }
    }
    return {};
}

bool allocFrames() {
    if (s_shown) return true;
    for (auto& f : s_frames) {
        f = static_cast<uint16_t*>(std::malloc(kMaxFramePixels * sizeof(uint16_t)));
        if (!f) return false;
        std::memset(f, 0, kMaxFramePixels * sizeof(uint16_t));
    }
    s_shown = static_cast<uint16_t*>(std::malloc(kMaxFramePixels * sizeof(uint16_t)));
    if (!s_shown) return false;
    std::memset(s_shown, 0, kMaxFramePixels * sizeof(uint16_t));
    return true;
}

void startDoom() {
    if (s_doomTask) return;
    s_dataDir = crosspad_gui::getGuiPlatform().storagePath(kDataDir) + "/";
    s_iwad = findIwad(s_dataDir);
    if (s_iwad.empty()) {
        std::string text = "No DOOM WAD found.\nPut doom1.wad in ";
        text += kDataDir;
        text += "\non the card.\n\nPower: back to the other firmware.";
        showMessage(text.c_str());
        return;
    }
    if (!s_keys) s_keys = xQueueCreate(kKeyQueueLen, sizeof(KeyEvent));
    if (!s_quitAnswer) s_quitAnswer = xSemaphoreCreateBinary();
    if (!s_audioAttached) {
        crosspad::getAudioModule().addNode(&s_audio);
        s_audioAttached = true;
    }
    printf("[Doom] IWAD %s, saves in %s\n", s_iwad.c_str(), s_dataDir.c_str());
    if (xTaskCreate(doomTask, "doom", kDoomStackBytes / sizeof(StackType_t), nullptr, kDoomPriority,
                    &s_doomTask) != pdPASS) {
        s_doomTask = nullptr;
        showMessage("DOOM could not start: no memory for its task.");
    }
}

lv_obj_t* lv_CreateDoom(lv_obj_t* parent, App* app) {
    (void)app;
    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    if (!allocFrames()) {
        showMessage("DOOM: no memory for its frames.");
        return s_root;
    }
    if (!s_doomTask) {
        lv_obj_update_layout(parent);
        const int32_t h = lv_obj_get_content_height(parent);
        s_outH = h >= static_cast<int32_t>(kTallH) ? kTallH : h > static_cast<int32_t>(CPDOOM_H) ? h : CPDOOM_H;
    }
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = CPDOOM_W;
    s_dsc.header.h = s_outH;
    s_dsc.header.stride = CPDOOM_W * sizeof(uint16_t);
    s_dsc.data = reinterpret_cast<const uint8_t*>(s_shown);
    s_dsc.data_size = CPDOOM_W * s_outH * sizeof(uint16_t);
    s_image = lv_image_create(s_root);
    lv_image_set_src(s_image, &s_dsc);
    lv_obj_center(s_image);

    /* The encoder's group: one object always in edit mode, so a turn is a
     * key here rather than a move of focus. Scrollable, so LVGL counts it
     * editable; it has nothing to scroll. */
    s_group = lv_group_create();
    lv_group_add_obj(s_group, s_image);
    lv_obj_add_event_cb(s_image, onEncoderKey, static_cast<lv_event_code_t>(LV_EVENT_KEY | LV_EVENT_PREPROCESS),
                        nullptr);
    if (lv_indev_t* nav = navEncoder()) {
        s_prevGroup = lv_indev_get_group(nav);
        lv_indev_set_group(nav, s_group);
    }
    lv_group_focus_obj(s_image);
    lv_group_set_editing(s_group, true);

    s_backToken = crosspad_gui::ui_back_push(onPowerBack, nullptr);
    s_frameTimer = lv_timer_create(frameTimer, kFrameTimerMs, nullptr);

    if (!s_padLogic) {
        s_padLogic = std::make_shared<DoomPadLogic>();
        crosspad::getPadManager().registerPadLogic(kPadLogicName, s_padLogic);
    }
    if (s_fatal) showMessage(s_errorText.c_str());
    startDoom();
    return s_root;
}

void lv_DestroyDoom(lv_obj_t* root) {
    if (s_frameTimer) {
        lv_timer_delete(s_frameTimer);
        s_frameTimer = nullptr;
    }
    crosspad_gui::ui_back_remove(s_backToken);
    s_backToken = 0;
    if (lv_indev_t* nav = navEncoder()) lv_indev_restore_group(nav, s_prevGroup);
    if (s_group) {
        lv_group_delete(s_group);
        s_group = nullptr;
    }
    s_image = nullptr;
    s_message = nullptr;
    s_root = nullptr;
    if (root) lv_obj_delete_async(root);
}

void quitOnGui(void*) {
    const bool left = leaveFirmware();
    if (!left) xSemaphoreGive(s_quitAnswer);
}

void fatalOnGui(void*) {
    showMessage(s_errorText.c_str());
}

} // namespace

/* ── DOOM's side (crosspad_doom_host.h), on the DOOM task ─────────────── */

extern "C" {

uint32_t cpdoom_ticks_ms(void) {
    return static_cast<uint32_t>(crosspad::getClock().getTimeUs() / 1000);
}

void cpdoom_sleep_ms(uint32_t ms) {
    const TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

int cpdoom_next_key(int* pressed, unsigned char* key) {
    const uint32_t now = cpdoom_ticks_ms();
    for (PendingUp& p : s_pendingUps) {
        if (p.key && static_cast<int32_t>(now - p.at) >= 0) {
            *pressed = 0;
            *key = p.key;
            p.key = 0;
            return 1;
        }
    }
    KeyEvent ev;
    if (!s_keys || xQueueReceive(s_keys, &ev, 0) != pdTRUE) return 0;
    if (ev.pressed & kTapFlag) {
        for (PendingUp& p : s_pendingUps) {
            if (p.key) continue;
            p.key = ev.key;
            p.at = now + kTapHoldMs;
            break;
        }
    }
    *pressed = ev.pressed & 1;
    *key = ev.key;
    return 1;
}

void cpdoom_frame(const uint8_t* indices, const uint8_t* palette) {
    uint16_t lut[256];
    for (int i = 0; i < 256; ++i) {
        const uint8_t r = palette[i * 3], g = palette[i * 3 + 1], b = palette[i * 3 + 2];
        lut[i] = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
    uint16_t* out = s_frames[s_back];
    for (uint32_t y = 0; y < s_outH; ++y) {
        const uint8_t* row = indices + (y * CPDOOM_H / s_outH) * CPDOOM_W;
        for (uint32_t x = 0; x < CPDOOM_W; ++x) *out++ = lut[row[x]];
    }
    s_back = s_ready.exchange(s_back | kDirty, std::memory_order_acq_rel) & ~kDirty;
}

void cpdoom_menu_active(int active) { s_menuActive.store(active != 0, std::memory_order_relaxed); }

const char* cpdoom_data_dir(void) { return s_dataDir.c_str(); }

void cpdoom_quit(void) {
    printf("[Doom] Quit: back to the other firmware\n");
    crosspad_gui::getGuiPlatform().postToGui(quitOnGui, nullptr);
    xSemaphoreTake(s_quitAnswer, portMAX_DELAY);   // given only when the switch was refused
}

void cpdoom_fatal(const char* message) {
    printf("[Doom] I_Error: %s\n", message);
    s_errorText = std::string("DOOM stopped:\n") + message + "\n\nPower: back to the other firmware.";
    s_fatal = true;
    crosspad_gui::getGuiPlatform().postToGui(fatalOnGui, nullptr);
    vTaskSuspend(nullptr);
}

void cpdoom_sfx_start(int channel, const uint8_t* pcm, uint32_t length, uint32_t rate, int vol, int sep) {
    s_audio.start(channel, pcm, length, rate, vol, sep);
}
void cpdoom_sfx_params(int channel, int vol, int sep) { s_audio.params(channel, vol, sep); }
void cpdoom_sfx_stop(int channel) { s_audio.stop(channel); }
int cpdoom_sfx_playing(int channel) { return s_audio.playing(channel) ? 1 : 0; }

uint32_t cpdoom_audio_rate(void) { return s_audio.rate(); }

/* std::mutex, not a FreeRTOS one: the audio thread is no FreeRTOS task on
 * every platform. Never freed: the OPL driver keeps its two for good. */
void* cpdoom_mutex_create(void) { return new std::mutex(); }
void cpdoom_mutex_lock(void* m) { static_cast<std::mutex*>(m)->lock(); }
void cpdoom_mutex_unlock(void* m) { static_cast<std::mutex*>(m)->unlock(); }

} // extern "C"

REGISTER_APP_PL(Doom, nullptr, crosspad_gui::resolveAppIcon("doom.png"), lv_CreateDoom, lv_DestroyDoom,
                nullptr, nullptr, nullptr, 0, kPadLogicName)

#endif // USE_LVGL
