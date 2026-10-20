# crosspad-doom

DOOM ([doomgeneric](https://github.com/ozkl/doomgeneric)) as a CrossPad app:
the one app of the `doom` firmware kit, started from the Firmware app's
library like any other firmware and left through DOOM's own **Quit Game**,
which switches back to the other firmware slot.

Everything goes through crosspad-core and crosspad-gui (2.x), nothing through
a platform's own API: the pads are a pad logic (`IPadLogicHandler`), the
encoder an LVGL group, the power click a back handler, the sound effects and
the music a node on the audio module (`IAudioNode`), leaving the firmware the
Firmware views' service (`IFwViewService::flowSwitchAndRestart`).

The frame is stretched from 320x200 over as many lines as the app gets, up
to 240 -- DOOM's 4:3 -- so a kit without a top bar shows it as on a CRT.

## Controls

Pads, by their printed labels (1 = bottom left, 16 = top right):

| Pad | | Pad | | Pad | | Pad | |
|---|---|---|---|---|---|---|---|
| 13 | use | 14 | menu | 15 | enter | 16 | map |
| 9 | | 10 | forward | 11 | | 12 | run |
| 5 | turn left | 6 | fire | 7 | turn right | 8 | strafe left |
| 1 | | 2 | back | 3 | | 4 | strafe right |

- **Power click**: DOOM's menu, open and closed.
- **Encoder**: in the menu up / down and click to choose; in the game the
  weapons and click to use.
- **Quit Game** in the menu, confirmed with the encoder: back to the firmware
  in the other slot.
- **Pad 16 held while the board boots**: the platform starts the main
  firmware instead, whatever is in the slots -- the way out when DOOM's own
  menu is no use. That is the platform's, not this app's.

## Data on the card

`crosspad/doom/` holds the IWAD (`doom1.wad`, the shareware one, is in
`data/`; `doom.wad` and `doom2.wad` are found too), the configuration and the
saves.

## Layout

| Path | |
|---|---|
| `src/DoomApp.cpp` | the app: frame, pads, encoder, power, sound node, Quit |
| `port/` | doomgeneric's platform functions and sound module (`crosspad_doom_port.c`), the OPL driver (`crosspad_opl.c`, Chocolate Doom's `opl_sdl.c` without SDL), and what they ask of the app (`crosspad_doom_host.h`) |
| `third_party/doomgeneric/` | upstream e257a09, with patches marked `crosspad:` (Quit, I_Error, the configuration directory, the y/n prompts, no SDL); `i_oplmusic.c` and `midifile.c` from Chocolate Doom 2.1.0 |
| `third_party/opl/` | Chocolate Doom 2.1.0's OPL library (DBOPL), `OPL_Delay` without SDL |
| `crosspad_pc.cmake` | the simulator's build of the C side |
| `CMakeLists.txt` | the ESP-IDF component: built into the `doom` kit only (`crosspad-app.json` `kits`), empty in every other image |

DOOM's task has a 256 kB stack (in PSRAM on the board) and a 4 MB zone.

Music is DOOM's own: the MUS lumps through Chocolate Doom's OPL player and
the DOSBox OPL emulator, as an AdLib / Sound Blaster played them.

## Licence

DOOM's source (doomgeneric, Chocolate Doom) is GPL-2.0-or-later
(`third_party/doomgeneric/LICENSE`); the CrossPad parts are GPL-3.0-or-later.
`data/doom1.wad` is id Software's shareware episode, distributed unmodified.
