// ID_VL.C

#include <string.h>
#include "wl_def.h"
#pragma hdrstop

// Uncomment the following line, if you get destination out of bounds
// assertion errors and want to ignore them during debugging
//#define IGNORE_BAD_DEST

#ifdef IGNORE_BAD_DEST
#undef assert
#define assert(x) if(!(x)) return
#define assert_ret(x) if(!(x)) return 0
#else
#define assert_ret(x) assert(x)
#endif

boolean fullscreen = false;


boolean usedoublebuffering = true;
unsigned screenWidth = 320;
unsigned screenHeight = 200;
unsigned screenBits = -1;

scrsurf *screen;
unsigned screenPitch;

scrsurf *screenBuffer;
unsigned bufferPitch;

unsigned scaleFactor;

boolean  screenfaded;
unsigned bordercolor;

pixcolor palette1[256], palette2[256];
pixcolor curpal[256];

uint32_t curpal_lut[256];

#define CASSERT(x) extern int ASSERT_COMPILE[((x) != 0) * 2 - 1];
#define RGB(r, g, b) {(r)*255/63, (g)*255/63, (b)*255/63, 0}

const pixcolor gamepal[]={
#ifdef SPEAR
    #include "sodpal.inc"
#else
    #include "wolfpal.inc"
#endif
};

CASSERT(lengthof(gamepal) == 256)

//===========================================================================


/*
=======================
=
= VL_Shutdown
=
=======================
*/

void    VL_Shutdown (void)
{
    //VL_SetTextMode ();
}


/*
=======================
=
= VL_SetVGAPlaneMode
=
=======================
*/

/* G&W port: real static buffers (the STM32 original placed these in LTDC
 * framebuffer RAM via the driver; here they are plain overlay BSS arrays). */
uint8_t framebuf[200][320];

static uint8_t screenbuf_arr[200][320];
uint8_t *screenbuf = (uint8_t *) screenbuf_arr;

void    VL_SetVGAPlaneMode (void)
{
    memcpy(curpal, gamepal, sizeof(pixcolor) * 256);

    screenBits = 16;

    screen = (scrsurf *) malloc(sizeof(scrsurf));
    screen->pixels.u8 = framebuf;
    screen->width = screenWidth;
    screen->height = screenHeight;
    screen->pitch = 1 * screenWidth;

    screenBuffer = (scrsurf *) malloc(sizeof(scrsurf));
    screenBuffer->pixels.u8 = screenbuf;
    screenBuffer->width = screenWidth;
    screenBuffer->height = screenHeight;
    screenBuffer->pitch = 1 * screenWidth;

    screenPitch = screen->pitch;
    bufferPitch = screenBuffer->pitch;

    scaleFactor = screenWidth/320;
    if(screenHeight/200 < scaleFactor) scaleFactor = screenHeight/200;

    
    pixelangle = (short *) malloc(screenWidth * sizeof(short));
    CHECKMALLOCRESULT(pixelangle);
    wallheight = (int *) malloc(screenWidth * sizeof(int));
    CHECKMALLOCRESULT(wallheight);
    
    
}

/*
=============================================================================

                        PALETTE OPS

        To avoid snow, do a WaitVBL BEFORE calling these

=============================================================================
*/

/*
=================
=
= VL_ConvertPalette
=
=================
*/

void VL_ConvertPalette(byte *srcpal, pixcolor *destpal, int numColors)
{
    for(int i=0; i<numColors; i++)
    {
        destpal[i].r = *srcpal++ * 255 / 63;
        destpal[i].g = *srcpal++ * 255 / 63;
        destpal[i].b = *srcpal++ * 255 / 63;
    }
}

/*
=================
=
= VL_FillPalette
=
=================
*/

void VL_FillPalette (int red, int green, int blue)
{
    int i;
    pixcolor pal[256];

    for(i=0; i<256; i++)
    {
        pal[i].r = red;
        pal[i].g = green;
        pal[i].b = blue;
    }

    VL_SetPalette(pal, true);
}

//===========================================================================

/*
=================
=
= VL_SetColor
=
=================
*/

void VL_SetColor    (int color, int red, int green, int blue)
{
    pixcolor col = { .r = (uint8_t) red, .g = (uint8_t) green, .b = (uint8_t) blue, .a = 0 };
    curpal[color] = col;
}

//===========================================================================

/*
=================
=
= VL_GetColor
=
=================
*/

void VL_GetColor    (int color, int *red, int *green, int *blue)
{
    pixcolor *col = &curpal[color];
    *red = col->r;
    *green = col->g;
    *blue = col->b;
}

//===========================================================================

/*
=================
=
= VL_SetPalette
=
=================
*/

void VL_SetPalette (const pixcolor *palette, bool forceupdate)
{
    memcpy(curpal, palette, sizeof(pixcolor) * 256);

    for (int col = 0; col < 256; col++)
    {
        uint32_t fullcol = (uint32_t) curpal[col].r << 16 |
                           (uint32_t) curpal[col].g << 8  |
                           (uint32_t) curpal[col].b << 0;
        curpal_lut[col] = fullcol;
    }

    if(forceupdate)
    {
        VH_UpdateScreen();
    }
}


//===========================================================================

/*
=================
=
= VL_GetPalette
=
=================
*/

void VL_GetPalette (pixcolor *palette)
{
    memcpy(palette, curpal, sizeof(pixcolor) * 256);
}


//===========================================================================

/*
=================
=
= VL_FadeOut
=
= Fades the current palette to the given color in the given number of steps
=
=================
*/

void VL_FadeOut (int start, int end, int red, int green, int blue, int steps)
{
    int         i,j,orig,delta;
    pixcolor   *origptr, *newptr;

    red = red * 255 / 63;
    green = green * 255 / 63;
    blue = blue * 255 / 63;

    VL_WaitVBL(1);
    VL_GetPalette(palette1);
    memcpy(palette2, palette1, sizeof(pixcolor) * 256);

//
// fade through intermediate frames
//
    for (i=0;i<steps;i++)
    {
        origptr = &palette1[start];
        newptr = &palette2[start];
        for (j=start;j<=end;j++)
        {
            orig = origptr->r;
            delta = red-orig;
            newptr->r = orig + delta * i / steps;
            orig = origptr->g;
            delta = green-orig;
            newptr->g = orig + delta * i / steps;
            orig = origptr->b;
            delta = blue-orig;
            newptr->b = orig + delta * i / steps;
            origptr++;
            newptr++;
        }

        if(!usedoublebuffering || screenBits == 8) VL_WaitVBL(1);
        VL_SetPalette (palette2, true);
    }

//
// final color
//
    VL_FillPalette (red,green,blue);

    screenfaded = true;
}


/*
=================
=
= VL_FadeIn
=
=================
*/

void VL_FadeIn (int start, int end, const pixcolor *palette, int steps)
{
    int i,j,delta;

    VL_WaitVBL(1);
    VL_GetPalette(palette1);
    memcpy(palette2, palette1, sizeof(pixcolor) * 256);

//
// fade through intermediate frames
//
    for (i=0;i<steps;i++)
    {
        for (j=start;j<=end;j++)
        {
            delta = palette[j].r-palette1[j].r;
            palette2[j].r = palette1[j].r + delta * i / steps;
            delta = palette[j].g-palette1[j].g;
            palette2[j].g = palette1[j].g + delta * i / steps;
            delta = palette[j].b-palette1[j].b;
            palette2[j].b = palette1[j].b + delta * i / steps;
        }

        if(!usedoublebuffering || screenBits == 8) VL_WaitVBL(1);
        VL_SetPalette(palette2, true);
    }

//
// final color
//
    VL_SetPalette (palette, true);
    screenfaded = false;
}

/*
=============================================================================

                            PIXEL OPS

=============================================================================
*/

/*
=================
=
= VL_Plot
=
=================
*/

void VL_Plot (int x, int y, int color)
{
    assert(x >= 0 && (unsigned) x < screenWidth
            && y >= 0 && (unsigned) y < screenHeight
            && "VL_Plot: Pixel out of bounds!");

    screenBuffer->pixels.u8[y * bufferPitch + x] = (uint8_t) color;
}

/*
=================
=
= VL_GetPixel
=
=================
*/

byte VL_GetPixel (int x, int y)
{
    assert_ret(x >= 0 && (unsigned) x < screenWidth
            && y >= 0 && (unsigned) y < screenHeight
            && "VL_GetPixel: Pixel out of bounds!");

    byte col = screenBuffer->pixels.u8[y * bufferPitch + x];
    return col;
}


/*
=================
=
= VL_Hlin
=
=================
*/

void VL_Hlin (unsigned x, unsigned y, unsigned width, int color)
{
    assert(x >= 0 && x + width <= screenWidth
            && y >= 0 && y < screenHeight
            && "VL_Hlin: Destination rectangle out of bounds!");

    uint8_t *dest = &screenBuffer->pixels.u8[y * bufferPitch + x];
    memset(dest, (uint8_t) color, width);
}


/*
=================
=
= VL_Vlin
=
=================
*/

void VL_Vlin (int x, int y, int height, int color)
{
    assert(x >= 0 && (unsigned) x < screenWidth
            && y >= 0 && (unsigned) y + height <= screenHeight
            && "VL_Vlin: Destination rectangle out of bounds!");

    uint8_t *dest = &screenBuffer->pixels.u8[y * bufferPitch + x];

    while (height--)
    {
        *dest = color;
        dest += bufferPitch;
    }
}


/*
=================
=
= VL_Bar
=
=================
*/

void VL_BarScaledCoord (int scx, int scy, int scwidth, int scheight, int color)
{
    assert(scx >= 0 && (unsigned) scx + scwidth <= screenWidth
            && scy >= 0 && (unsigned) scy + scheight <= screenHeight
            && "VL_BarScaledCoord: Destination rectangle out of bounds!");

    uint8_t *dest = &screenBuffer->pixels.u8[scy * bufferPitch + scx];

    while (scheight--)
    {
        memset(dest, color, scwidth);
        dest += bufferPitch;
    }
}

void VL_Bar      (int x, int y, int width, int height, int color)
{
    VL_BarScaledCoord(scaleFactor*x, scaleFactor*y,
        scaleFactor*width, scaleFactor*height, color);
}

void VL_ClearScreen(int color)
{
    memset(screenBuffer->pixels.u8, (uint8_t) color, screenWidth * screenHeight);
}

/*
============================================================================

                            MEMORY OPS

============================================================================
*/

/*
=================
=
= VL_MemToLatch
=
=================
*/

void VL_MemToLatch(byte *source, int width, int height,
    scrsurf *destSurface, int x, int y)
{
    assert(x >= 0 && (unsigned) x + width <= screenWidth
            && y >= 0 && (unsigned) y + height <= screenHeight
            && "VL_MemToLatch: Destination rectangle out of bounds!");

    int pitch = destSurface->pitch;
    byte *dest = (byte *) &destSurface->pixels.u8[y * pitch + x];
    for(int ysrc = 0; ysrc < height; ysrc++)
    {
        for(int xsrc = 0; xsrc < width; xsrc++)
        {
            dest[ysrc * pitch + xsrc] = source[(ysrc * (width >> 2) + (xsrc >> 2))
                + (xsrc & 3) * (width >> 2) * height];
        }
    }
}

//===========================================================================


/*
=================
=
= VL_MemToScreenScaledCoord
=
= Draws a block of data to the screen with scaling according to scaleFactor.
=
=================
*/

void VL_MemToScreenScaledCoordOrig (byte *source, int width, int height, int destx, int desty)
{
    VL_MemToScreenScaledCoord (source, width, height, 0, 0, destx, desty, width, height);
}

/*
=================
=
= VL_MemToScreenScaledCoord
=
= Draws a part of a block of data to the screen.
= The block has the size origwidth*origheight.
= The part at (srcx, srcy) has the size width*height
= and will be painted to (destx, desty) with scaling according to scaleFactor.
=
=================
*/

void VL_MemToScreenScaledCoord (byte *source, int origwidth, int origheight, int srcx, int srcy,
                                int destx, int desty, int width, int height)
{
    assert(destx >= 0 && destx + width * scaleFactor <= screenWidth
            && desty >= 0 && desty + height * scaleFactor <= screenHeight
            && "VL_MemToScreenScaledCoord: Destination rectangle out of bounds!");

    byte *vbuf = (byte *) screenBuffer->pixels.u8;
    for(int j=0,scj=0; j<height; j++, scj+=scaleFactor)
    {
        for(int i=0,sci=0; i<width; i++, sci+=scaleFactor)
        {
            byte col = source[((j+srcy)*(origwidth>>2)+((i+srcx)>>2))+((i+srcx)&3)*(origwidth>>2)*origheight];
            for(unsigned m=0; m<scaleFactor; m++)
            {
                for(unsigned n=0; n<scaleFactor; n++)
                {
                    vbuf[(scj+m+desty)*bufferPitch+sci+n+destx] = col;
                }
            }
        }
    }
}

void VL_MemToScreen (byte *source, int width, int height, int x, int y)
{
    VL_MemToScreenScaledCoordOrig(source, width, height, scaleFactor*x, scaleFactor*y);
}

//==========================================================================

/*
=================
=
= VL_LatchToScreen
=
=================
*/

void VL_LatchToScreenScaledCoord(scrsurf *source, int xsrc, int ysrc,
    int width, int height, int scxdest, int scydest)
{
    assert(scxdest >= 0 && scxdest + width * scaleFactor <= screenWidth
            && scydest >= 0 && scydest + height * scaleFactor <= screenHeight
            && "VL_LatchToScreenScaledCoord: Destination rectangle out of bounds!");

    if(scaleFactor == 1)
    {
        byte *src = (byte *) source->pixels.u8;
        unsigned srcPitch = source->pitch;

        byte *vbuf = (byte *) screenBuffer->pixels.u8;
        for(int j=0,scj=0; j<height; j++, scj++)
        {
            for(int i=0,sci=0; i<width; i++, sci++)
            {
                byte col = src[(ysrc + j)*srcPitch + xsrc + i];
                vbuf[(scydest+scj)*bufferPitch+scxdest+sci] = col;
            }
        }
    }
    else
    {
        byte *src = (byte *) source->pixels.u8;
        unsigned srcPitch = source->pitch;

        byte *vbuf = (byte *) screenBuffer->pixels.u8;
        for(int j=0,scj=0; j<height; j++, scj+=scaleFactor)
        {
            for(int i=0,sci=0; i<width; i++, sci+=scaleFactor)
            {
                byte col = src[(ysrc + j)*srcPitch + xsrc + i];
                for(unsigned m=0; m<scaleFactor; m++)
                {
                    for(unsigned n=0; n<scaleFactor; n++)
                    {
                        vbuf[(scydest+scj+m)*bufferPitch+scxdest+sci+n] = col;
                    }
                }
            }
        }
    }
}

void VL_LatchToScreen (scrsurf *source, int xsrc, int ysrc,
    int width, int height, int xdest, int ydest)
{
    VL_LatchToScreenScaledCoord(source,xsrc,ysrc,width,height,
        scaleFactor*xdest,scaleFactor*ydest);
}

void VL_LatchToScreenScaledCoordOrig (scrsurf *source, int scx, int scy)
{
    VL_LatchToScreenScaledCoord(source,0,0,source->width,source->height,scx,scy);
}

void VL_LatchToScreenOrig (scrsurf *source, int x, int y)
{
    VL_LatchToScreenScaledCoord(source,0,0,source->width,source->height,
        scaleFactor*x,scaleFactor*y);
}

//===========================================================================

/*
=================
=
= VL_ScreenToScreen
=
=================
*/

void VL_ScreenToScreen (scrsurf *source, scrsurf *dest)
{
    //memcpy(dest->pixels, source->pixels, sizeof(uint32_t) * source->width * source->height);
}
