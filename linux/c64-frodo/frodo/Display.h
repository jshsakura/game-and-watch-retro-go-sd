/*
 *  Display.h - C64 graphics display, emulator window handling
 *
 *  Frodo (C) 1994-1997,2002 Christian Bauer
 */

#ifndef _DISPLAY_H
#define _DISPLAY_H

#ifdef __BEOS__
#include <InterfaceKit.h>
#endif

#ifdef AMIGA
#include <graphics/rastport.h>
#endif

#ifdef HAVE_SDL
struct SDL_Surface;
#endif

#ifdef WIN32
#include <ddraw.h>
#endif

#ifdef __riscos__
#include "ROlib.h"
#endif


// Display dimensions
#if defined(SMALL_DISPLAY)
const int DISPLAY_X = 0x168;
const int DISPLAY_Y = 0x110;
#else
/* G&W RAM-fit: the Frodo render target is the single biggest overlay-.bss item
 * (DISPLAY_X*DISPLAY_Y bytes). Shrunk from 384x272 to 340x208 so the C++ overlay
 * heap (badheap) gains ~32KB for C64::C64()'s allocations. 340 (=0x154) is the
 * minimum safe width: the 40-column display window is COL40_XSTART(20)..COL40_XSTOP(340),
 * so content occupies bytes 20..339 with no right border; the device blit crops the
 * 320px window at C64_CROP_X=20. 208 (=0xd0) rows bracket the C64 active picture
 * (raster FIRST_DISP_LINE 0x2c .. LAST_DISP_LINE 0xfb); the device blit letterboxes
 * them into the 240-tall LCD. DISPLAY_X must stay a multiple of 4 (32-bit border fills). */
const int DISPLAY_X = 0x154;
const int DISPLAY_Y = 0xd0;
#endif


class C64Window;
class C64Screen;
class C64;
class Prefs;

// Class for C64 graphics display
class C64Display {
public:
	C64Display(C64 *the_c64);
	~C64Display();

	void Update(void);
	void UpdateLEDs(int l0, int l1, int l2, int l3);
	void Speedometer(int speed);
	uint8 *BitmapBase(void);
	int BitmapXMod(void);
#ifdef __riscos__
	void PollKeyboard(uint8 *key_matrix, uint8 *rev_matrix, uint8 *joystick, uint8 *joystick2);
#else
	void PollKeyboard(uint8 *key_matrix, uint8 *rev_matrix, uint8 *joystick);
#endif
	bool NumLock(void);
	void InitColors(uint8 *colors);
	void NewPrefs(Prefs *prefs);

	C64 *TheC64;



#if 1
	bool quit_requested;
#endif

private:
	int led_state[4];
	int old_led_state[4];



// #ifdef HAVE_SDL
// 	char speedometer_string[16];		// Speedometer text
// 	void draw_string(SDL_Surface *s, int x, int y, const char *str, uint8 front_color, uint8 back_color);
// #endif

// #ifdef __unix
// 	void draw_led(int num, int state);	// Draw one LED
// 	static void pulse_handler(...);		// LED error blinking
// #endif

};


// Exported functions
extern long ShowRequester(char *str, char *button1, char *button2 = NULL);


#endif
