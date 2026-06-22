#include <stdio.h>

#include "m_argv.h"

#include "doomgeneric.h"

/* GNW VENDORED COPY of external/doomgeneric/doomgeneric/doomgeneric.c.
 * Change vs original: DO NOT allocate the 256KB ARGB8888 DG_ScreenBuffer.
 * On the G&W (724KB RAM_EMU shared with the DOOM zone) that buffer starved
 * the zone and the WAD lumpinfo realloc (I_Error "Couldn't realloc lumpinfo").
 * The vendored i_video.c no longer expands into DG_ScreenBuffer, and our
 * DG_DrawFrame (main_doom.c) blits the 8bpp I_VideoBuffer + palette straight
 * to the RGB565 LCD, so DG_ScreenBuffer is unused and stays NULL. */
pixel_t* DG_ScreenBuffer = NULL;

void M_FindResponseFile(void);
void D_DoomMain (void);


void doomgeneric_Create(int argc, char **argv)
{
	// save arguments
    myargc = argc;
    myargv = argv;

	M_FindResponseFile();

	/* DG_ScreenBuffer intentionally not allocated (see note above). */

	DG_Init();

	D_DoomMain ();
}

