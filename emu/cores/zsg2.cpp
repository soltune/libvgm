// license:BSD-3-Clause
// copyright-holders:Olivier Galibert, R. Belmont, hap, superctr
/*
    Taito Zoom sound board for libvgm: ZOOM ZSG-2 custom wavetable synthesizer
    feeding a Texas Instruments TMS57002 "DASP" effects DSP.

    ZSG-2 and TMS57002 code ported from MAME 0.288 (src/devices/sound/zsg2.cpp,
    src/devices/cpu/tms57002, src/mame/sony/taito_zm.cpp).

    The ZSG-2 has 4 outputs: 0 = reverb send, 1 = chorus send, 2 = left direct,
    3 = right direct. They all go into the DSP (sends at half level, as on the MAME
    board), and only the DSP outputs 2/3 reach the speakers, scaled by the board's
    6-bit output gain registers.

    Sample ROM: 32-bit little-endian blocks, each holding 4 compressed samples
    (see MAME zsg2.cpp). The MN10200 sound CPU is not emulated: the VGM carries the
    ZSG-2 register writes and the DSP host-port traffic (program, coefficients and
    run-time coefficient updates) as they were logged.
*/

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../../stdtype.h"
#include "../EmuStructs.h"
#include "../SoundDevs.h"
#include "../EmuCores.h"
#include "../snddef.h"
#include "../EmuHelper.h"
#include "zsg2.h"
#include "zoom/tms57002.h"

#define EMPHASIS_INITIAL_BIAS 0
#define EMPHASIS_FILTER_SHIFT (16-10)
#define EMPHASIS_ROUNDING 0x20
#define EMPHASIS_OUTPUT_SHIFT 1

#define STATUS_ACTIVE	0x8000

typedef struct
{
	UINT16 v[16];
	UINT16 status;
	UINT32 cur_pos;
	UINT32 step_ptr;
	UINT32 step;
	UINT32 start_pos;
	UINT32 end_pos;
	UINT32 loop_pos;
	UINT32 page;

	UINT16 vol;
	UINT16 vol_initial;
	UINT16 vol_target;
	INT16 vol_delta;

	UINT16 output_cutoff;
	UINT16 output_cutoff_initial;
	UINT16 output_cutoff_target;
	INT16 output_cutoff_delta;

	INT32 emphasis_filter_state;
	INT32 output_filter_state;

	UINT8 output_gain[4];
	INT16 samples[5];
	UINT8 muted;
} zchan;

typedef struct
{
	DEV_DATA _devData;

	UINT16 gain_tab[256];
	UINT16 reg[32];
	zchan chan[48];
	UINT32 sample_count;

	UINT32* mem_base;	// ROM as 32-bit blocks
	UINT32* mem_copy;
	UINT32 mem_blocks;
	INT16* full_samples;
	UINT32 read_address;

	zoom::tms57002_device* dsp;
	UINT8 out_gain[2];	// board output gain L/R (6-bit)
	UINT8 bypass;
	UINT32 dsp_cycles;	// DSP cycles per sample
} zsg2_state;

static void zsg2_update(void* info, UINT32 samples, DEV_SMPL** outputs);
static UINT8 device_start_zsg2(const DEV_GEN_CFG* cfg, DEV_INFO* retDevInf);
static void device_stop_zsg2(void* info);
static void device_reset_zsg2(void* info);
static void zsg2_w(void* info, UINT16 offset, UINT16 data);
static UINT16 zsg2_r(void* info, UINT16 offset);
static void zsg2_alloc_rom(void* info, UINT32 memsize);
static void zsg2_write_rom(void* info, UINT32 offset, UINT32 length, const UINT8* data);
static void zsg2_set_mute_mask(void* info, UINT32 MuteMask);
static void zsg2_set_options(void* info, UINT32 Flags);

static DEVDEF_RWFUNC devFunc[] =
{
	{RWF_REGISTER | RWF_WRITE, DEVRW_A16D16, 0, (void*)zsg2_w},
	{RWF_REGISTER | RWF_READ, DEVRW_A16D16, 0, (void*)zsg2_r},
	{RWF_MEMORY | RWF_WRITE, DEVRW_BLOCK, 0, (void*)zsg2_write_rom},
	{RWF_MEMORY | RWF_WRITE, DEVRW_MEMSIZE, 0, (void*)zsg2_alloc_rom},
	{RWF_CHN_MUTE | RWF_WRITE, DEVRW_ALL, 0, (void*)zsg2_set_mute_mask},
	{0x00, 0x00, 0, NULL}
};
static DEV_DEF devDef =
{
	"ZSG-2", "MAME", FCC_MAME,

	device_start_zsg2,
	device_stop_zsg2,
	device_reset_zsg2,
	zsg2_update,

	zsg2_set_options,	// SetOptionBits
	zsg2_set_mute_mask,
	NULL,	// SetPanning
	NULL,	// SetSampleRateChangeCallback
	NULL,	// SetLoggingCallback
	NULL,	// LinkDevice

	devFunc,	// rwFuncs
};

static const char* DeviceName(const DEV_GEN_CFG* devCfg)
{
	return "ZSG-2";
}

static UINT16 DeviceChannels(const DEV_GEN_CFG* devCfg)
{
	return 48;
}

static const char** DeviceChannelNames(const DEV_GEN_CFG* devCfg)
{
	return NULL;
}

static const DEVLINK_IDS* DeviceLinkIDs(const DEV_GEN_CFG* devCfg)
{
	return NULL;
}

extern "C" const DEV_DECL sndDev_ZSG2 =
{
	DEVID_ZSG2,
	DeviceName,
	DeviceChannels,
	DeviceChannelNames,
	DeviceLinkIDs,
	{	// cores
		&devDef,
		NULL
	}
};


static void chan_w(zsg2_state* chip, int ch, int reg, UINT16 data);
static void control_w(zsg2_state* chip, int reg, UINT16 data);

static UINT8 device_start_zsg2(const DEV_GEN_CFG* cfg, DEV_INFO* retDevInf)
{
	zsg2_state* chip;
	int i;

	chip = (zsg2_state*)calloc(1, sizeof(zsg2_state));
	if (chip == NULL)
		return 0xFF;

	// Generate the output gain table. Assuming -1dB per step for now.
	for (i = 1; i < 32; i++)
		chip->gain_tab[i] = (UINT16)(pow(10, -(31 - i) / 20.) * 65535.);
	chip->gain_tab[0] = 0;

	chip->dsp = new zoom::tms57002_device(0x40000);	// 256 KB of DSP RAM on the board
	// DSP clock 12.5 MHz (= ZSG-2 clock / 2), one program pass per output sample
	chip->dsp_cycles = 768 / 2;
	chip->out_gain[0] = chip->out_gain[1] = 0x3F;
	chip->bypass = 0x00;

	zsg2_set_mute_mask(chip, 0x00000000);

	chip->_devData.chipInf = chip;
	INIT_DEVINF(retDevInf, &chip->_devData, cfg->clock / 768, &devDef);
	return 0x00;
}

static void device_stop_zsg2(void* info)
{
	zsg2_state* chip = (zsg2_state*)info;

	delete chip->dsp;
	free(chip->mem_base);
	free(chip->mem_copy);
	free(chip->full_samples);
	free(chip);
}

static void device_reset_zsg2(void* info)
{
	zsg2_state* chip = (zsg2_state*)info;
	int ch, reg;

	chip->read_address = 0;

	// stop playing and clear all channels
	control_w(chip, 4, 0xffff);
	control_w(chip, 5, 0xffff);
	control_w(chip, 6, 0xffff);

	for (ch = 0; ch < 48; ch++)
		for (reg = 0; reg < 0x10; reg++)
			chan_w(chip, ch, reg, 0);	// keeps the mute flags
	chip->sample_count = 0;

	if (chip->mem_copy != NULL)
		memset(chip->mem_copy, 0, chip->mem_blocks * sizeof(UINT32));

	chip->dsp->device_reset();
	chip->out_gain[0] = chip->out_gain[1] = 0x3F;
}

/******************************************************************************/

static UINT32 read_memory(zsg2_state* chip, UINT32 offset)
{
	if (offset >= chip->mem_blocks)
		return 0;
	return chip->mem_base[offset];
}

static INT16* prepare_samples(zsg2_state* chip, UINT32 offset)
{
	UINT32 block = read_memory(chip, offset);
	UINT8 shift;
	UINT32 i;

	if (block == 0)
		return &chip->full_samples[chip->mem_blocks * 4];	// overflow or 0

	if (block == chip->mem_copy[offset])
		return &chip->full_samples[offset * 4];	// cached

	chip->mem_copy[offset] = block;
	offset *= 4;

	// decompress 32 bit block to 4 16-bit samples
	// 42222222 51111111 60000000 ssss3333
	chip->full_samples[offset|0] = block >> 8 & 0x7f;
	chip->full_samples[offset|1] = block >> 16 & 0x7f;
	chip->full_samples[offset|2] = block >> 24 & 0x7f;
	chip->full_samples[offset|3] = (block >> (8+1) & 0x40) | (block >> (16+2) & 0x20) | (block >> (24+3) & 0x10) | (block & 0xf);

	// sign-extend and shift
	shift = block >> 4 & 0xf;
	for (i = offset; i < (offset + 4); i++)
	{
		chip->full_samples[i] <<= 9;
		chip->full_samples[i] >>= shift;
	}

	return &chip->full_samples[offset];
}

// Fill the buffer with filtered samples
static void filter_samples(zsg2_state* chip, zchan* ch)
{
	INT16* raw_samples = prepare_samples(chip, ch->page | ch->cur_pos);
	int i;

	ch->samples[0] = ch->samples[4];	// we want to remember the last sample

	for (i = 0; i < 4; i++)
	{
		INT32 sample;

		ch->emphasis_filter_state += raw_samples[i]-((ch->emphasis_filter_state+EMPHASIS_ROUNDING)>>EMPHASIS_FILTER_SHIFT);

		sample = ch->emphasis_filter_state >> EMPHASIS_OUTPUT_SHIFT;
		if (sample < -32768)
			sample = -32768;
		else if (sample > 32767)
			sample = 32767;
		ch->samples[i+1] = sample;
	}
}

INLINE UINT16 ramp(UINT16 current, UINT16 target, INT16 delta)
{
	INT32 rampval = current + delta;
	if (delta < 0 && rampval < target)
		rampval = target;
	else if (delta >= 0 && rampval > target)
		rampval = target;

	return rampval;
}

INLINE INT32 clamp16(INT32 v)
{
	if (v < -32768)
		return -32768;
	else if (v > 32767)
		return 32767;
	return v;
}

static void zsg2_update(void* info, UINT32 samples, DEV_SMPL** outputs)
{
	zsg2_state* chip = (zsg2_state*)info;
	UINT32 i;
	int c, output;

	for (i = 0; i < samples; i++)
	{
		INT32 mix[4] = {0, 0, 0, 0};

		for (c = 0; c < 48 && chip->mem_base != NULL; c++)
		{
			zchan* elem = &chip->chan[c];
			UINT8 sample_pos;
			INT32 sample;

			if (~elem->status & STATUS_ACTIVE)
				continue;

			elem->step_ptr += elem->step;
			if (elem->step_ptr & 0xffff0000)
			{
				if (++elem->cur_pos >= elem->end_pos)
				{
					// loop sample
					elem->cur_pos = elem->loop_pos;
					if ((elem->cur_pos + 1) >= elem->end_pos)
					{
						// end of sample
						elem->vol = 0;
						elem->status &= ~STATUS_ACTIVE;
						continue;
					}
				}

				if (elem->cur_pos == elem->start_pos)
					elem->emphasis_filter_state = EMPHASIS_INITIAL_BIAS;

				elem->step_ptr &= 0xffff;
				filter_samples(chip, elem);
			}

			sample_pos = elem->step_ptr >> 14 & 3;
			sample = elem->samples[sample_pos];

			// linear interpolation (hardware certainly does something similar)
			sample += ((UINT16)(elem->step_ptr << 2 & 0xffff) * (INT16)(elem->samples[sample_pos+1] - sample)) >> 16;

			// another filter...
			elem->output_filter_state += (sample - (elem->output_filter_state >> 16)) * elem->output_cutoff;
			sample = elem->output_filter_state >> 16;

			// To prevent DC bias, we need to slowly discharge the filter when the output filter cutoff is 0
			if (!elem->output_cutoff)
				elem->output_filter_state >>= 1;

			sample = (sample * elem->vol) >> 16;

			for (output = 0; output < 4 && !elem->muted; output++)
			{
				int output_gain = elem->output_gain[output] & 0x1f;	// left / right
				INT32 output_sample = sample;

				if (elem->output_gain[output] & 0x80)	// perhaps ?
					output_sample = -output_sample;

				mix[output] += (output_sample * chip->gain_tab[output_gain & 0x1f]) >> 16;
			}

			// Apply ramping every other update
			// It's possible key on is handled on the other sample
			if (chip->sample_count & 1)
			{
				elem->vol = ramp(elem->vol, elem->vol_target, elem->vol_delta);
				elem->output_cutoff = ramp(elem->output_cutoff, elem->output_cutoff_target, elem->output_cutoff_delta);
			}
		}
		chip->sample_count++;

		for (output = 0; output < 4; output++)
			mix[output] = clamp16(mix[output]);

		if (chip->bypass)
		{
			outputs[0][i] = mix[2];
			outputs[1][i] = mix[3];
		}
		else
		{
			// MAME board: sends routed at 0.5, direct at 1.0; DSP input = 16-bit value (x256 in SIM mode)
			INT32 in[4], out[4];
			int sim = (chip->dsp->get_st0() & zoom::tms57002_device::ST0_SIM_) ? 8 : 0;
			in[0] = (INT32)((mix[0] / 2.0f) * (1 << sim));
			in[1] = (INT32)((mix[1] / 2.0f) * (1 << sim));
			in[2] = mix[2] << sim;
			in[3] = mix[3] << sim;
			chip->dsp->sample(in, out, chip->dsp_cycles);
			// 32-bit DSP output -> 16-bit scale, times the board's output gain
			outputs[0][i] = (DEV_SMPL)(((INT64)out[2] * chip->out_gain[0]) / (63 * 65536));
			outputs[1][i] = (DEV_SMPL)(((INT64)out[3] * chip->out_gain[1]) / (63 * 65536));
		}
	}
}

/******************************************************************************/

// Convert ramping register value to something more usable.
static INT16 get_ramp(UINT8 val)
{
	INT16 frac = val << 12;	// sign extend
	frac = ((frac >> 12) ^ 8) << (val >> 4);
	return (frac >> 4);
}

static void chan_w(zsg2_state* chip, int ch, int reg, UINT16 data)
{
	zchan* c = &chip->chan[ch];

	switch (reg)
	{
		case 0x0:
			// lo byte: unknown, 0 on most games
			// hi byte: start address low
			c->start_pos = (c->start_pos & 0xff00) | (data >> 8 & 0xff);
			break;

		case 0x1:
			// lo byte: start address high
			// hi byte: address page
			c->start_pos = (c->start_pos & 0x00ff) | (data << 8 & 0xff00);
			c->page = data << 8 & 0xff0000;
			break;

		case 0x2:
			// no function? always 0
			break;

		case 0x3:
			// unknown, always 0x0400. is this a flag?
			c->status &= 0x8000;
			c->status |= data & 0x7fff;
			break;

		case 0x4:
			// frequency
			c->step = data + 1;
			break;

		case 0x5:
			// lo byte: loop address low
			// hi byte: right output gain (direct)
			c->loop_pos = (c->loop_pos & 0xff00) | (data & 0xff);
			c->output_gain[3] = data >> 8;
			break;

		case 0x6:
			// end address
			c->end_pos = data;
			break;

		case 0x7:
			// lo byte: loop address high
			// hi byte: left output gain (direct)
			c->loop_pos = (c->loop_pos & 0x00ff) | (data << 8 & 0xff00);
			c->output_gain[2] = data >> 8;
			break;

		case 0x8:
			// IIR lowpass time constant (initial, latched on key on)
			c->output_cutoff_initial = data;
			break;

		case 0x9:
			// writes 0 at key on
			c->output_cutoff = data;
			break;

		case 0xa:
			// volume (initial, latched on key on)
			c->vol_initial = data;
			break;

		case 0xb:
			// writes 0 at key on
			c->vol = data;
			break;

		case 0xc:
			// IIR lowpass time constant (target)
			c->output_cutoff_target = data;
			break;

		case 0xd:
			// hi byte: DSP channel 1 (chorus) gain
			// lo byte: Filter ramping speed
			c->output_gain[1] = data >> 8;
			c->output_cutoff_delta = get_ramp(data & 0xff);
			break;

		case 0xe:
			// volume target
			c->vol_target = data;
			break;

		case 0xf:
			// hi byte: DSP channel 0 (reverb) gain
			// lo byte: Volume ramping speed
			c->output_gain[0] = data >> 8;
			c->vol_delta = get_ramp(data & 0xff);
			break;

		default:
			break;
	}

	c->v[reg] = data;
}

static UINT16 chan_r(zsg2_state* chip, int ch, int reg)
{
	switch (reg)
	{
		case 0x3:
			return chip->chan[ch].status;
		case 0x9:
			return chip->chan[ch].output_cutoff;
		case 0xb:
			return chip->chan[ch].vol;
		default:
			break;
	}

	return chip->chan[ch].v[reg];
}

static void control_w(zsg2_state* chip, int reg, UINT16 data)
{
	int i;

	switch (reg)
	{
		case 0x00: case 0x01: case 0x02:
		{
			// key on
			int base = (reg & 3) << 4;
			for (i = 0; i < 16; i++)
			{
				if (data & (1 << i))
				{
					zchan* c = &chip->chan[base | i];
					c->status |= STATUS_ACTIVE;
					c->cur_pos = c->start_pos - 1;
					c->step_ptr = 0x10000;
					// Ignoring the "initial volume" for now because it causes lots of clicking
					c->vol = 0;
					c->vol_delta = 0x0400;
					c->output_cutoff = c->output_cutoff_initial;
					c->output_filter_state = 0;
				}
			}
			break;
		}

		case 0x04: case 0x05: case 0x06:
		{
			// key off
			int base = (reg & 3) << 4;
			for (i = 0; i < 16; i++)
			{
				if (data & (1 << i))
				{
					zchan* c = &chip->chan[base | i];
					c->vol = 0;
					c->status &= ~STATUS_ACTIVE;
				}
			}
			break;
		}

		case 0x1c:
			// rom readback address low (low 2 bits always 0)
			chip->read_address = (chip->read_address & 0x3fffc000) | (data >> 2 & 0x00003fff);
			break;
		case 0x1d:
			// rom readback address high
			chip->read_address = (chip->read_address & 0x00003fff) | (data << 14 & 0x3fffc000);
			break;

		default:
			if (reg < 0x20)
				chip->reg[reg] = data;
			break;
	}
}

static UINT16 control_r(zsg2_state* chip, int reg)
{
	switch (reg)
	{
		case 0x14:
			return 0;
		case 0x1e:
			return read_memory(chip, chip->read_address) & 0xffff;
		case 0x1f:
			return read_memory(chip, chip->read_address) >> 16;
		default:
			if (reg < 0x20)
				return chip->reg[reg];
			break;
	}

	return 0;
}

/******************************************************************************/

static void zsg2_w(void* info, UINT16 offset, UINT16 data)
{
	zsg2_state* chip = (zsg2_state*)info;

	if (offset < 0x300)
	{
		chan_w(chip, offset >> 4, offset & 0xf, data);
	}
	else if (offset < 0x400)
	{
		control_w(chip, offset - 0x300, data);
	}
	else if (offset == 0x400)
	{
		chip->dsp->data_w(data & 0xff);
	}
	else if (offset == 0x401)
	{
		// taito_zoom_device::tms_ctrl_w
		chip->dsp->cload_w(data & 2);
		chip->dsp->pload_w(data & 1);
	}
	else if (offset >= 0x480 && offset < 0x500)
	{
		// taito_zoom_device::reg_data_w
		switch (offset & 0x7f)
		{
			case 0x04:
				chip->out_gain[0] = data & 0x3f;
				break;
			case 0x05:
				chip->out_gain[1] = data & 0x3f;
				break;
		}
	}
}

static UINT16 zsg2_r(void* info, UINT16 offset)
{
	zsg2_state* chip = (zsg2_state*)info;

	if (offset < 0x300)
		return chan_r(chip, offset >> 4, offset & 0xf);
	else if (offset < 0x400)
		return control_r(chip, offset - 0x300);
	else if (offset == 0x400)
		return chip->dsp->data_r();
	return 0;
}

static void zsg2_alloc_rom(void* info, UINT32 memsize)
{
	zsg2_state* chip = (zsg2_state*)info;
	UINT32 blocks = memsize / 4;

	if (chip->mem_blocks == blocks && chip->mem_base != NULL)
		return;

	chip->mem_base = (UINT32*)realloc(chip->mem_base, blocks * sizeof(UINT32));
	chip->mem_copy = (UINT32*)realloc(chip->mem_copy, blocks * sizeof(UINT32));
	chip->full_samples = (INT16*)realloc(chip->full_samples, (blocks * 4 + 4) * sizeof(INT16));
	chip->mem_blocks = blocks;
	memset(chip->mem_base, 0, blocks * sizeof(UINT32));
	memset(chip->mem_copy, 0, blocks * sizeof(UINT32));
	memset(chip->full_samples, 0, (blocks * 4 + 4) * sizeof(INT16));
}

static void zsg2_write_rom(void* info, UINT32 offset, UINT32 length, const UINT8* data)
{
	zsg2_state* chip = (zsg2_state*)info;
	UINT32 i;

	if (offset >= chip->mem_blocks * 4)
		return;
	if (offset + length > chip->mem_blocks * 4)
		length = chip->mem_blocks * 4 - offset;

	// ROM_REGION32_LE: little-endian 32-bit blocks
	for (i = 0; i < length; i++)
	{
		UINT32 a = offset + i;
		UINT32 sh = (a & 3) * 8;
		chip->mem_base[a >> 2] = (chip->mem_base[a >> 2] & ~(0xFFu << sh)) | ((UINT32)data[i] << sh);
		chip->mem_copy[a >> 2] = 0;
	}
}

static void zsg2_set_mute_mask(void* info, UINT32 MuteMask)
{
	zsg2_state* chip = (zsg2_state*)info;
	int ch;

	// 48 voices, the mask covers voices 0-31 (voices 32-47 follow bit 31)
	for (ch = 0; ch < 48; ch++)
		chip->chan[ch].muted = (MuteMask >> (ch < 32 ? ch : 31)) & 0x01;
}

static void zsg2_set_options(void* info, UINT32 Flags)
{
	zsg2_state* chip = (zsg2_state*)info;

	chip->bypass = Flags & 0x01;
}
