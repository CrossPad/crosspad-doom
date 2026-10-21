#ifndef DOOM_GENERIC
#define DOOM_GENERIC

#include <stdlib.h>
#include <stdint.h>

#ifndef DOOMGENERIC_RESX
#define DOOMGENERIC_RESX 640
#endif  // DOOMGENERIC_RESX

#ifndef DOOMGENERIC_RESY
#define DOOMGENERIC_RESY 400
#endif  // DOOMGENERIC_RESY


#ifdef CMAP256

typedef uint8_t pixel_t;

#else  // CMAP256

typedef uint32_t pixel_t;

#endif  // CMAP256


extern pixel_t* DG_ScreenBuffer;

#ifdef __cplusplus
extern "C" {
#endif

void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick();


//Implement below functions for your platform
void DG_Init();
void DG_DrawFrame();
void DG_SleepMs(uint32_t ms);
uint32_t DG_GetTicksMs();
int DG_GetKey(int* pressed, unsigned char* key);
void DG_SetWindowTitle(const char * title);

/* crosspad: the quit confirmed in the menu. Returns only when the platform
 * refused to leave; the game then goes on. */
void DG_Quit(void);
/* crosspad: the directory DOOM keeps its configuration and saves in, with
 * a trailing separator. */
const char* DG_DataDir(void);
/* crosspad: I_Error's message; never returns. */
void DG_Fatal(const char* message);

#ifdef __cplusplus
}
#endif

#endif //DOOM_GENERIC
