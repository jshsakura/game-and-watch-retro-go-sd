//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
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
//	Mission begin melt/wipe screen special effect.
//
// GNW (Game & Watch port) vendored copy of doomgeneric's f_wipe.c.
// The stock screen-melt allocates TWO full 320x200 PU_STATIC scratch
// buffers (wipe_scr_start / wipe_scr_end = 2 x 64000 bytes) from the DOOM
// zone, plus the melt's per-column y[] array. On the device's ~490 KiB
// zone that 64024-byte allocation is the single largest transient and
// fails during the title/demo -> E1M5 transition: the zone is fragmented
// (measured: ~106 KiB free but largest contiguous run < 64 KiB), so the
// melt buffer cannot be placed and DOOM aborts with
// "Z_Malloc: failed on allocation of 64024 bytes".
//
// The melt is purely cosmetic, so this copy turns it into a no-op hard
// cut that allocates nothing from the zone. Removing it was verified (host
// ARM32 harness, device-matched) to be necessary together with serving WAD
// lumps from flash; see Core/Src/porting/doom/w_wad.c. Ref:
// next-hack/nRF52840Doom, whose GBA-derived base likewise drops the melt.
//

#include "f_wipe.h"

//
// wipe_StartScreen / wipe_EndScreen
//
// Stock DOOM captures the source and destination framebuffers into two
// freshly Z_Malloc'd 320x200 buffers here. We skip the melt entirely, so
// there is nothing to capture and nothing to allocate.
//
int
wipe_StartScreen
( int	x,
  int	y,
  int	width,
  int	height )
{
    (void) x; (void) y; (void) width; (void) height;
    return 0;
}

int
wipe_EndScreen
( int	x,
  int	y,
  int	width,
  int	height )
{
    (void) x; (void) y; (void) width; (void) height;
    return 0;
}

//
// wipe_ScreenWipe
//
// Returns "done" (1) on the first call so D_Display's do { ... } while
// (!done) loop exits immediately. The new frame has already been drawn, so
// the transition is a hard cut with zero zone allocation.
//
int
wipe_ScreenWipe
( int	wipeno,
  int	x,
  int	y,
  int	width,
  int	height,
  int	ticks )
{
    (void) wipeno; (void) x; (void) y; (void) width; (void) height; (void) ticks;
    return 1;
}
