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
    { uint32_t _saved_cs = nec_get_reg(NEC_CS); nec_set_reg(NEC_CS, 0);
      for (i = 0x00; i <= 0x3F; i++)
          WriteIO(i, IO[i]);
      printf("WSLD: writeio 00-3F done\n");
      WriteIO(0xC1, IO[0xC1]);
      WriteIO(0xC2, IO[0xC2]);
      WriteIO(0xC3, IO[0xC3]);
      printf("WSLD: writeio C1-C3 done\n");
      /* WriteIO(0xC0) calls nec_execute(1) when CS>=0x4000; CS is forced 0
       * above so that branch (and the whole replay) can't run the emulated
       * CPU before Page[] is mapped -> null NEC handler -> crash. */
      WriteIO(0xC0, IO[0xC0]);
      printf("WSLD: writeio C0 done\n");
      for (i = 0x80; i <= 0x90; i++)
          WriteIO(i, IO[i]);
      printf("WSLD: writeio 80-90 done\n");
      nec_set_reg(NEC_CS, _saved_cs); }
    printf("WSLD: resume CS=%lx IP=%lx SS=%lx SP=%lx DS=%lx ES=%lx PEND=%lx\n",
           (unsigned long)nec_get_reg(NEC_CS), (unsigned long)nec_get_reg(NEC_IP),
           (unsigned long)nec_get_reg(NEC_SS), (unsigned long)nec_get_reg(NEC_SP),
           (unsigned long)nec_get_reg(NEC_DS), (unsigned long)nec_get_reg(NEC_ES),
           (unsigned long)nec_get_reg(NEC_PENDING));
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
    char buf[96];
    int i, n;

    if (shown) return;

    cs = (uint16_t)nec_get_reg(NEC_CS);
    ip = (uint16_t)nec_get_reg(NEC_IP);

    /* The corruption sends the CPU into the IVT / low IRAM (CS small, IP tiny)
     * executing zeros. Trap that the instant it happens so the stack still
     * shows HOW we got here. Fallback: dump after ~6s regardless. */
    if (!(cs < 0x100 || ++frame >= 360)) return;
    shown = 1;

    {
        uint16_t ss = (uint16_t)nec_get_reg(NEC_SS);
        uint16_t sp = (uint16_t)nec_get_reg(NEC_SP);
        uint32_t sbase = ((uint32_t)ss << 4) + sp;
        printf("WSBAD: CS:IP=%04X:%04X SS:SP=%04X:%04X REPC=%u REPNC=%u\n",
               cs, ip, ss, sp, g_repc_n, g_repnc_n);
        /* stack words at SP (pushed return CS:IP / flags if we INT'd or CALLed) */
        n = 0;
        for (i = 0; i < 16; i++)
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
    }

    snprintf(buf, sizeof(buf), "WS BAD-JUMP @ %04X:%04X", cs, ip);
    gw_debug_show_log(buf);
}


