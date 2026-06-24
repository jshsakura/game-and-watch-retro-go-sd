#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "WSHard.h"
#include "WS.h"
#include "WSFileio.h"
#include "WSRender.h"
#include "cpu/necintrf.h"

#include "shared.h"

static unsigned long result;
static char SaveName[512]; 
static char StateName[512];
static char IEepPath[512]; 

uint32_t WsSetPdata(void)
{
    ROMBanks = 4;
	RAMBanks = 1;
	RAMSize = 0x2000;
	CartKind = 0;
    if ((ROMMap[0xFF] = (uint8_t*)malloc(0x10000)) == NULL)
    {
		fprintf(stderr,"WsSetPdata\n");
        return 0;
    }
    WsReset();
    HVMode = 0;
    return 1;
}

uint32_t WsCreate(char *CartName)
{
    uint32_t Checksum, j;
    int32_t i;
    FILE* fp;
    char buf[16];

    for (i = 0; i < 256; i++)
    {
        ROMMap[i] = MemDummy;
        RAMMap[i] = MemDummy;
    }
    memset(IRAM, 0, sizeof(IRAM));
    memset(MemDummy, 0xA0, sizeof(MemDummy));
    memset(IO, 0, sizeof(IO));
    
    if (CartName == NULL)
    {
        return WsSetPdata();
    }
 
#ifdef ZIP_SUPPORT   
#endif
	fp = fopen(CartName, "rb");
	if (!fp)
	{
		fprintf(stderr,"ERR_FOPEN\n");
		return 1;
	}
    
    /* ws_romsize = sizeof(fp); */

    result = fseek(fp, -10, SEEK_END);
    if (fread(buf, 1, 10, fp) != 10)
    {
		fprintf(stderr,"ERR_FREAD_ROMINFO\n");
		fclose(fp);
        return 1;
    }

    switch (buf[4])
    {
    case 1:
        ROMBanks = 4;
        break;
    case 2:
        ROMBanks = 8;
        break;
    case 3:
        ROMBanks = 16;
        break;
    case 4:
        ROMBanks = 32;
        break;
    case 5:
        ROMBanks = 48;
        break;
    case 6:
        ROMBanks = 64;
        break;
    case 7:
        ROMBanks = 96;
        break;
    case 8:
        ROMBanks = 128;
        break;
    case 9:
        ROMBanks = 256;
        break;
    default:
        ROMBanks = 0;
        break;
    }
    if (ROMBanks == 0)
    {
		fprintf(stderr,"ERR_ILLEGAL_ROMSIZE\n");
        return 1;
    }
    switch (buf[5])
    {
    case 0x01:
        RAMBanks = 1;
        RAMSize = 0x2000;
        CartKind = 0;
        break;
    case 0x02:
        RAMBanks = 1;
        RAMSize = 0x8000;
        CartKind = 0;
        break;
    case 0x03:
        RAMBanks = 2;
        RAMSize = 0x20000;
        CartKind = 0;
        break;
    case 0x04:
        RAMBanks = 4;
        RAMSize = 0x40000;
        CartKind = 0;
        break;
    case 0x10:
        RAMBanks = 1;
        RAMSize = 0x80;
        CartKind = CK_EEP;
        break;
    case 0x20:
        RAMBanks = 1;
        RAMSize = 0x800;
        CartKind = CK_EEP;
        break;
    case 0x50:
        RAMBanks = 1;
        RAMSize = 0x400;
        CartKind = CK_EEP;
        break;
    default:
        RAMBanks = 1;
        RAMSize = 0x2000;
        CartKind = 0;
        break;
    }

    WsRomPatch(buf);
    
    Checksum = (uint32_t)((buf[9] << 8) + buf[8]);
    Checksum += (uint32_t)(buf[9] + buf[8]);
    for (i = ROMBanks - 1; i >= 0; i--)
    {
        fseek(fp, (ROMBanks - i) * -0x10000, 2);
        if ((ROMMap[0x100 - ROMBanks + i] = (uint8_t*)malloc(0x10000)) != NULL)
        {
            if (fread(ROMMap[0x100 - ROMBanks + i], 1, 0x10000, fp) == 0x10000)
            {
                for (j = 0; j < 0x10000; j++)
                {
                    Checksum -= ROMMap[0x100 - ROMBanks + i][j];
                }
            }
        }
        else
        {
			fprintf(stderr,"ERR_MALLOC\n");
            return 1;
        }
    }
    fclose(fp);
    if (i >= 0)
    {
        return 0;
    }
    if (Checksum & 0xFFFF)
    {
		fprintf(stderr,"ERR_CHECKSUM\n");
    }
    if (RAMBanks)
    {
        for (i = 0; i < RAMBanks; i++)
        {
            if ((RAMMap[i] = (uint8_t*)malloc(0x10000)) != NULL)
            {
                memset(RAMMap[i], 0x00, 0x10000);
            }
            else
            {
				fprintf(stderr,"ERR_MALLOC 1\n");
				return 1;
            }
        }
    }
    if (RAMSize)
    {
		char* tmp =  strstr(CartName, "/");
		if (tmp == NULL)
		{
			snprintf(SaveName, sizeof(SaveName), "%s%s%s.epm%s", PATH_DIRECTORY, SAVE_DIRECTORY, CartName, EXTENSION);
		}
		else
		{
			snprintf(SaveName, sizeof(SaveName), "%s%s%s.epm%s", PATH_DIRECTORY, SAVE_DIRECTORY, strrchr(CartName, '/')+1, EXTENSION);
		}
        
        if ((fp = fopen(SaveName, "rb")) != NULL)
        {
            for (i = 0; i < RAMBanks; i++)
            {
                if (RAMSize < 0x10000)
                {
                    if (fread(RAMMap[i], 1, RAMSize, fp) != RAMSize)
                    {
						fprintf(stderr,"ERR_FREAD_SAVE\n");
						break;
                    }
                }
                else
                {
                    if (fread(RAMMap[i], 1, 0x10000, fp) != 0x10000)
                    {
						fprintf(stderr,"ERR_FREAD_SAVE 1\n");
                        break;
                    }
                }
            }
            fclose(fp);
        }
        else
        {
			fp = fopen(SaveName, "wb");
			if (fp) fclose(fp);
		}
    }
    else
    {
        SaveName[0] = 0;
    }
    WsReset();
	HVMode = buf[6] & 1;
    
	return 1;
}

/* G&W: load the cart ROM straight from memory-mapped external flash (XIP)
 * instead of fopen()+malloc()+fread() per 64KB bank. WonderSwan ROMs reach
 * 16MB and cannot be copied into RAM, so the bank map just points into flash.
 * WS ROM is read-only (saves live in RAMMap), so XIP is safe. Mirrors
 * WsCreate's footer parse (last 10 bytes: ROM size, save type, HV mode). */
#define WS_CART_RAM_BANKS 4   /* max SRAM banks a WS cart declares (4 x 64KB) */
static uint8_t ws_cart_ram[WS_CART_RAM_BANKS * 0x10000]; /* cart SRAM/EEPROM backing */

int ws_create_from_flash(const uint8_t *data, uint32_t size)
{
    char footer[10];
    int32_t i;

    for (i = 0; i < 256; i++) {
        ROMMap[i] = MemDummy;
        RAMMap[i] = MemDummy;
    }
    memset(IRAM, 0, sizeof(IRAM));
    memset(MemDummy, 0xA0, sizeof(MemDummy));
    memset(IO, 0, sizeof(IO));

    if (data == NULL || size < 16)
        return 1;

    /* Copy the 10-byte footer out of read-only flash so WsRomPatch can poke it. */
    memcpy(footer, data + size - 10, sizeof(footer));

    switch (footer[4]) {
    case 1: ROMBanks = 4;   break;
    case 2: ROMBanks = 8;   break;
    case 3: ROMBanks = 16;  break;
    case 4: ROMBanks = 32;  break;
    case 5: ROMBanks = 48;  break;
    case 6: ROMBanks = 64;  break;
    case 7: ROMBanks = 96;  break;
    case 8: ROMBanks = 128; break;
    case 9: ROMBanks = 256; break;
    default: ROMBanks = (uint16_t)(size / 0x10000); break;
    }
    if (ROMBanks == 0)
        return 1;

    switch ((uint8_t)footer[5]) {
    case 0x01: RAMBanks = 1; RAMSize = 0x2000;  CartKind = 0;      break;
    case 0x02: RAMBanks = 1; RAMSize = 0x8000;  CartKind = 0;      break;
    case 0x03: RAMBanks = 2; RAMSize = 0x20000; CartKind = 0;      break;
    case 0x04: RAMBanks = 4; RAMSize = 0x40000; CartKind = 0;      break;
    case 0x10: RAMBanks = 1; RAMSize = 0x80;    CartKind = CK_EEP; break;
    case 0x20: RAMBanks = 1; RAMSize = 0x800;   CartKind = CK_EEP; break;
    case 0x50: RAMBanks = 1; RAMSize = 0x400;   CartKind = CK_EEP; break;
    default:   RAMBanks = 1; RAMSize = 0x2000;  CartKind = 0;      break;
    }

    WsRomPatch(footer);

    /* If the footer's size code disagrees with the actual image, trust the
     * image so the bank math can't run off either end. */
    if ((uint32_t)ROMBanks * 0x10000 > size)
        ROMBanks = (uint16_t)(size / 0x10000);
    if (ROMBanks == 0)
        return 1;

    /* Anchor banks to the END of the image (exactly like WsCreate's
     * fseek-from-SEEK_END): bank (0x100-ROMBanks+i) lives at
     * size-(ROMBanks-i)*64K. This keeps the reset vector / footer in the last
     * bank correct even if the file has leading padding or isn't an exact
     * multiple of 64KB. */
    {
        uint32_t total = (uint32_t)ROMBanks * 0x10000;
        for (i = 0; i < ROMBanks; i++)
            ROMMap[0x100 - ROMBanks + i] =
                (uint8_t *)(data + (size - total) + (uint32_t)i * 0x10000);
    }

    /* Mirror a sub-16MB cart across the whole 0x00-0xFF bank space, like the
     * real hardware: the cart's unconnected high bank-address lines make bank V
     * alias to (V mod ROMBanks). Without this, a game that selects a bank below
     * the top-anchored range (0x100-ROMBanks) reads MemDummy (0xA0) instead of
     * the aliased ROM. One Piece (8MB, ROMBanks=128) writes 0xC3 bank 0x14/0x00
     * during play; the missing mirror fed it 0xA0 garbage, and on savestate
     * resume that corrupted its control flow (BP popped to 0 -> MOV SP,BP ->
     * SP=0 -> IVT overwrite -> crash). Only mirror power-of-two cart sizes,
     * where the aliasing is exact; odd sizes (48/96 banks) are left as-is. */
    if (ROMBanks > 0 && ROMBanks < 0x100 &&
        (ROMBanks & (ROMBanks - 1)) == 0) {
        int v, lo = 0x100 - ROMBanks;
        for (v = 0; v < lo; v++)
            ROMMap[v] = ROMMap[lo + (v & (ROMBanks - 1))];
    }

    /* Cart save RAM, backed by one static buffer covering every bank the cart
     * declares (up to 4 x 64KB). Map each bank to its slice so multi-bank SRAM
     * games keep real, contiguous storage instead of aliasing MemDummy. */
    memset(ws_cart_ram, 0, sizeof(ws_cart_ram));
    {
        uint32_t b;
        uint32_t banks = RAMBanks;
        if (banks > WS_CART_RAM_BANKS) banks = WS_CART_RAM_BANKS;
        for (b = 0; b < banks; b++)
            RAMMap[b] = ws_cart_ram + b * 0x10000;
        /* Mirror the SRAM banks across the C1-reachable range (0..7) like the
         * hardware: a cart with fewer than 8 SRAM banks aliases bank V to
         * (V mod banks). Without this, IO[0xC1]=non-zero on a 1-bank cart maps
         * Page[1] to MemDummy (0xA0) -> the game reads garbage SRAM. (Parallels
         * the ROM bank mirror above.) */
        if (banks > 0)
            for (b = banks; b < 8; b++)
                RAMMap[b] = RAMMap[b % banks];
    }

    SaveName[0] = 0;
    HVMode = footer[6] & 1;
    return 1;
}

void WsRelease(void)
{
    FILE* fp;
    uint32_t i;

    if (SaveName[0] != 0)
    {
        if ((fp = fopen(SaveName, "wb"))!= NULL)
        {
            for (i  = 0; i < RAMBanks; i++)
            {
                if (RAMSize<0x10000)
                {
                    if (fwrite(RAMMap[i], 1, RAMSize, fp) != RAMSize)
                    {
                        break;
                    }
                }
                else
                {
                    if (fwrite(RAMMap[i], 1, 0x10000, fp)!=0x10000)
                    {
                        break;
                    }
                }
                free(RAMMap[i]);
                RAMMap[i] = NULL;
            }
            fclose(fp);
        }
        SaveName[0] = '\0';
    }
    for (i = 0xFF; i; i--)
    {
        if (ROMMap[i] == MemDummy)
        {
            break;
        }
        free(ROMMap[i]);
        ROMMap[i] = MemDummy;
    }
    StateName[0] = '\0';
}

void WsLoadEeprom(void)
{
    FILE* fp;

	snprintf(IEepPath, sizeof(IEepPath), "%s%s%s.epm%s", PATH_DIRECTORY, SAVE_DIRECTORY, strrchr(gameName, '/')+1, EXTENSION);

    if ((fp = fopen(IEepPath, "rb")) != NULL)
    {
        result = fread(IEep, sizeof(uint16_t), 64, fp);
        fclose(fp);
    }
	else
	{
		uint16_t* p = IEep + 0x30;
		memset(IEep, 0xFF, 0x60);
		memset(p, 0, 0x20);
		*p++ = 0x211D;
		*p++ = 0x180B;
		*p++ = 0x1C0D;
		*p++ = 0x1D23;
		*p++ = 0x0B1E;
		*p   = 0x0016;
	}
}

void WsSaveEeprom(void)
{
    FILE* fp;

	snprintf(IEepPath, sizeof(IEepPath), "%s%s%s.epm%s", PATH_DIRECTORY, SAVE_DIRECTORY, strrchr(gameName, '/')+1, EXTENSION);

    if ((fp = fopen(IEepPath, "wb")) != NULL)
    {
        fwrite(IEep, sizeof(uint16_t), 64, fp);
		fclose(fp);
    }
}

#define MacroLoadNecRegisterFromFile(F,R) \
		result = fread(&value, sizeof(uint32_t), 1, fp); \
	    nec_set_reg(R,value); 
uint32_t WsLoadState(const char *savename, uint32_t num)
{
    FILE* fp;
    char buf[PATH_MAX];
	uint32_t value;
	uint32_t i;
	
	snprintf(buf, sizeof(buf), "%s%s%s.%u.sta%s", PATH_DIRECTORY, SAVE_DIRECTORY, strrchr(savename,'/')+1, num, EXTENSION);
	fp = fopen(buf, "rb");
    if (!fp)
    {
		printf("Cannot load save state\n");
		return 1;
	}
	MacroLoadNecRegisterFromFile(fp,NEC_IP);
	MacroLoadNecRegisterFromFile(fp,NEC_AW);
	MacroLoadNecRegisterFromFile(fp,NEC_BW);
	MacroLoadNecRegisterFromFile(fp,NEC_CW);
	MacroLoadNecRegisterFromFile(fp,NEC_DW);
	MacroLoadNecRegisterFromFile(fp,NEC_CS);
	MacroLoadNecRegisterFromFile(fp,NEC_DS);
	MacroLoadNecRegisterFromFile(fp,NEC_ES);
	MacroLoadNecRegisterFromFile(fp,NEC_SS);
	MacroLoadNecRegisterFromFile(fp,NEC_IX);
	MacroLoadNecRegisterFromFile(fp,NEC_IY);
	MacroLoadNecRegisterFromFile(fp,NEC_BP);
	MacroLoadNecRegisterFromFile(fp,NEC_SP);
	MacroLoadNecRegisterFromFile(fp,NEC_FLAGS);
	MacroLoadNecRegisterFromFile(fp,NEC_VECTOR);
	MacroLoadNecRegisterFromFile(fp,NEC_PENDING);
	MacroLoadNecRegisterFromFile(fp,NEC_NMI_STATE);
	MacroLoadNecRegisterFromFile(fp,NEC_IRQ_STATE);
    result = fread(IRAM, sizeof(uint8_t), 0x10000, fp);
    result = fread(IO, sizeof(uint8_t), 0x100, fp);
    for (i = 0; i < RAMBanks; i++)
    {
        if (RAMSize < 0x10000)
        {
            result = fread(RAMMap[i], 1, RAMSize, fp);
        }
        else
        {
            result = fread(RAMMap[i], 1, 0x10000, fp);
        }
    }
	result = fread(Palette, sizeof(uint16_t), 16 * 16, fp);
    fclose(fp);
	WriteIO(0xC1, IO[0xC1]);
	WriteIO(0xC2, IO[0xC2]);
	WriteIO(0xC3, IO[0xC3]);
	{ /* G&W: restoring state must NOT run the emulated CPU. WriteIO(0xC0) calls
         nec_execute(1) when CS>=0x4000 (e.g. One Piece 8MB), executing a
         garbage opcode before Page[] is mapped -> null NEC handler -> crash.
         Force CS=0 so that branch is skipped; Page[] is still mapped. */
        uint32_t _saved_cs = nec_get_reg(NEC_CS); nec_set_reg(NEC_CS, 0);
        WriteIO(0xC0, IO[0xC0]);
        nec_set_reg(NEC_CS, _saved_cs); }
	for (i = 0x80; i <= 0x90; i++)
	{
		WriteIO(i, IO[i]);
	}
	
    return 0;
}

#define MacroStoreNecRegisterToFile(F,R) \
	    value = nec_get_reg(R); \
		fwrite(&value, sizeof(uint32_t), 1, fp);
		
uint32_t WsSaveState(const char *savename, uint32_t num)
{
    FILE* fp;
    char buf[PATH_MAX];
	uint32_t value;
	uint32_t i;
	
	snprintf(buf, sizeof(buf), "%s%s%s.%u.sta%s", PATH_DIRECTORY, SAVE_DIRECTORY, strrchr(savename,'/')+1, num, EXTENSION);
    if ((fp = fopen(buf, "w+")) == NULL)
    {
		printf("Failed to save\n");
		return 1;
	}
	MacroStoreNecRegisterToFile(fp,NEC_IP);
	MacroStoreNecRegisterToFile(fp,NEC_AW);
	MacroStoreNecRegisterToFile(fp,NEC_BW);
	MacroStoreNecRegisterToFile(fp,NEC_CW);
	MacroStoreNecRegisterToFile(fp,NEC_DW);
	MacroStoreNecRegisterToFile(fp,NEC_CS);
	MacroStoreNecRegisterToFile(fp,NEC_DS);
	MacroStoreNecRegisterToFile(fp,NEC_ES);
	MacroStoreNecRegisterToFile(fp,NEC_SS);
	MacroStoreNecRegisterToFile(fp,NEC_IX);
	MacroStoreNecRegisterToFile(fp,NEC_IY);
	MacroStoreNecRegisterToFile(fp,NEC_BP);
	MacroStoreNecRegisterToFile(fp,NEC_SP);
	MacroStoreNecRegisterToFile(fp,NEC_FLAGS);
	MacroStoreNecRegisterToFile(fp,NEC_VECTOR);
	MacroStoreNecRegisterToFile(fp,NEC_PENDING);
	MacroStoreNecRegisterToFile(fp,NEC_NMI_STATE);
	MacroStoreNecRegisterToFile(fp,NEC_IRQ_STATE);
    fwrite(IRAM, sizeof(uint8_t), 0x10000, fp);
    fwrite(IO, sizeof(uint8_t), 0x100, fp);
    for (i = 0; i < RAMBanks; i++)
    {
        if (RAMSize < 0x10000)
        {
            fwrite(RAMMap[i], 1, RAMSize, fp);
        }
        else
        {
            fwrite(RAMMap[i], 1, 0x10000, fp);
        }
    }
	fwrite(Palette, sizeof(uint16_t), 16 * 16, fp);
    fclose(fp);

    return 0;
}

/* --- Memory-based savestate (G&W front-end has no writable FILE storage) ---
 * Mirrors WsSaveState/WsLoadState exactly, but to/from a caller buffer. The
 * ROM stays in flash (XIP) and is never part of the snapshot. */

static const int ws_state_nec_regs[] = {
    NEC_IP, NEC_AW, NEC_BW, NEC_CW, NEC_DW, NEC_CS, NEC_DS, NEC_ES, NEC_SS,
    NEC_IX, NEC_IY, NEC_BP, NEC_SP, NEC_FLAGS, NEC_VECTOR, NEC_PENDING,
    NEC_NMI_STATE, NEC_IRQ_STATE,
};
#define WS_STATE_NEC_COUNT ((int)(sizeof(ws_state_nec_regs)/sizeof(ws_state_nec_regs[0])))

/* Write the full machine state straight to an open file. No intermediate
 * buffer, so large multi-bank SRAM games can't overflow a fixed scratch (that
 * silently dropped saves and left load reading stale data -> corrupt screen). */
/* Header so a save from an incompatible build (or a stale/short file) is
 * rejected on load instead of being applied as garbage -> corrupt screen. */
#define WS_STATE_MAGIC   0x53575347u   /* 'GSWS' */
#define WS_STATE_VERSION 1u

uint32_t WsSaveStateToFile(FILE *fp)
{
    uint32_t i;
    uint32_t bank = (RAMSize < 0x10000) ? RAMSize : 0x10000;
    uint32_t hdr[4];
    if (!fp) return 1;
    hdr[0] = WS_STATE_MAGIC;
    hdr[1] = WS_STATE_VERSION;
    hdr[2] = RAMBanks;
    hdr[3] = RAMSize;
    if (fwrite(hdr, sizeof(uint32_t), 4, fp) != 4) return 1;
    for (i = 0; i < (uint32_t)WS_STATE_NEC_COUNT; i++) {
        uint32_t v = nec_get_reg(ws_state_nec_regs[i]);
        if (fwrite(&v, sizeof(uint32_t), 1, fp) != 1) return 1;
    }
    fwrite(IRAM, 1, 0x10000, fp);
    fwrite(IO,   1, 0x100,   fp);
    for (i = 0; i < RAMBanks; i++) fwrite(RAMMap[i], 1, bank, fp);
    fwrite(Palette, sizeof(uint16_t), 16 * 16, fp);
    return 0;
}

/* Resume-time stack/BP-chain snapshot, surfaced in the freeze panel (logbuf
 * truncates the early WSLD lines off the SD file). */
unsigned char g_resume_stk[64];
unsigned int  g_resume_base;
unsigned int  g_resume_csip;   /* CS:IP at resume (surfaced in panel) */
unsigned int  g_resume_sssp;   /* SS:SP at resume */
unsigned int  g_resume_dses;   /* DS:ES at resume */

uint32_t WsLoadStateFromFile(FILE *fp)
{
    uint32_t i;
    uint32_t bank = (RAMSize < 0x10000) ? RAMSize : 0x10000;
    uint32_t hdr[4];
    if (!fp) return 1;
    /* Reject saves from another build / wrong cart layout / short files. */
    if (fread(hdr, sizeof(uint32_t), 4, fp) != 4) return 1;
    if (hdr[0] != WS_STATE_MAGIC || hdr[1] != WS_STATE_VERSION) return 1;
    if (hdr[2] != RAMBanks || hdr[3] != RAMSize) return 1;
    printf("WSLD: hdr ok RAMBanks=%lu RAMSize=%lx bank=%lx\n",
           (unsigned long)RAMBanks, (unsigned long)RAMSize, (unsigned long)bank);
    for (i = 0; i < (uint32_t)WS_STATE_NEC_COUNT; i++) {
        uint32_t v;
        if (fread(&v, sizeof(uint32_t), 1, fp) != 1) return 1;
        nec_set_reg(ws_state_nec_regs[i], v);
    }
    printf("WSLD: nec regs done\n");
    if (fread(IRAM, 1, 0x10000, fp) != 0x10000) return 1;
    if (fread(IO,   1, 0x100,   fp) != 0x100)   return 1;
    printf("WSLD: iram+io done\n");
    for (i = 0; i < RAMBanks; i++) {
        if (RAMMap[i] == NULL) { printf("WSLD: RAMMap[%lu] NULL, skip\n", (unsigned long)i); continue; }
        fread(RAMMap[i], 1, bank, fp);
    }
    fread(Palette, sizeof(uint16_t), 16 * 16, fp);
    printf("WSLD: rammap+palette done\n");

    /* Rebuild derived state that WriteIO caches outside IO[]. The display
     * registers 0x00-0x3F are critical: WriteIO(0x07) recomputes Scr1TMap /
     * Scr2TMap (the BG/FG tilemap base pointers) and 0x1C-0x3F rebuild the
     * palette - without replaying them the tilemap base stays stale and the
     * whole screen renders garbled. 0x00-0x3F are pure config (no DMA/sound
     * trigger lives below 0x40), so replaying them is side-effect-safe. */
    /* Display config replay (our addition): rebuild the tilemap base pointers so
     * the screen isn't garbled. No nec_execute / DMA below 0x40, so safe. */
    for (i = 0x00; i <= 0x3F; i++)
        WriteIO(i, IO[i]);
    printf("WSLD: writeio 00-3F done\n");
    /* Bank replay -- EXACTLY mirror the stock WsLoadState (C1,C2,C3,C0,80-90)
     * with the REAL CS. WriteIO(0xC0) deliberately runs nec_execute(1) at
     * CS>=0x4000: that completes the in-flight bank-switch the save interrupted,
     * which the resume needs. We previously forced CS=0 to dodge a HardFault,
     * but that was the NULL V30 opcode slots (0x0F/0x64/0x65), now implemented --
     * so forcing CS=0 only SKIPPED that instruction and left the bank/CPU state
     * inconsistent, which is what corrupted One Piece's savestate resume. */
    WriteIO(0xC1, IO[0xC1]);
    /* C1>=8 hits WriteIO's WonderWitch->MemDummy branch, but a real-SRAM cart
     * aliases the bank to a valid one (One Piece leaves C1=0xFF at save time).
     * Point Page[1] back at the mirrored real SRAM so resume reads valid data
     * instead of 0xA0 garbage (which corrupted return addresses -> wrong jumps). */
    { extern uint8_t *Page[16];
      int fixed = 0;
      if (RAMBanks > 0 && RAMMap[0] != MemDummy && Page[1] == MemDummy) {
          Page[1] = RAMMap[IO[0xC1] % RAMBanks];
          fixed = 1;
      }
      WriteIO(0xC2, IO[0xC2]);
      WriteIO(0xC3, IO[0xC3]);
      printf("WSLD: writeio C1-C3 done (C1=%02X page1=%s%s)\n",
             IO[0xC1], (Page[1] == MemDummy) ? "dummy" : "ram",
             fixed ? " [remapped]" : ""); }
    /* Skip WriteIO(0xC0)'s bank-delay nec_execute(1) during resume: it models
     * the 1-instruction delay after a LIVE 'OUT 0xC0', but on resume there is no
     * in-flight OUT -- it would run the resumed first instruction (e.g. B978:0D85
     * 'PUSH DI; CALL FAR ES:[DI]') fetched from the not-yet-mapped (stale) bank,
     * decoding a garbage opcode that corrupts a function pointer -> far-call into
     * data. Force CS=0 so the branch is skipped; the banks are still mapped, and
     * the real first instruction then runs in the main loop with the correct bank. */
    { uint32_t _scs = nec_get_reg(NEC_CS);
      nec_set_reg(NEC_CS, 0);
      WriteIO(0xC0, IO[0xC0]);
      nec_set_reg(NEC_CS, _scs); }
    printf("WSLD: writeio C0 done\n");
    for (i = 0x80; i <= 0x90; i++)
        WriteIO(i, IO[i]);
    printf("WSLD: writeio 80-90 done\n");
    /* Restart the HBlank/VBlank timer countdowns. HTimer/VTimer are static in
     * WS.c -- NOT saved and NOT in the replay set -- so on resume they stay
     * dead/stale: a timer the game enabled never counts down, its IRQ never
     * fires, and a timer-driven counter the game's logic waits on never advances
     * -> it spins / recurses forever (exactly the B978:30xx recursion we see).
     * Replay the timer preset + control regs so WriteIO reloads HTimer/VTimer
     * from the restored HPRE/VPRE and re-enables them. */
    WriteIO(0xA4, IO[0xA4]);   /* HPRE lo -> HTimer */
    WriteIO(0xA5, IO[0xA5]);   /* HPRE hi -> HTimer */
    WriteIO(0xA6, IO[0xA6]);   /* VPRE lo -> VTimer */
    WriteIO(0xA7, IO[0xA7]);   /* VPRE hi -> VTimer */
    WriteIO(0xA2, IO[0xA2]);   /* TIMCTL -> enable + reload both */
    printf("WSLD: writeio timers A2-A7 done (TIMCTL=%02X HPRE=%02X%02X VPRE=%02X%02X)\n",
           IO[0xA2], IO[0xA5], IO[0xA4], IO[0xA7], IO[0xA6]);
    printf("WSLD: resume CS=%lx IP=%lx SS=%lx SP=%lx BP=%lx DS=%lx ES=%lx PEND=%lx\n",
           (unsigned long)nec_get_reg(NEC_CS), (unsigned long)nec_get_reg(NEC_IP),
           (unsigned long)nec_get_reg(NEC_SS), (unsigned long)nec_get_reg(NEC_SP),
           (unsigned long)nec_get_reg(NEC_BP),
           (unsigned long)nec_get_reg(NEC_DS), (unsigned long)nec_get_reg(NEC_ES),
           (unsigned long)nec_get_reg(NEC_PENDING));
    /* Dump the stack/BP-chain region AT RESUME. If [BP]=[0x1FFA] is already 0
     * here, the saved state itself has a broken chain (save-side problem); if it
     * is a valid outer-frame pointer now but 0 by the crash, something overwrites
     * it during the first frames (runtime corruption). */
    { extern unsigned char g_resume_stk[64];
      extern unsigned int  g_resume_base, g_resume_csip, g_resume_sssp, g_resume_dses;
      uint16_t ss = (uint16_t)nec_get_reg(NEC_SS);
      uint16_t bp = (uint16_t)nec_get_reg(NEC_BP);
      uint16_t lo = (uint16_t)(bp - 0x12);
      uint32_t base = ((uint32_t)ss << 4) + lo;
      g_resume_csip = ((unsigned int)(uint16_t)nec_get_reg(NEC_CS) << 16) | (uint16_t)nec_get_reg(NEC_IP);
      g_resume_sssp = ((unsigned int)ss << 16) | (uint16_t)nec_get_reg(NEC_SP);
      g_resume_dses = ((unsigned int)(uint16_t)nec_get_reg(NEC_DS) << 16) | (uint16_t)nec_get_reg(NEC_ES);
      char b2[140]; int q, m = 0;
      g_resume_base = ((unsigned int)ss << 16) | lo;
      for (q = 0; q < 64; q++) {
          g_resume_stk[q] = (unsigned char)ReadMem(base + q);
          m += snprintf(b2 + m, sizeof(b2) - m, "%02X", g_resume_stk[q]);
      }
      printf("WSLD: stk@BP-12 (%04X:%04X)=%s\n", ss, lo, b2); }
    printf("WSLD: complete\n");
    return 0;
}

/* GNW diagnostic. Called once per emulated frame from main_wswan.c. One Piece
 * (WSC) plays fine from a cold boot but its savestate RESUME runs the game with
 * corrupted output (graphics degrade) -- it is NOT frozen, it's executing, so a
 * freeze detector never fires. The likely cause is a mis-implemented V30 0x0F
 * sub-opcode (the BCD ADD4S/SUB4S/CMP4S macros were never exercised before).
 * So instead of waiting for a freeze, ~3s after launch we UNCONDITIONALLY pop
 * the on-screen panel and dump which 0x0F sub-opcodes (and REPC/REPNC) the game
 * actually executed, plus CS:IP + the bytes there. That pins down exactly which
 * op to re-verify against MAME. One-shot per launch. */
void ws_freeze_check(void)
{
    extern void gw_debug_show_log(const char *banner);
    extern unsigned short g_v30op[256];
    extern unsigned int   g_repc_n, g_repnc_n;
    static int frame = 0, shown = 0;
    uint16_t cs, ip;
    char buf[260];
    int i, n;

    if (shown) return;

    cs = (uint16_t)nec_get_reg(NEC_CS);
    ip = (uint16_t)nec_get_reg(NEC_IP);

    /* Fire the instant the stack-runaway ring is frozen (the real target), or
     * when the CPU has already fallen into low IRAM, or after ~6s. */
    {
        extern unsigned char g_runaway_caught, g_b436_caught;
        /* Also fire once B978:436D was reached (cold-boot OR resume) so its full
         * state can be compared. */
        if (!(g_runaway_caught || cs < 0x100 || g_b436_caught || ++frame >= 360)) return;
    }
    shown = 1;

    /* Clear the 4KB printf ring so the whole dump below is written contiguously
     * from index 0 -- without this the boot/WSLD/WSf lines push the dump past
     * 4KB and it wraps, mangling the copy fwrite()n to /ws_debug.txt. The dump
     * (~3KB) fits in one pass, so the SD file then holds it complete and in
     * order. Every datum we need is reprinted in the dump below. */
    { extern uint32_t log_idx; extern char logbuf[]; log_idx = 0; logbuf[0] = '\0'; }

    /* The runaway loop body: last 8 (CS:IP) executed before SP hit 0x0400. */
    {
        extern unsigned int   g_csip_ring[16];
        extern unsigned short g_sp_ring[16];
        extern unsigned short g_bp_ring[16];
        extern unsigned char g_ring_pos;
        int k;
        unsigned int a_old = g_csip_ring[g_ring_pos & 15];
        unsigned int a_new = g_csip_ring[(g_ring_pos + 15) & 15];
        n = 0;
        for (k = 0; k < 16; k++) {
            unsigned int v = g_csip_ring[(g_ring_pos + k) & 15];
            n += snprintf(buf + n, sizeof(buf) - n, "%04X:%04X ",
                          (v >> 16) & 0xFFFF, v & 0xFFFF);
        }
        printf("WSRING: %s\n", buf);
        /* Full-state snapshot at the first B978:436D visit -- compare cold-boot
         * vs resume to find the root inconsistency (IVT/regs/stack). */
        { extern unsigned char  g_b436_caught, g_b436_ivt[16], g_b436_stk[24];
          extern unsigned short g_b436_regs[8], g_b436_segs[4];
          printf("WSCMP: caught=%u CS=%04X DS=%04X ES=%04X SS=%04X  AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X\n",
                 g_b436_caught, g_b436_segs[0], g_b436_segs[1], g_b436_segs[2], g_b436_segs[3],
                 g_b436_regs[0], g_b436_regs[1], g_b436_regs[2], g_b436_regs[3],
                 g_b436_regs[4], g_b436_regs[5], g_b436_regs[6], g_b436_regs[7]);
          n = 0; for (k = 0; k < 16; k++)
              n += snprintf(buf + n, sizeof(buf) - n, "%02X", g_b436_ivt[k]);
          printf("WSCMP: IVT0-3=%s\n", buf);
          n = 0; for (k = 0; k < 24; k++)
              n += snprintf(buf + n, sizeof(buf) - n, "%02X", g_b436_stk[k]);
          printf("WSCMP: stk@SP=%s\n", buf); }
        /* Where IVT[1] (the INT 1 handler) was first installed + with what value
         * -- traces the garbage 0007:0085 to its source. */
        { extern unsigned char g_ivt1_caught;
          extern unsigned int  g_ivt1_csip, g_ivt1_val, g_ivt1_ring[8];
          printf("WSIVT1: caught=%u at=%04X:%04X val=%04X:%04X\n",
                 g_ivt1_caught, g_ivt1_csip>>16, g_ivt1_csip&0xFFFF,
                 g_ivt1_val>>16, g_ivt1_val&0xFFFF);
          n = 0; for (k = 0; k < 8; k++)
              n += snprintf(buf + n, sizeof(buf) - n, "%04X:%04X ",
                            g_ivt1_ring[k]>>16, g_ivt1_ring[k]&0xFFFF);
          printf("WSIVT1: ring=%s\n", buf); }
        /* Far-transfer trail: last 32 CALL FAR(C)/RETF(R)/INT(I)/IRET(T)/JMPF(J)/
         * HWINT(H) with SP-after -- trace the call nesting to where an extra word
         * leaks onto the stack (the +2 that crashes the A068 RETF). Oldest first. */
        { extern unsigned int g_far_csip[32], g_far_meta[32];
          extern unsigned char g_far_pos;
          static const char tc[7] = "?CRITJH";
          int row; for (row = 0; row < 4; row++) {
              n = 0;
              for (k = 0; k < 8; k++) {
                  unsigned char idx = (unsigned char)((g_far_pos + row * 8 + k) & 31);
                  unsigned int  m = g_far_meta[idx], c = g_far_csip[idx];
                  unsigned char t = (m >> 24) & 0xFF;
                  n += snprintf(buf + n, sizeof(buf) - n, "%c%04X:%04X/%04X ",
                                (t <= 6) ? tc[t] : '?', c >> 16, c & 0xFFFF, m & 0xFFFF);
              }
              printf("WSFAR%d: %s\n", row, buf);
          } }
        /* SP/BP trajectory aligned with WSRING -- shows whether SP marched down
         * gradually (deep recursion) or dropped in one step (a MOV SP,BP with a
         * corrupt BP). Oldest->newest, same order as WSRING. */
        n = 0;
        for (k = 0; k < 16; k++)
            n += snprintf(buf + n, sizeof(buf) - n, "%04X/%04X ",
                          g_sp_ring[(g_ring_pos + k) & 15],
                          g_bp_ring[(g_ring_pos + k) & 15]);
        printf("WSTRAJ(SP/BP): %s\n", buf);
        /* Stack snapshot captured AT the runaway (frames intact). */
        {
            extern unsigned int  g_runaway_sp;
            extern unsigned char g_runaway_stack[48];
            n = 0;
            for (k = 0; k < 40; k++)
                n += snprintf(buf + n, sizeof(buf) - n, "%02X", g_runaway_stack[k]);
            printf("WSRSTK: SS:SP=%04X:%04X %s\n",
                   (g_runaway_sp >> 16) & 0xFFFF, g_runaway_sp & 0xFFFF, buf);
        }
    }

    {
        uint16_t ss = (uint16_t)nec_get_reg(NEC_SS);
        uint16_t sp = (uint16_t)nec_get_reg(NEC_SP);
        uint32_t sbase = ((uint32_t)ss << 4) + sp;
        { extern unsigned int g_int_n, g_iret_n;
          printf("WSBAD: CS:IP=%04X:%04X SS:SP=%04X:%04X INT=%u IRET=%u REPC=%u\n",
                 cs, ip, ss, sp, g_int_n, g_iret_n, g_repc_n); }
        /* Stack from SP upward = the recursion frames pushed so far. Caught
         * early in the descent, repeated return CS:IP pairs here reveal the
         * recursive function. 40 bytes = ~10 words ~= 5 far-call frames. */
        n = 0;
        for (i = 0; i < 40; i++)
            n += snprintf(buf + n, sizeof(buf) - n, "%02X", ReadMem(sbase + i));
        printf("WSBAD: stack@SP=%s\n", buf);
        /* IRQ state + IVT base region (vectors the WS IRQs index into) */
        printf("WSBAD: IRQENA=%02X IRQBSE=%02X IRQVEC=%02X PEND=%02lX\n",
               IO[0xB2], IO[0xB0], IO[0xB4], (unsigned long)nec_get_reg(NEC_PENDING));
        n = 0;
        for (i = 0; i < 16; i++)              /* IVT vectors 0..3 (16 bytes) */
            n += snprintf(buf + n, sizeof(buf) - n, "%02X", ReadMem(i));
        printf("WSBAD: IVT0-3=%s\n", buf);
        /* IVT entry the IRQ base points at */
        { uint32_t v = ((uint32_t)IO[0xB0]) << 2; n = 0;
          for (i = 0; i < 16; i++)
              n += snprintf(buf + n, sizeof(buf) - n, "%02X", ReadMem(v + i));
          printf("WSBAD: IVT@IRQBSE(%lx)=%s\n", (unsigned long)v, buf); }
        /* The instruction that drove BP nonzero->0 (root of the SP collapse) +
         * ROM around it, dumped from -0x08 so we see the full instruction. */
        { extern unsigned int   g_bpz_n, g_bpz_at, g_bpz_by;
          extern unsigned short g_bpz_sp;
          extern unsigned char  g_bpz_rom[24];
          extern unsigned char  g_bpz_stk[16];
          n = 0; for (i = 0; i < 24; i++)
              n += snprintf(buf + n, sizeof(buf) - n, "%02X", g_bpz_rom[i]);
          printf("WSBPZ: n=%u by=%04X:%04X at=%04X:%04X SP=%04X rom@-8=%s\n",
                 g_bpz_n, (g_bpz_by >> 16) & 0xFFFF, g_bpz_by & 0xFFFF,
                 (g_bpz_at >> 16) & 0xFFFF, g_bpz_at & 0xFFFF, g_bpz_sp, buf);
          n = 0; for (i = 0; i < 16; i++)
              n += snprintf(buf + n, sizeof(buf) - n, "%02X", g_bpz_stk[i]);
          printf("WSBPZ: stk@SP=%s\n", buf);
          /* Resume-time BP-chain (captured at load) -- in the panel so it can't
           * be truncated off the SD file. base = BP-0x12, so [BP]=[0x1FFA] is at
           * offset 0x12 (bytes 18-19). 0 there => save-side broken chain. */
          { extern unsigned char g_resume_stk[64];
            extern unsigned int  g_resume_base;
            n = 0; for (i = 0; i < 64; i++)
                n += snprintf(buf + n, sizeof(buf) - n, "%02X", g_resume_stk[i]);
            printf("WSRSM: stk@%04X:%04X(+12=[BP])=%s\n",
                   (g_resume_base >> 16) & 0xFFFF, g_resume_base & 0xFFFF, buf); }
          /* Resume regs, surfaced in the panel (the WSLD line truncates off). */
          { extern unsigned int g_resume_csip, g_resume_sssp, g_resume_dses;
            printf("WSRSM: resume CS:IP=%04X:%04X SS:SP=%04X:%04X DS:ES=%04X:%04X\n",
                   (g_resume_csip >> 16) & 0xFFFF, g_resume_csip & 0xFFFF,
                   (g_resume_sssp >> 16) & 0xFFFF, g_resume_sssp & 0xFFFF,
                   (g_resume_dses >> 16) & 0xFFFF, g_resume_dses & 0xFFFF);
            printf("WSRSM: banks C0=%02X C1=%02X C2=%02X C3=%02X RAMBanks=%lu RAMSize=%lx\n",
                   IO[0xC0], IO[0xC1], IO[0xC2], IO[0xC3],
                   (unsigned long)RAMBanks, (unsigned long)RAMSize);
            printf("WSRSM: regs AX=%04lX BX=%04lX CX=%04lX DX=%04lX SI=%04lX DI=%04lX FLAGS=%04lX\n",
                   (unsigned long)nec_get_reg(NEC_AW), (unsigned long)nec_get_reg(NEC_BW),
                   (unsigned long)nec_get_reg(NEC_CW), (unsigned long)nec_get_reg(NEC_DW),
                   (unsigned long)nec_get_reg(NEC_IX), (unsigned long)nec_get_reg(NEC_IY),
                   (unsigned long)nec_get_reg(NEC_FLAGS)); }
          /* Bank-switch (OUT 0xC0-0xC3) history, last 16 oldest->newest, each
           * CS:IP=port:val. Placed in the late (retained) block so it survives the
           * logbuf truncation. A code-bank (0xC0) switch right before the crash,
           * or a switch to bank 0 (MemDummy), confirms the bank-timing divergence. */
          { extern unsigned int   g_bnk_ring[16];
            extern unsigned short g_bnk_meta[16];
            extern unsigned char  g_bnk_pos;
            n = 0;
            for (i = 0; i < 16; i++) {
                unsigned int  v = g_bnk_ring[(g_bnk_pos + i) & 15];
                unsigned short mt = g_bnk_meta[(g_bnk_pos + i) & 15];
                n += snprintf(buf + n, sizeof(buf) - n, "%04X:%04X=%02X:%02X ",
                              (v >> 16) & 0xFFFF, v & 0xFFFF,
                              (mt >> 8) & 0xFF, mt & 0xFF);
            }
            printf("WSBNK: %s\n", buf); }
          /* The wrong jump that lands in the A068:0Cxx data table + the regs. */
          { extern unsigned char g_jmp_caught, g_jmp_rom[24];
            extern unsigned int   g_jmp_from, g_jmp_to, g_jmp_dses;
            extern unsigned short g_jmp_regs[8];
            n = 0; for (i = 0; i < 24; i++)
                n += snprintf(buf + n, sizeof(buf) - n, "%02X", g_jmp_rom[i]);
            printf("WSJMP: c=%u from=%04X:%04X to=%04X:%04X rom@-8=%s\n",
                   g_jmp_caught, (g_jmp_from >> 16) & 0xFFFF, g_jmp_from & 0xFFFF,
                   (g_jmp_to >> 16) & 0xFFFF, g_jmp_to & 0xFFFF, buf);
            printf("WSJMP: AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X DS=%04X ES=%04X\n",
                   g_jmp_regs[0], g_jmp_regs[1], g_jmp_regs[2], g_jmp_regs[3],
                   g_jmp_regs[4], g_jmp_regs[5], g_jmp_regs[6], g_jmp_regs[7],
                   (g_jmp_dses >> 16) & 0xFFFF, g_jmp_dses & 0xFFFF);
            { extern unsigned char g_jmp_stk[16];
              n = 0; for (i = 0; i < 16; i++)
                  n += snprintf(buf + n, sizeof(buf) - n, "%02X", g_jmp_stk[i]);
              printf("WSJMP: stk@SP-8=%s\n", buf); }
            { extern unsigned int g_nullint_n, g_nullint_last;
              printf("WSJMP: nullINT n=%u last=%u\n", g_nullint_n, g_nullint_last); } }
          printf("WSSER: IO B0=%02X B1=%02X B2=%02X B3=%02X B4=%02X B5=%02X B6=%02X\n",
                 IO[0xB0], IO[0xB1], IO[0xB2], IO[0xB3], IO[0xB4], IO[0xB5], IO[0xB6]); }
    }

    /* Dump the whole captured log to the SD card so the user can read it off
     * the card instead of photographing the screen. */
    {
        extern char logbuf[];
        FILE *lf = fopen("/ws_debug.txt", "w");
        if (lf) {
            fwrite(logbuf, 1, strlen(logbuf), lf);
            fclose(lf);
            printf("WSLOG: wrote /ws_debug.txt\n");
        } else {
            printf("WSLOG: fopen /ws_debug.txt FAILED\n");
        }
    }

    snprintf(buf, sizeof(buf), "WS BAD-JUMP @ %04X:%04X (saved /ws_debug.txt)", cs, ip);
    gw_debug_show_log(buf);
}


