/* Sega/Mega CD — RF5C164 PCM + final audio mix, phase 4.
 *
 * The mix mirrors PCE-CD (Core/Src/porting/pce/main_pce.c sound submit): the
 * base chip output (here gwenesis YM2612 + SN76489) plus CD-DA (Red Book BGM,
 * streamed from SD, halved) plus the sample chip (here RF5C164, there ADPCM),
 * summed, scaled by volume, clamped. The RF5C164 generator is adapted from
 * PicoDrive pd_cd/pcm.c against SCD.pcm_ram; its state lives in SCD.pcm so
 * savestate captures it (like PCE's ADPCM block).
 */
#include <string.h>
#include "segacd.h"

#define SEGACD_AUDIO_MAX 1200   /* max stereo frames per submit */

/* ---- RF5C164 register writes (from the sub-CPU $FF00xx PCM window) ---- */

void segacd_pcm_write(unsigned int reg, unsigned int val)
{
    segacd_pcm_chan *ch = &SCD.pcm.ch[SCD.pcm.cur_ch];
    switch (reg & 0x0F) {
    case 0x00: ch->env = (uint16_t)val; break;
    case 0x01: ch->pan = (uint8_t)val; break;
    case 0x02: ch->fd = (uint16_t)((ch->fd & 0xFF00) | (val & 0xFF)); break;
    case 0x03: ch->fd = (uint16_t)((ch->fd & 0x00FF) | ((val & 0xFF) << 8)); break;
    case 0x04: ch->loop = (uint16_t)((ch->loop & 0xFF00) | (val & 0xFF)); break;
    case 0x05: ch->loop = (uint16_t)((ch->loop & 0x00FF) | ((val & 0xFF) << 8)); break;
    case 0x06: /* start address (high byte) — set play pointer */
        ch->start = (uint16_t)(val & 0xFF);
        ch->addr  = (uint32_t)(ch->start << 8) << SEGACD_PCM_STEP_SHIFT;
        break;
    case 0x07: /* control: bit7 enable, bit6 => select ch, else select bank */
        if (val & 0x40) SCD.pcm.cur_ch = (uint8_t)(val & 7);
        else            SCD.pcm.bank   = (uint8_t)(val & 0x0F);
        SCD.pcm.control = (uint8_t)val;
        break;
    case 0x08: /* per-channel on/off (active-low) */
        SCD.pcm.enabled = (uint8_t)~val;
        break;
    default: break;
    }
}

/* ---- generate `frames` stereo samples into out (interleaved L/R) ---- */

void segacd_pcm_update(int16_t *out, int frames)
{
    memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));
    if (!(SCD.pcm.control & 0x80) || !SCD.pcm_ram)   /* chip off */
        return;

    for (int c = 0; c < 8; c++) {
        if (!(SCD.pcm.enabled & (1 << c))) continue;
        segacd_pcm_chan *ch = &SCD.pcm.ch[c];
        uint32_t addr = ch->addr;
        int env  = ch->env;                 /* 0..255 */
        int panL = ch->pan & 0x0F;
        int panR = (ch->pan >> 4) & 0x0F;

        for (int s = 0; s < frames; s++) {
            uint8_t smp = SCD.pcm_ram[(addr >> SEGACD_PCM_STEP_SHIFT) & (SEGACD_PCM_RAM_SIZE - 1)];
            if (smp == 0xFF) {              /* loop/end marker */
                addr = (uint32_t)ch->loop << SEGACD_PCM_STEP_SHIFT;
                smp  = SCD.pcm_ram[(addr >> SEGACD_PCM_STEP_SHIFT) & (SEGACD_PCM_RAM_SIZE - 1)];
                if (smp == 0xFF) break;     /* dead channel */
            }
            /* 8-bit sign-magnitude: bit7 = sign, bits0-6 = magnitude */
            int v = smp & 0x7F;
            if (!(smp & 0x80)) v = -v;
            v = (v * env) >> 5;
            out[s * 2]     += (int16_t)((v * panL) >> 5);
            out[s * 2 + 1] += (int16_t)((v * panR) >> 5);
            addr += ch->fd;
        }
        ch->addr = addr;
    }
}

/* ---- final mix: gwenesis (YM+SN) + CD-DA + PCM -> device sound buffer ---- */

void segacd_audio_mix(int16_t *dst, const int16_t *ym, const int16_t *sn,
                      int frames, int volume)
{
    static int16_t cdda[SEGACD_AUDIO_MAX * 2];
    static int16_t pcm[SEGACD_AUDIO_MAX * 2];

    if (frames > SEGACD_AUDIO_MAX) frames = SEGACD_AUDIO_MAX;

    int cn = segacd_cdda_fill(cdda, frames);   /* CD-DA streamed from SD */
    segacd_pcm_update(pcm, frames);            /* RF5C164 */

    for (int i = 0; i < frames; i++) {
        int32_t s = (int32_t)ym[i] + (int32_t)sn[i];
        if (cn && i < cn)
            s += ((int32_t)cdda[i * 2] + (int32_t)cdda[i * 2 + 1]) >> 1;
        s += ((int32_t)pcm[i * 2] + (int32_t)pcm[i * 2 + 1]) >> 1;
        s = (s * volume) >> 8;
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        dst[i] = (int16_t)s;
    }
}
