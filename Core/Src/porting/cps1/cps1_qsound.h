/*
 * Capcom DL-1425 QSound emulator -- vendored.
 *
 * Original: superctr (Ian Karlsson), with thanks to Valley Bell, 2018.
 * The same authors' same emulator is carried in MAME as
 * src/devices/sound/qsoundhle.cpp under "license:BSD-3-Clause
 * copyright-holders:superctr, Valley Bell"; this copy is used under those
 * terms. Local changes are marked GNW.
 *
 * Why THIS implementation and not MAME's own qsound.cpp/qsoundhle.cpp: both of
 * those read their pan/filter/ADPCM tables out of the DL-1425's internal DSP
 * ROM (dl-1425.bin, 8 KB) and no CPS-1 game romset carries it -- there would be
 * nothing to load. superctr extracted those tables and BAKED THEM IN
 * (qsound_filter_data[5][95], the mix tables, adpcm_step_table below), so this
 * runs correctly with the game's sample ROMs alone.
 *
 * GNW change: the sample ROM. This emulator wants one flat rom_data[] pointer,
 * but the four 512 KB sample chips are cached at four unrelated flash
 * addresses, so the flat buffer does not exist here. rom_data/rom_mask are
 * replaced by rom_chip[4] and get_sample() dispatches per chip, exactly the way
 * cps1_gfx_chip_byte() already does for the graphics chips.
 */
#ifndef CPS1_QSOUND_H
#define CPS1_QSOUND_H

#ifdef __cplusplus
extern "C" {
#endif
/*

	Capcom DL-1425 QSound emulator
	==============================

	by superctr (Ian Karlsson)
	with thanks to Valley Bell

	2018-05-12 - 2018-05-15

*/

#include <stdint.h>

#define CPS1_QSOUND_ROM_CHIPS 4       /* 4 x 512 KB = 2 MB of samples */
#define CPS1_QSOUND_ROM_CHIP_SHIFT 19 /* 512 KB per chip */

struct cps1_qsound_voice {
	uint16_t bank;
	int16_t addr; // top word is the sample address
	uint16_t phase;
	uint16_t rate;
	int16_t loop_len;
	int16_t end_addr;
	int16_t volume;
	int16_t echo;
};

struct cps1_qsound_adpcm {
	uint16_t start_addr;
	uint16_t end_addr;
	uint16_t bank;
	int16_t volume;
	uint16_t flag;
	int16_t cur_vol;
	int16_t step_size;
	uint16_t cur_addr;
};

// Q1 Filter
struct cps1_qsound_fir {
	int tap_count;	// usually 95
	int delay_pos;
	int16_t table_pos;
	int16_t taps[95];
	int16_t delay_line[95];
};

// Delay line
struct cps1_qsound_delay {
	int16_t delay;
	int16_t volume;
	int16_t write_pos;
	int16_t read_pos;
	int16_t delay_line[51];
};

struct cps1_qsound_echo {
	uint16_t end_pos;

	int16_t feedback;
	int16_t length;
	int16_t last_sample;
	int16_t delay_line[1024];
	int16_t delay_pos;
};

struct cps1_qsound_chip {

	/* GNW: four 512 KB sample chips in cached flash, not one flat buffer. */
	const uint8_t *rom_chip[CPS1_QSOUND_ROM_CHIPS];

	uint32_t mute_mask;

	uint16_t data_latch;
	int16_t out[2];

	int16_t pan_tables[2][2][98];

	struct cps1_qsound_voice voice[16];
	struct cps1_qsound_adpcm adpcm[3];

	uint16_t voice_pan[16+3];
	int16_t voice_output[16+3];

	struct cps1_qsound_echo echo;

	struct cps1_qsound_fir filter[2];
	struct cps1_qsound_fir alt_filter[2];

	struct cps1_qsound_delay wet[2];
	struct cps1_qsound_delay dry[2];

	uint16_t state;
	uint16_t next_state;

	uint16_t delay_update;

	int state_counter;
	int ready_flag;

	uint16_t *register_map[256];
};

long cps1_qsound_start(struct cps1_qsound_chip *chip, int clock);
void cps1_qsound_reset(struct cps1_qsound_chip *chip);
void cps1_qsound_update(struct cps1_qsound_chip *chip);

void cps1_qsound_stream_update(struct cps1_qsound_chip *chip, int16_t **outputs, int samples);
void cps1_qsound_w(struct cps1_qsound_chip *chip, uint8_t offset, uint8_t data);
uint8_t cps1_qsound_r(struct cps1_qsound_chip *chip);
void cps1_qsound_write_data(struct cps1_qsound_chip *chip, uint8_t address, uint16_t data);
uint16_t cps1_qsound_read_data(struct cps1_qsound_chip *chip, uint8_t address);

#ifdef __cplusplus
};
#endif
#endif
