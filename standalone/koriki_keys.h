/*
 * koriki_keys.h - Miyoo Mini buttons for SDL 1.2 emulators written for the
 * old Miyoo (BittBoy / PocketGo / MiyooCFW).
 *
 * Force-included when building (-include koriki_keys.h), so the emulator
 * source keeps its own key handling. SDL_PollEvent, SDL_WaitEvent,
 * SDL_GetKeyState and SDL_PumpEvents are wrapped and every Miyoo Mini key
 * code is translated to the one the emulator expects for that button.
 *
 * The old-Miyoo emulators do not agree on the face buttons:
 *
 *   button   Miyoo Mini   MiyooCFW (default)   BittBoy (KORIKI_LAYOUT_BITTBOY)
 *   A        SPACE        LALT                 LCTRL
 *   B        LCTRL        LCTRL                LALT
 *   X        LSHIFT       LSHIFT               LSHIFT
 *   Y        LALT         SPACE                SPACE
 *   L1       E            TAB                  TAB
 *   R1       T            BACKSPACE            BACKSPACE
 *   L2       TAB          PAGEUP               PAGEUP
 *   R2       BACKSPACE    PAGEDOWN             PAGEDOWN
 *   SELECT   RCTRL        ESCAPE               ESCAPE
 *   START    RETURN       RETURN               RETURN
 *   MENU     ESCAPE       KORIKI_MENU_KEY      KORIKI_MENU_KEY
 *   d-pad    arrows       arrows               arrows
 *
 * MiyooCFW: RACE, Temper, gpSP.  BittBoy: Gambatte-DMS, Oswan.
 * KORIKI_MENU_KEY is RCTRL by default (RESET/menu), set per emulator.
 *
 * The translation is a single lookup, so swapped pairs (A/Y, SELECT/MENU)
 * do not chain.
 */
#ifndef KORIKI_KEYS_H
#define KORIKI_KEYS_H
#ifndef __ASSEMBLER__

#include <SDL/SDL.h>
#include <string.h>

#ifndef KORIKI_MENU_KEY
#define KORIKI_MENU_KEY SDLK_RCTRL
#endif

/* key the emulator expects for each Miyoo Mini button */
#ifdef KORIKI_LAYOUT_BITTBOY
#define KORIKI_TO_A      SDLK_LCTRL
#define KORIKI_TO_B      SDLK_LALT
#else
#define KORIKI_TO_A      SDLK_LALT
#define KORIKI_TO_B      SDLK_LCTRL
#endif
#define KORIKI_TO_X      SDLK_LSHIFT
#define KORIKI_TO_Y      SDLK_SPACE
#define KORIKI_TO_L1     SDLK_TAB
#define KORIKI_TO_R1     SDLK_BACKSPACE
#define KORIKI_TO_L2     SDLK_PAGEUP
#define KORIKI_TO_R2     SDLK_PAGEDOWN
#define KORIKI_TO_SELECT SDLK_ESCAPE
#define KORIKI_TO_START  SDLK_RETURN
#define KORIKI_TO_MENU   KORIKI_MENU_KEY
#define KORIKI_TO_UP     SDLK_UP
#define KORIKI_TO_DOWN   SDLK_DOWN
#define KORIKI_TO_LEFT   SDLK_LEFT
#define KORIKI_TO_RIGHT  SDLK_RIGHT

static inline SDLKey koriki_key(SDLKey k)
{
	switch (k) {
	case SDLK_SPACE:     return KORIKI_TO_A;      /* A      */
	case SDLK_LCTRL:     return KORIKI_TO_B;      /* B      */
	case SDLK_LSHIFT:    return KORIKI_TO_X;      /* X      */
	case SDLK_LALT:      return KORIKI_TO_Y;      /* Y      */
	case SDLK_e:         return KORIKI_TO_L1;     /* L1     */
	case SDLK_t:         return KORIKI_TO_R1;     /* R1     */
	case SDLK_TAB:       return KORIKI_TO_L2;     /* L2     */
	case SDLK_BACKSPACE: return KORIKI_TO_R2;     /* R2     */
	case SDLK_RCTRL:     return KORIKI_TO_SELECT; /* SELECT */
	case SDLK_RETURN:    return KORIKI_TO_START;  /* START  */
	case SDLK_ESCAPE:    return KORIKI_TO_MENU;   /* MENU   */
	case SDLK_UP:        return KORIKI_TO_UP;     /* UP     */
	case SDLK_DOWN:      return KORIKI_TO_DOWN;   /* DOWN   */
	case SDLK_LEFT:      return KORIKI_TO_LEFT;   /* LEFT   */
	case SDLK_RIGHT:     return KORIKI_TO_RIGHT;  /* RIGHT  */
	default:             return k;                /* not a Miyoo Mini button */
	}
}

/* every Miyoo Mini button: its state moves to the translated slot */
static const SDLKey koriki_moved[] = {
	SDLK_SPACE, SDLK_LCTRL, SDLK_LSHIFT, SDLK_LALT, SDLK_e, SDLK_t,
	SDLK_TAB, SDLK_BACKSPACE, SDLK_RCTRL, SDLK_RETURN, SDLK_ESCAPE,
	SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT
};

static Uint8 koriki_keystate[SDLK_LAST];

/* rebuild the translated copy of SDL's key-state array */
static inline void koriki_refresh_keystate(void)
{
	unsigned i;
	Uint8 *in = (SDL_GetKeyState)(NULL);

	if (!in)
		return;
	memcpy(koriki_keystate, in, sizeof(koriki_keystate));
	for (i = 0; i < sizeof(koriki_moved) / sizeof(koriki_moved[0]); i++)
		koriki_keystate[koriki_key(koriki_moved[i])] = 0;
	for (i = 0; i < sizeof(koriki_moved) / sizeof(koriki_moved[0]); i++)
		if (in[koriki_moved[i]])
			koriki_keystate[koriki_key(koriki_moved[i])] = 1;
}

static inline int koriki_PollEvent(SDL_Event *e)
{
	int r = (SDL_PollEvent)(e);

	if (r && e && (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP))
		e->key.keysym.sym = koriki_key(e->key.keysym.sym);
	koriki_refresh_keystate();
	return r;
}

static inline int koriki_WaitEvent(SDL_Event *e)
{
	int r = (SDL_WaitEvent)(e);

	if (r && e && (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP))
		e->key.keysym.sym = koriki_key(e->key.keysym.sym);
	koriki_refresh_keystate();
	return r;
}

static inline void koriki_PumpEvents(void)
{
	(SDL_PumpEvents)();
	koriki_refresh_keystate();
}

/* the returned array stays valid and is refreshed by the wrappers above,
   like SDL's own array is refreshed by SDL_PumpEvents */
static inline Uint8 *koriki_GetKeyState(int *numkeys)
{
	if (numkeys)
		*numkeys = SDLK_LAST;
	koriki_refresh_keystate();
	return koriki_keystate;
}

#define SDL_PollEvent  koriki_PollEvent
#define SDL_WaitEvent  koriki_WaitEvent
#define SDL_PumpEvents koriki_PumpEvents
#define SDL_GetKeyState koriki_GetKeyState

#endif /* __ASSEMBLER__ */
#endif /* KORIKI_KEYS_H */
