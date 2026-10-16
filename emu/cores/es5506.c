// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/**********************************************************************************************

     Ensoniq ES5505 (OTIS) / ES5506 (OTTO) driver
     by Aaron Giles

     Ported to libvgm from MAME src/devices/sound/es5506.cpp (MAME 0.28x, 2025-10:
     "es550[56] devices generate 20bit samples").

     VGM interface (compatible with VGMPlay 0.40.x):
     - clock bit 31 set = ES5506, clear = ES5505
     - cfg->flags = number of output channels (ES5505: 1..4, ES5506: 1..6); each output channel is
       clamped to 20 bits like on the chip, then all channels are mixed down to stereo (20-bit scale).
       (The ES5510 ESP that processes them on Taito F3 / Ensoniq boards is not emulated.)
     - 8-bit register write (VGM 0xBE): offset 0x00..0x3F = register byte
         ES5505: offset = register * 2 + byte (byte 0 = bits 15-8, byte 1 = bits 7-0)
         ES5506: offset = register * 4 + byte (32-bit write latch, committed on byte 3)
       offset 0x40..0x5F = external bank of voice (offset & 0x1F), in units of 1M words
     - 16-bit register write (VGM 0xD6): offset = register byte offset (bit 0 clear), data = 16 bits
     - ROM data block (VGM 0x90): DataStart bits 28-29 = region (selected by the voice's BS bits),
       bit 31 = 8-bit ROM (each byte is the high byte of a sample word)

***********************************************************************************************/

#include <stdlib.h>
#include <string.h>	// for memset
#include <stddef.h>	// for NULL

#include "../../stdtype.h"
#include "../EmuStructs.h"
#include "../SoundDevs.h"
#include "../EmuCores.h"
#include "../snddef.h"
#include "../EmuHelper.h"
#include "es5506.h"


static void es5506_update(void* info, UINT32 samples, DEV_SMPL** outputs);
static UINT8 device_start_es5506(const DEV_GEN_CFG* cfg, DEV_INFO* retDevInf);
static void device_stop_es5506(void* info);
static void device_reset_es5506(void* info);

static void es550x_w(void* info, UINT8 offset, UINT8 data);
static void es550x_w16(void* info, UINT8 offset, UINT16 data);

static void es5506_alloc_rom(void* info, UINT32 memsize);
static void es5506_write_rom(void* info, UINT32 offset, UINT32 length, const UINT8* data);

static void es5506_set_mute_mask(void* info, UINT32 MuteMask);
static void es5506_set_srchg_cb(void* info, DEVCB_SRATE_CHG CallbackFunc, void* DataPtr);


static DEVDEF_RWFUNC devFunc[] =
{
	{RWF_REGISTER | RWF_WRITE, DEVRW_A8D8, 0, es550x_w},
	{RWF_REGISTER | RWF_WRITE, DEVRW_A8D16, 0, es550x_w16},
	{RWF_MEMORY | RWF_WRITE, DEVRW_BLOCK, 0, es5506_write_rom},
	{RWF_MEMORY | RWF_WRITE, DEVRW_MEMSIZE, 0, es5506_alloc_rom},
	{RWF_CHN_MUTE | RWF_WRITE, DEVRW_ALL, 0, es5506_set_mute_mask},
	{0x00, 0x00, 0, NULL}
};
static DEV_DEF devDef =
{
	"ES5506", "MAME", FCC_MAME,

	device_start_es5506,
	device_stop_es5506,
	device_reset_es5506,
	es5506_update,

	NULL,	// SetOptionBits
	es5506_set_mute_mask,
	NULL,	// SetPanning
	es5506_set_srchg_cb,	// SetSampleRateChangeCallback
	NULL,	// SetLoggingCallback
	NULL,	// LinkDevice

	devFunc,	// rwFuncs
};

static const char* DeviceName(const DEV_GEN_CFG* devCfg)
{
	if (devCfg != NULL && ! (devCfg->clock & 0x80000000))
		return "ES5505";
	return "ES5506";
}

static UINT16 DeviceChannels(const DEV_GEN_CFG* devCfg)
{
	return 32;
}

static const char** DeviceChannelNames(const DEV_GEN_CFG* devCfg)
{
	return NULL;
}

static const DEVLINK_IDS* DeviceLinkIDs(const DEV_GEN_CFG* devCfg)
{
	return NULL;
}

const DEV_DECL sndDev_ES5506 =
{
	DEVID_ES5506,
	DeviceName,
	DeviceChannels,
	DeviceChannelNames,
	DeviceLinkIDs,
	{	// cores
		&devDef,
		NULL
	}
};


/**********************************************************************************************

     CONSTANTS

***********************************************************************************************/

#define FINE_FILTER_BIT	16
#define FILTER_BIT		12
#define FILTER_SHIFT	(FINE_FILTER_BIT - FILTER_BIT)

#define ULAW_MAXBITS	8

#define VOLUME_ACC_BIT		20
#define ADDRESS_FRAC_BIT	11

#define VOLUME_BIT_ES5506			16
#define ADDRESS_INTEGER_BIT_ES5506	21
#define ADDRESS_FRAC_BIT_ES5506		11
#define VOLUME_BIT_ES5505			8
#define ADDRESS_INTEGER_BIT_ES5505	20
#define ADDRESS_FRAC_BIT_ES5505		9

#define LP3		1
#define LP4		2
#define LP_MASK	(LP3 | LP4)

enum
{
	CONTROL_BS1         = 0x8000,
	CONTROL_BS0         = 0x4000,
	CONTROL_CMPD        = 0x2000,
	CONTROL_CA2         = 0x1000,
	CONTROL_CA1         = 0x0800,
	CONTROL_CA0         = 0x0400,
	CONTROL_LP4         = 0x0200,
	CONTROL_LP3         = 0x0100,
	CONTROL_IRQ         = 0x0080,
	CONTROL_DIR         = 0x0040,
	CONTROL_IRQE        = 0x0020,
	CONTROL_BLE         = 0x0010,
	CONTROL_LPE         = 0x0008,
	CONTROL_LEI         = 0x0004,
	CONTROL_STOP1       = 0x0002,
	CONTROL_STOP0       = 0x0001,

	CONTROL_LOOPMASK    = (CONTROL_BLE | CONTROL_LPE),
	CONTROL_STOPMASK    = (CONTROL_STOP1 | CONTROL_STOP0)
};

typedef struct
{
	// external state
	UINT32 control;		// control register
	UINT64 freqcount;	// frequency count register
	UINT64 start;		// start register
	UINT32 lvol;		// left volume register
	UINT64 end;			// end register
	UINT32 lvramp;		// left volume ramp register
	UINT64 accum;		// accumulator register
	UINT32 rvol;		// right volume register
	UINT32 rvramp;		// right volume ramp register
	UINT32 ecount;		// envelope count register
	UINT32 k2;			// k2 register
	UINT32 k2ramp;		// k2 ramp register
	UINT32 k1;			// k1 register
	UINT32 k1ramp;		// k1 ramp register
	INT32 o4n1;			// filter storage O4(n-1)
	INT32 o3n1;			// filter storage O3(n-1)
	INT32 o3n2;			// filter storage O3(n-2)
	INT32 o2n1;			// filter storage O2(n-1)
	INT32 o2n2;			// filter storage O2(n-2)
	INT32 o1n1;			// filter storage O1(n-1)
	UINT32 exbank;		// external address bank (in words)

	// internal state
	UINT8 index;		// index of this voice
	UINT8 filtcount;	// filter count
	UINT8 Muted;
} ES550X_VOICE;

typedef struct
{
	DEV_DATA _devData;

	UINT8 sndtype;				// 0 = ES5505, 1 = ES5506
	UINT32 master_clock;		// master clock frequency
	UINT32 sample_rate;			// current sample rate
	INT8 address_acc_shift;		// right shift accumulator for generate integer address
	UINT64 address_acc_mask;	// accumulator mask
	INT8 volume_shift;			// right shift volume for generate integer volume
	INT8 volume_acc_shift;		// right shift output for output normalizing
	UINT8 volume_bit;			// total volume bits (ES5505: 8, ES5506: 16)

	UINT8 current_page;			// current register page
	UINT8 active_voices;		// number of active voices
	UINT16 mode;				// MODE register
	UINT8 irqv;					// IRQV register

	// ES5506 specific registers
	UINT32 write_latch;			// currently accumulated data for write
	UINT8 wst;					// W_ST register
	UINT8 wend;					// W_END register
	UINT8 lrend;				// LR_END register

	ES550X_VOICE voice[32];		// the 32 voices

	INT16 ulaw_lookup[1 << ULAW_MAXBITS];
	UINT32 volume_lookup[1 << 12];

	UINT16* region_base[4];		// sample ROM, in words
	UINT32 region_size[4];		// size in words
	UINT32 pending_rom_size;	// size from the last data block header (bytes)

	UINT8 channels;				// number of output channels: 1 .. 6

	DEVCB_SRATE_CHG SmpRateFunc;
	void* SmpRateData;
} ES5506_STATE;


static UINT64 lshift_signed64(UINT64 val, INT8 shift)
{
	return (shift >= 0) ? (val << shift) : (val >> (-shift));
}

static UINT32 rshift_signed32(UINT32 val, INT8 shift)
{
	return (shift >= 0) ? (val >> shift) : (val << (-shift));
}

static INT64 rshift_signed_s64(INT64 val, INT8 shift)
{
	return (shift >= 0) ? (val >> shift) : (val << (-shift));
}

INLINE UINT64 get_address_acc_shifted_val(ES5506_STATE* chip, UINT64 val, int bias)
{
	return lshift_signed64(val, chip->address_acc_shift - bias);
}

INLINE UINT64 get_integer_addr(ES5506_STATE* chip, UINT64 accum, INT32 bias)
{
	return ((accum + ((UINT64)bias << ADDRESS_FRAC_BIT)) & chip->address_acc_mask) >> ADDRESS_FRAC_BIT;
}

INLINE UINT32 get_volume(ES5506_STATE* chip, UINT32 volume)
{
	return chip->volume_lookup[rshift_signed32(volume, chip->volume_shift)];
}

INLINE INT32 get_sample(ES5506_STATE* chip, INT32 sample, UINT32 volume)
{
	return (INT32)rshift_signed_s64((INT64)sample * get_volume(chip, volume), chip->volume_acc_shift);
}

INLINE UINT32 get_bank(ES5506_STATE* chip, UINT32 control)
{
	return chip->sndtype ? ((control >> 14) & 3) : ((control >> 2) & 1);
}

INLINE UINT32 get_ca(ES5506_STATE* chip, UINT32 control)
{
	return chip->sndtype ? ((control >> 10) & 7) : ((control >> 8) & 3);
}

INLINE UINT32 get_lp(ES5506_STATE* chip, UINT32 control)
{
	return chip->sndtype ? ((control >> 8) & LP_MASK) : ((control >> 10) & LP_MASK);
}

INLINE INT32 sext18(UINT32 data)
{
	return (INT32)(data << 14) >> 14;
}

INLINE UINT16 read_sample(ES5506_STATE* chip, ES550X_VOICE* voice, UINT32 addr)
{
	UINT32 rgn = get_bank(chip, voice->control);
	UINT32 pos = voice->exbank + addr;

	if (chip->region_base[rgn] == NULL || pos >= chip->region_size[rgn])
		return 0x0000;
	return chip->region_base[rgn][pos];
}

static void set_sample_rate(ES5506_STATE* chip)
{
	UINT32 newRate = chip->master_clock / (16 * (chip->active_voices + 1));

	if (newRate == chip->sample_rate)
		return;
	chip->sample_rate = newRate;
	if (chip->SmpRateFunc != NULL)
		chip->SmpRateFunc(chip->SmpRateData, chip->sample_rate);
}


/**********************************************************************************************

     compute_tables -- compute static tables

***********************************************************************************************/

static void compute_tables(ES5506_STATE* chip, UINT32 total_volume_bit, UINT32 exponent_bit, UINT32 mantissa_bit)
{
	UINT32 i;
	UINT32 volume_bit, volume_len;
	UINT32 exponent_shift, exponent_mask;
	UINT32 mantissa_len, mantissa_mask, mantissa_shift;

	// generate ulaw lookup table
	for (i = 0; i < (1 << ULAW_MAXBITS); i++)
	{
		UINT16 rawval = (UINT16)((i << (16 - ULAW_MAXBITS)) | (1 << (15 - ULAW_MAXBITS)));
		UINT8 exponent = rawval >> 13;
		UINT32 mantissa = (rawval << 3) & 0xffff;

		if (exponent == 0)
			chip->ulaw_lookup[i] = (INT16)mantissa >> 7;
		else
		{
			mantissa = (mantissa >> 1) | (~mantissa & 0x8000);
			chip->ulaw_lookup[i] = (INT16)mantissa >> (7 - exponent);
		}
	}

	volume_bit = (exponent_bit + mantissa_bit);
	chip->volume_bit = (UINT8)total_volume_bit;
	chip->volume_shift = (INT8)(total_volume_bit - volume_bit);
	volume_len = 1 << volume_bit;

	// generate volume lookup table
	exponent_shift = 1 << exponent_bit;
	exponent_mask = exponent_shift - 1;

	mantissa_len = (1 << mantissa_bit);
	mantissa_mask = (mantissa_len - 1);
	mantissa_shift = exponent_shift - mantissa_bit - 1;

	for (i = 0; i < volume_len; i++)
	{
		UINT32 exponent = (i >> mantissa_bit) & exponent_mask;
		UINT32 mantissa = (i & mantissa_mask) | mantissa_len;

		chip->volume_lookup[i] = (mantissa << mantissa_shift) >> (exponent_shift - exponent);
	}
	chip->volume_acc_shift = (INT8)((16 + exponent_mask) - VOLUME_ACC_BIT);
}

/**********************************************************************************************

     get_accum_mask -- get address accumulator mask

***********************************************************************************************/

static void get_accum_mask(ES5506_STATE* chip, UINT32 address_integer, UINT32 address_frac)
{
	chip->address_acc_shift = (INT8)(ADDRESS_FRAC_BIT - address_frac);
	chip->address_acc_mask = lshift_signed64(((((UINT64)1 << address_integer) - 1) << address_frac) |
							(((UINT64)1 << address_frac) - 1), chip->address_acc_shift);
	if (chip->address_acc_shift > 0)
		chip->address_acc_mask |= (((UINT64)1 << chip->address_acc_shift) - 1);
}


/**********************************************************************************************

     interpolate -- interpolate between two samples

***********************************************************************************************/

INLINE INT32 interpolate(ES5506_STATE* chip, INT32 sample1, INT32 sample2, UINT64 accum)
{
	const UINT32 shifted = 1 << ADDRESS_FRAC_BIT;
	const UINT32 mask = shifted - 1;
	accum &= mask & chip->address_acc_mask;
	return (sample1 * (INT32)(shifted - accum) +
			sample2 * (INT32)(accum)) >> ADDRESS_FRAC_BIT;
}


/**********************************************************************************************

     apply_filters -- apply the 4-pole digital filter to the sample

***********************************************************************************************/

// apply lowpass/highpass result
INLINE INT32 apply_lowpass(INT32 out, INT32 cutoff, INT32 in)
{
	return ((INT32)(cutoff >> FILTER_SHIFT) * (out - in) / (1 << FILTER_BIT)) + in;
}

INLINE INT32 apply_highpass(INT32 out, INT32 cutoff, INT32 in, INT32 prev)
{
	return out - prev + ((INT32)(cutoff >> FILTER_SHIFT) * in) / (1 << (FILTER_BIT + 1)) + in / 2;
}

INLINE void apply_filters(ES5506_STATE* chip, ES550X_VOICE* voice, INT32* sample)
{
	INT32 k1 = (INT32)voice->k1;
	INT32 k2 = (INT32)voice->k2;

	// pole 1 is always low-pass using K1
	*sample = apply_lowpass(*sample, k1, voice->o1n1);
	voice->o1n1 = *sample;

	// pole 2 is always low-pass using K1
	*sample = apply_lowpass(*sample, k1, voice->o2n1);
	voice->o2n2 = voice->o2n1;
	voice->o2n1 = *sample;

	// remaining poles depend on the current filter setting
	switch (get_lp(chip, voice->control))
	{
	case 0:
		// pole 3 is high-pass using K2
		*sample = apply_highpass(*sample, k2, voice->o3n1, voice->o2n2);
		voice->o3n2 = voice->o3n1;
		voice->o3n1 = *sample;

		// pole 4 is high-pass using K2
		*sample = apply_highpass(*sample, k2, voice->o4n1, voice->o3n2);
		voice->o4n1 = *sample;
		break;

	case LP3:
		// pole 3 is low-pass using K1
		*sample = apply_lowpass(*sample, k1, voice->o3n1);
		voice->o3n2 = voice->o3n1;
		voice->o3n1 = *sample;

		// pole 4 is high-pass using K2
		*sample = apply_highpass(*sample, k2, voice->o4n1, voice->o3n2);
		voice->o4n1 = *sample;
		break;

	case LP4:
		// pole 3 is low-pass using K2
		*sample = apply_lowpass(*sample, k2, voice->o3n1);
		voice->o3n2 = voice->o3n1;
		voice->o3n1 = *sample;

		// pole 4 is low-pass using K2
		*sample = apply_lowpass(*sample, k2, voice->o4n1);
		voice->o4n1 = *sample;
		break;

	case LP3 | LP4:
		// pole 3 is low-pass using K1
		*sample = apply_lowpass(*sample, k1, voice->o3n1);
		voice->o3n2 = voice->o3n1;
		voice->o3n1 = *sample;

		// pole 4 is low-pass using K2
		*sample = apply_lowpass(*sample, k2, voice->o4n1);
		voice->o4n1 = *sample;
		break;
	}
}


/**********************************************************************************************

     update_envelopes -- update the envelopes

***********************************************************************************************/

static void update_envelopes(ES5506_STATE* chip, ES550X_VOICE* voice)
{
	const UINT32 volume_max = (1 << VOLUME_BIT_ES5506) - 1;

	if (! chip->sndtype)
	{
		// no envelopes in ES5505
		voice->ecount = 0;
		return;
	}

	// decrement the envelope counter
	voice->ecount--;

	// ramp left volume
	if (voice->lvramp)
	{
		voice->lvol += (INT8)voice->lvramp;
		if ((INT32)voice->lvol < 0) voice->lvol = 0;
		else if (voice->lvol > volume_max) voice->lvol = volume_max;
	}

	// ramp right volume
	if (voice->rvramp)
	{
		voice->rvol += (INT8)voice->rvramp;
		if ((INT32)voice->rvol < 0) voice->rvol = 0;
		else if (voice->rvol > volume_max) voice->rvol = volume_max;
	}

	// ramp k1 filter constant
	if (voice->k1ramp && ((INT32)voice->k1ramp >= 0 || !(voice->filtcount & 7)))
	{
		voice->k1 += (INT8)voice->k1ramp;
		if ((INT32)voice->k1 < 0) voice->k1 = 0;
		else if (voice->k1 > 0xffff) voice->k1 = 0xffff;
	}

	// ramp k2 filter constant
	if (voice->k2ramp && ((INT32)voice->k2ramp >= 0 || !(voice->filtcount & 7)))
	{
		voice->k2 += (INT8)voice->k2ramp;
		if ((INT32)voice->k2 < 0) voice->k2 = 0;
		else if (voice->k2 > 0xffff) voice->k2 = 0xffff;
	}

	// update the filter constant counter
	voice->filtcount++;
}


/**********************************************************************************************

     check_for_end_forward
     check_for_end_reverse -- check for loop end and loop appropriately

***********************************************************************************************/

static void check_for_end_forward(ES5506_STATE* chip, ES550X_VOICE* voice, UINT64* accum)
{
	// are we past the end?
	if (chip->sndtype)
	{
		if (*accum > voice->end && !(voice->control & CONTROL_LEI))
		{
			// generate interrupt if required
			if (voice->control & CONTROL_IRQE)
				voice->control |= CONTROL_IRQ;

			// handle the different types of looping
			switch (voice->control & CONTROL_LOOPMASK)
			{
			case 0:	// non-looping
				voice->control |= CONTROL_STOP0;
				break;
			case CONTROL_LPE:	// uni-directional looping
				*accum = (voice->start + (*accum - voice->end)) & chip->address_acc_mask;
				break;
			case CONTROL_BLE:	// trans-wave looping
				*accum = (voice->start + (*accum - voice->end)) & chip->address_acc_mask;
				voice->control = (voice->control & ~CONTROL_LOOPMASK) | CONTROL_LEI;
				break;
			case CONTROL_LPE | CONTROL_BLE:	// bi-directional looping
				*accum = (voice->end - (*accum - voice->end)) & chip->address_acc_mask;
				voice->control ^= CONTROL_DIR;
				break;
			}
		}
	}
	else
	{
		// ES5505 : BLE is ignored when LPE = 0
		if (*accum > voice->end)
		{
			if (voice->control & CONTROL_IRQE)
				voice->control |= CONTROL_IRQ;

			switch (voice->control & CONTROL_LOOPMASK)
			{
			case 0:	// non-looping
			case CONTROL_BLE:
				voice->control |= CONTROL_STOP0;
				break;
			case CONTROL_LPE:	// uni-directional looping
				*accum = (voice->start + (*accum - voice->end)) & chip->address_acc_mask;
				break;
			case CONTROL_LPE | CONTROL_BLE:	// bi-directional looping
				*accum = (voice->end - (*accum - voice->end)) & chip->address_acc_mask;
				voice->control ^= CONTROL_DIR;
				break;
			}
		}
	}
}

static void check_for_end_reverse(ES5506_STATE* chip, ES550X_VOICE* voice, UINT64* accum)
{
	if (chip->sndtype)
	{
		if (*accum < voice->start && !(voice->control & CONTROL_LEI))
		{
			if (voice->control & CONTROL_IRQE)
				voice->control |= CONTROL_IRQ;

			switch (voice->control & CONTROL_LOOPMASK)
			{
			case 0:	// non-looping
				voice->control |= CONTROL_STOP0;
				break;
			case CONTROL_LPE:	// uni-directional looping
				*accum = (voice->end - (voice->start - *accum)) & chip->address_acc_mask;
				break;
			case CONTROL_BLE:	// trans-wave looping
				*accum = (voice->end - (voice->start - *accum)) & chip->address_acc_mask;
				voice->control = (voice->control & ~CONTROL_LOOPMASK) | CONTROL_LEI;
				break;
			case CONTROL_LPE | CONTROL_BLE:	// bi-directional looping
				*accum = (voice->start + (voice->start - *accum)) & chip->address_acc_mask;
				voice->control ^= CONTROL_DIR;
				break;
			}
		}
	}
	else
	{
		if (*accum < voice->start)
		{
			if (voice->control & CONTROL_IRQE)
				voice->control |= CONTROL_IRQ;

			switch (voice->control & CONTROL_LOOPMASK)
			{
			case 0:	// non-looping
			case CONTROL_BLE:
				voice->control |= CONTROL_STOP0;
				break;
			case CONTROL_LPE:	// uni-directional looping
				*accum = (voice->end - (voice->start - *accum)) & chip->address_acc_mask;
				break;
			case CONTROL_LPE | CONTROL_BLE:	// bi-directional looping
				*accum = (voice->start + (voice->start - *accum)) & chip->address_acc_mask;
				voice->control ^= CONTROL_DIR;
				break;
			}
		}
	}
}


/**********************************************************************************************

     generate_voice -- general u-law / PCM decoding routine (one output sample)

***********************************************************************************************/

static void generate_voice(ES5506_STATE* chip, ES550X_VOICE* voice, INT32* dest)
{
	const UINT64 freqcount = voice->freqcount;
	UINT64 accum = voice->accum & chip->address_acc_mask;
	UINT8 ulaw = chip->sndtype && (voice->control & CONTROL_CMPD);

	if (!(voice->control & CONTROL_STOPMASK))
	{
		INT32 val1, val2;
		UINT8 reverse = (voice->control & CONTROL_DIR) ? 1 : 0;

		// fetch two samples
		val1 = read_sample(chip, voice, (UINT32)get_integer_addr(chip, accum, 0));
		val2 = read_sample(chip, voice, (UINT32)get_integer_addr(chip, accum, 1));
		if (ulaw)
		{
			// decompress u-law
			val1 = chip->ulaw_lookup[val1 >> (16 - ULAW_MAXBITS)];
			val2 = chip->ulaw_lookup[val2 >> (16 - ULAW_MAXBITS)];
		}
		else
		{
			val1 = (INT16)val1;
			val2 = (INT16)val2;
		}

		// interpolate
		val1 = interpolate(chip, val1, val2, accum);
		if (! reverse)
			accum = (accum + freqcount) & chip->address_acc_mask;
		else
			accum = (accum - freqcount) & chip->address_acc_mask;

		// apply filters
		apply_filters(chip, voice, &val1);

		// update filters/volumes
		if (voice->ecount != 0)
			update_envelopes(chip, voice);

		// apply volumes and add
		dest[0] += get_sample(chip, val1, voice->lvol);
		dest[1] += get_sample(chip, val1, voice->rvol);

		// check for loop end
		if (! reverse)
			check_for_end_forward(chip, voice, &accum);
		else
			check_for_end_reverse(chip, voice, &accum);
	}
	else
	{
		// if we stopped, process any additional envelope
		if (voice->ecount != 0)
			update_envelopes(chip, voice);
	}

	voice->accum = accum;
}


/**********************************************************************************************

     es5506_update -- update the sound chip so that it is in sync with CPU execution

***********************************************************************************************/

static void es5506_update(void* info, UINT32 samples, DEV_SMPL** outputs)
{
	ES5506_STATE* chip = (ES5506_STATE*)info;
	UINT32 smpl;
	int v, c;
	INT32 cursample[12];
	INT32 dummy[2];
	const INT32 clampMax = (1 << 19) - 1;
	const INT32 clampMin = -(1 << 19);

	for (smpl = 0; smpl < samples; smpl++)
	{
		INT32 outL = 0;
		INT32 outR = 0;

		memset(cursample, 0, sizeof(cursample));
		for (v = 0; v <= chip->active_voices; v++)
		{
			ES550X_VOICE* voice = &chip->voice[v];
			int channel;

			// ES5506: special case: if end == start, stop the voice
			// (MAME no longer does this for the ES5505: Ensoniq synths keep transwaves running that way)
			if (chip->sndtype && voice->start == voice->end)
				voice->control |= CONTROL_STOP0;

			channel = get_ca(chip, voice->control) % chip->channels;
			if (voice->Muted)
			{
				dummy[0] = dummy[1] = 0;
				generate_voice(chip, voice, dummy);
			}
			else
			{
				generate_voice(chip, voice, &cursample[channel << 1]);
			}

			// the host never acknowledges voice IRQs here: just take the IRQ bit down again
			voice->control &= ~CONTROL_IRQ;
		}

		// generated samples are 20-bit signed per output channel, range is [-2^19 .. 2^19-1];
		// the output channels are then mixed down to stereo
		for (c = 0; c < chip->channels; c++)
		{
			INT32 l = cursample[c * 2 + 0];
			INT32 r = cursample[c * 2 + 1];
			if (l > clampMax) l = clampMax; else if (l < clampMin) l = clampMin;
			if (r > clampMax) r = clampMax; else if (r < clampMin) r = clampMin;
			outL += l;
			outR += r;
		}
		// kept at 20-bit scale: the VGM player's chip volume for the ES5506 (0x20) expects it,
		// as with VGMPlay 0.40.x
		outputs[0][smpl] = outL;
		outputs[1][smpl] = outR;
	}
}


/**********************************************************************************************

     device start / stop / reset

***********************************************************************************************/

static void init_voices(ES5506_STATE* chip)
{
	int j;
	UINT32 muteMask = 0;

	for (j = 0; j < 32; j++)
		muteMask |= (UINT32)chip->voice[j].Muted << j;
	memset(chip->voice, 0, sizeof(chip->voice));
	for (j = 0; j < 32; j++)
	{
		chip->voice[j].index = (UINT8)j;
		chip->voice[j].control = CONTROL_STOPMASK;
		chip->voice[j].lvol = (1 << (chip->volume_bit - 1));
		chip->voice[j].rvol = (1 << (chip->volume_bit - 1));
		chip->voice[j].Muted = (muteMask >> j) & 0x01;
	}
}

static UINT8 device_start_es5506(const DEV_GEN_CFG* cfg, DEV_INFO* retDevInf)
{
	ES5506_STATE* chip;
	UINT8 max_chns;

	chip = (ES5506_STATE*)calloc(1, sizeof(ES5506_STATE));
	if (chip == NULL)
		return 0xFF;

	chip->sndtype = (cfg->clock & 0x80000000) ? 1 : 0;
	chip->master_clock = cfg->clock & 0x7FFFFFFF;
	chip->irqv = 0x80;

	// only override the number of channels if the value is in the valid range
	max_chns = chip->sndtype ? 6 : 4;
	chip->channels = cfg->flags;
	if (chip->channels < 1 || chip->channels > max_chns)
		chip->channels = 1;

	if (chip->sndtype)
	{
		compute_tables(chip, VOLUME_BIT_ES5506, 4, 8);	// 4 bit exponent, 8 bit mantissa
		get_accum_mask(chip, ADDRESS_INTEGER_BIT_ES5506, ADDRESS_FRAC_BIT_ES5506);	// 21 bit integer and 11 bit fraction
	}
	else
	{
		compute_tables(chip, VOLUME_BIT_ES5505, 4, 4);	// 4 bit exponent, 4 bit mantissa
		get_accum_mask(chip, ADDRESS_INTEGER_BIT_ES5505, ADDRESS_FRAC_BIT_ES5505);	// 20 bit integer and 9 bit fraction
	}

	// all 32 voices are active after reset
	chip->active_voices = 0x1f;
	chip->sample_rate = chip->master_clock / (16 * (chip->active_voices + 1));

	es5506_set_mute_mask(chip, 0x00000000);
	init_voices(chip);

	chip->_devData.chipInf = chip;
	INIT_DEVINF(retDevInf, &chip->_devData, chip->sample_rate, &devDef);
	return 0x00;
}

static void device_stop_es5506(void* info)
{
	ES5506_STATE* chip = (ES5506_STATE*)info;
	int i;

	for (i = 0; i < 4; i++)
		free(chip->region_base[i]);
	free(chip);
}

static void device_reset_es5506(void* info)
{
	ES5506_STATE* chip = (ES5506_STATE*)info;

	init_voices(chip);
	chip->current_page = 0;
	chip->irqv = 0x80;
	chip->write_latch = 0;
	chip->mode = chip->sndtype ? 0x17 : 0;	// ES5506: Dual, Slave, BCLK_EN, WCLK_EN, LRCLK_EN high
	chip->active_voices = 0x1f;
	set_sample_rate(chip);
}


/**********************************************************************************************

     reg_write -- handle a write to the selected ES5506 register

***********************************************************************************************/

static void es5506_reg_write_low(ES5506_STATE* chip, ES550X_VOICE* voice, UINT8 offset, UINT32 data)
{
	switch (offset)
	{
	case 0x00/8:	// CR
		voice->control = data & 0xffff;
		break;
	case 0x08/8:	// FC
		voice->freqcount = get_address_acc_shifted_val(chip, data & 0x1ffff, 0);
		break;
	case 0x10/8:	// LVOL
		voice->lvol = data & 0xffff;	// low 4 bit is used for finer envelope control
		break;
	case 0x18/8:	// LVRAMP
		voice->lvramp = (data & 0xff00) >> 8;
		break;
	case 0x20/8:	// RVOL
		voice->rvol = data & 0xffff;
		break;
	case 0x28/8:	// RVRAMP
		voice->rvramp = (data & 0xff00) >> 8;
		break;
	case 0x30/8:	// ECOUNT
		voice->ecount = data & 0x1ff;
		voice->filtcount = 0;
		break;
	case 0x38/8:	// K2
		voice->k2 = data & 0xffff;
		break;
	case 0x40/8:	// K2RAMP
		voice->k2ramp = ((data & 0xff00) >> 8) | ((data & 0x0001) << 31);
		break;
	case 0x48/8:	// K1
		voice->k1 = data & 0xffff;
		break;
	case 0x50/8:	// K1RAMP
		voice->k1ramp = ((data & 0xff00) >> 8) | ((data & 0x0001) << 31);
		break;
	case 0x58/8:	// ACTV
		chip->active_voices = data & 0x1f;
		set_sample_rate(chip);
		break;
	case 0x60/8:	// MODE
		chip->mode = data & 0x1f;
		break;
	case 0x68/8:	// PAR - read only
	case 0x70/8:	// IRQV - read only
		break;
	case 0x78/8:	// PAGE
		chip->current_page = data & 0x7f;
		break;
	}
}

static void es5506_reg_write_high(ES5506_STATE* chip, ES550X_VOICE* voice, UINT8 offset, UINT32 data)
{
	switch (offset)
	{
	case 0x00/8:	// CR
		voice->control = data & 0xffff;
		break;
	case 0x08/8:	// START
		voice->start = get_address_acc_shifted_val(chip, data & 0xfffff800, 0);
		break;
	case 0x10/8:	// END
		voice->end = get_address_acc_shifted_val(chip, data & 0xffffff80, 0);
		break;
	case 0x18/8:	// ACCUM
		voice->accum = get_address_acc_shifted_val(chip, data, 0);
		break;
	case 0x20/8:	// O4(n-1)
		voice->o4n1 = sext18(data);
		break;
	case 0x28/8:	// O3(n-1)
		voice->o3n1 = sext18(data);
		break;
	case 0x30/8:	// O3(n-2)
		voice->o3n2 = sext18(data);
		break;
	case 0x38/8:	// O2(n-1)
		voice->o2n1 = sext18(data);
		break;
	case 0x40/8:	// O2(n-2)
		voice->o2n2 = sext18(data);
		break;
	case 0x48/8:	// O1(n-1)
		voice->o1n1 = sext18(data);
		break;
	case 0x50/8:	// W_ST
		chip->wst = data & 0x7f;
		break;
	case 0x58/8:	// W_END
		chip->wend = data & 0x7f;
		break;
	case 0x60/8:	// LR_END
		chip->lrend = data & 0x7f;
		break;
	case 0x68/8:	// PAR - read only
	case 0x70/8:	// IRQV - read only
		break;
	case 0x78/8:	// PAGE
		chip->current_page = data & 0x7f;
		break;
	}
}

static void es5506_reg_write_test(ES5506_STATE* chip, ES550X_VOICE* voice, UINT8 offset, UINT32 data)
{
	if (offset == 0x78/8)	// PAGE
		chip->current_page = data & 0x7f;
}

static void es5506_w(ES5506_STATE* chip, UINT8 offset, UINT8 data)
{
	ES550X_VOICE* voice = &chip->voice[chip->current_page & 0x1f];
	int shift = 8 * (offset & 3);

	// accumulate the data
	chip->write_latch = (chip->write_latch & ~(0xff000000 >> shift)) | ((UINT32)data << (24 - shift));

	// wait for a write to complete
	if (shift != 24)
		return;

	// switch off the page and register
	if (chip->current_page < 0x20)
		es5506_reg_write_low(chip, voice, offset / 4, chip->write_latch);
	else if (chip->current_page < 0x40)
		es5506_reg_write_high(chip, voice, offset / 4, chip->write_latch);
	else
		es5506_reg_write_test(chip, voice, offset / 4, chip->write_latch);

	// clear the write latch when done
	chip->write_latch = 0;
}


/**********************************************************************************************

     reg_write -- handle a write to the selected ES5505 register

***********************************************************************************************/

#define ACCESSING_BITS_0_7	(mem_mask & 0x00ff)
#define ACCESSING_BITS_8_15	(mem_mask & 0xff00)

static void es5505_reg_write_low(ES5506_STATE* chip, ES550X_VOICE* voice, UINT8 offset, UINT16 data, UINT16 mem_mask)
{
	switch (offset)
	{
	case 0x00:	// CR
		voice->control |= 0xf000;	// bit 15-12 always 1
		if (ACCESSING_BITS_0_7)
		{
			voice->control &= ~0x00ff;
			voice->control |= (data & 0x00ff);
		}
		if (ACCESSING_BITS_8_15)
			voice->control = (voice->control & ~0x0f00) | (data & 0x0f00);
		break;
	case 0x01:	// FC
		if (ACCESSING_BITS_0_7)
			voice->freqcount = (voice->freqcount & ~get_address_acc_shifted_val(chip, 0x00fe, 1)) | (get_address_acc_shifted_val(chip, (data & 0x00fe), 1));
		if (ACCESSING_BITS_8_15)
			voice->freqcount = (voice->freqcount & ~get_address_acc_shifted_val(chip, 0xff00, 1)) | (get_address_acc_shifted_val(chip, (data & 0xff00), 1));
		break;
	case 0x02:	// STRT (hi)
		if (ACCESSING_BITS_0_7)
			voice->start = (voice->start & ~get_address_acc_shifted_val(chip, 0x00ff0000, 0)) | (get_address_acc_shifted_val(chip, (UINT32)(data & 0x00ff) << 16, 0));
		if (ACCESSING_BITS_8_15)
			voice->start = (voice->start & ~get_address_acc_shifted_val(chip, 0x1f000000, 0)) | (get_address_acc_shifted_val(chip, (UINT32)(data & 0x1f00) << 16, 0));
		break;
	case 0x03:	// STRT (lo)
		if (ACCESSING_BITS_0_7)
			voice->start = (voice->start & ~get_address_acc_shifted_val(chip, 0x000000e0, 0)) | (get_address_acc_shifted_val(chip, data & 0x00e0, 0));
		if (ACCESSING_BITS_8_15)
			voice->start = (voice->start & ~get_address_acc_shifted_val(chip, 0x0000ff00, 0)) | (get_address_acc_shifted_val(chip, data & 0xff00, 0));
		break;
	case 0x04:	// END (hi)
		if (ACCESSING_BITS_0_7)
			voice->end = (voice->end & ~get_address_acc_shifted_val(chip, 0x00ff0000, 0)) | (get_address_acc_shifted_val(chip, (UINT32)(data & 0x00ff) << 16, 0));
		if (ACCESSING_BITS_8_15)
			voice->end = (voice->end & ~get_address_acc_shifted_val(chip, 0x1f000000, 0)) | (get_address_acc_shifted_val(chip, (UINT32)(data & 0x1f00) << 16, 0));
		break;
	case 0x05:	// END (lo)
		if (ACCESSING_BITS_0_7)
			voice->end = (voice->end & ~get_address_acc_shifted_val(chip, 0x000000e0, 0)) | (get_address_acc_shifted_val(chip, data & 0x00e0, 0));
		if (ACCESSING_BITS_8_15)
			voice->end = (voice->end & ~get_address_acc_shifted_val(chip, 0x0000ff00, 0)) | (get_address_acc_shifted_val(chip, data & 0xff00, 0));
		break;
	case 0x06:	// K2
		if (ACCESSING_BITS_0_7)
			voice->k2 = (voice->k2 & ~0x00f0) | (data & 0x00f0);
		if (ACCESSING_BITS_8_15)
			voice->k2 = (voice->k2 & ~0xff00) | (data & 0xff00);
		break;
	case 0x07:	// K1
		if (ACCESSING_BITS_0_7)
			voice->k1 = (voice->k1 & ~0x00f0) | (data & 0x00f0);
		if (ACCESSING_BITS_8_15)
			voice->k1 = (voice->k1 & ~0xff00) | (data & 0xff00);
		break;
	case 0x08:	// LVOL
		if (ACCESSING_BITS_8_15)
			voice->lvol = (voice->lvol & ~0xff) | ((data & 0xff00) >> 8);
		break;
	case 0x09:	// RVOL
		if (ACCESSING_BITS_8_15)
			voice->rvol = (voice->rvol & ~0xff) | ((data & 0xff00) >> 8);
		break;
	case 0x0a:	// ACC (hi)
		if (ACCESSING_BITS_0_7)
			voice->accum = (voice->accum & ~get_address_acc_shifted_val(chip, 0x00ff0000, 0)) | (get_address_acc_shifted_val(chip, (UINT32)(data & 0x00ff) << 16, 0));
		if (ACCESSING_BITS_8_15)
			voice->accum = (voice->accum & ~get_address_acc_shifted_val(chip, 0x1f000000, 0)) | (get_address_acc_shifted_val(chip, (UINT32)(data & 0x1f00) << 16, 0));
		break;
	case 0x0b:	// ACC (lo)
		if (ACCESSING_BITS_0_7)
			voice->accum = (voice->accum & ~get_address_acc_shifted_val(chip, 0x000000ff, 0)) | (get_address_acc_shifted_val(chip, data & 0x00ff, 0));
		if (ACCESSING_BITS_8_15)
			voice->accum = (voice->accum & ~get_address_acc_shifted_val(chip, 0x0000ff00, 0)) | (get_address_acc_shifted_val(chip, data & 0xff00, 0));
		break;
	case 0x0c:	// unused
		break;
	case 0x0d:	// ACT
		if (ACCESSING_BITS_0_7)
		{
			chip->active_voices = data & 0x1f;
			set_sample_rate(chip);
		}
		break;
	case 0x0e:	// IRQV - read only
		break;
	case 0x0f:	// PAGE
		if (ACCESSING_BITS_0_7)
			chip->current_page = data & 0x7f;
		break;
	}
}

#define WRITE_POLE(pole)	\
	if (ACCESSING_BITS_0_7)	\
		pole = (pole & ~0x00ff) | (data & 0x00ff);	\
	if (ACCESSING_BITS_8_15)	\
		pole = (INT16)((pole & ~0xff00) | (data & 0xff00));

static void es5505_reg_write_high(ES5506_STATE* chip, ES550X_VOICE* voice, UINT8 offset, UINT16 data, UINT16 mem_mask)
{
	switch (offset)
	{
	case 0x00:	// CR
		voice->control |= 0xf000;	// bit 15-12 always 1
		if (ACCESSING_BITS_0_7)
			voice->control = (voice->control & ~0x00ff) | (data & 0x00ff);
		if (ACCESSING_BITS_8_15)
			voice->control = (voice->control & ~0x0f00) | (data & 0x0f00);
		break;
	case 0x01:	// O4(n-1)
		WRITE_POLE(voice->o4n1)
		break;
	case 0x02:	// O3(n-1)
		WRITE_POLE(voice->o3n1)
		break;
	case 0x03:	// O3(n-2)
		WRITE_POLE(voice->o3n2)
		break;
	case 0x04:	// O2(n-1)
		WRITE_POLE(voice->o2n1)
		break;
	case 0x05:	// O2(n-2)
		WRITE_POLE(voice->o2n2)
		break;
	case 0x06:	// O1(n-1)
		WRITE_POLE(voice->o1n1)
		break;
	case 0x0d:	// ACT
		if (ACCESSING_BITS_0_7)
		{
			chip->active_voices = data & 0x1f;
			set_sample_rate(chip);
		}
		break;
	case 0x0f:	// PAGE
		if (ACCESSING_BITS_0_7)
			chip->current_page = data & 0x7f;
		break;
	default:	// 0x07..0x0c unused, 0x0e IRQV read only
		break;
	}
}

static void es5505_reg_write_test(ES5506_STATE* chip, ES550X_VOICE* voice, UINT8 offset, UINT16 data, UINT16 mem_mask)
{
	switch (offset)
	{
	case 0x08:	// SERMODE
		chip->mode |= 0x7f8;	// bit 10-3 always 1
		if (ACCESSING_BITS_8_15)
			chip->mode = (chip->mode & ~0xf800) | (data & 0xf800);
		if (ACCESSING_BITS_0_7)
			chip->mode = (chip->mode & ~0x0007) | (data & 0x0007);
		break;
	case 0x0d:	// ACT
		if (ACCESSING_BITS_0_7)
		{
			chip->active_voices = data & 0x1f;
			set_sample_rate(chip);
		}
		break;
	case 0x0f:	// PAGE
		if (ACCESSING_BITS_0_7)
			chip->current_page = data & 0x7f;
		break;
	default:	// 0x00..0x07 CHnL/R, 0x09 PAR, 0x0e IRQV
		break;
	}
}

static void es5505_w(ES5506_STATE* chip, UINT8 offset, UINT16 data, UINT16 mem_mask)
{
	ES550X_VOICE* voice = &chip->voice[chip->current_page & 0x1f];

	// switch off the page and register
	if (chip->current_page < 0x20)
		es5505_reg_write_low(chip, voice, offset, data, mem_mask);
	else if (chip->current_page < 0x40)
		es5505_reg_write_high(chip, voice, offset, data, mem_mask);
	else
		es5505_reg_write_test(chip, voice, offset, data, mem_mask);
}


/**********************************************************************************************

     VGM interface

***********************************************************************************************/

static void es550x_w(void* info, UINT8 offset, UINT8 data)
{
	ES5506_STATE* chip = (ES5506_STATE*)info;

	if (offset < 0x40)
	{
		if (! chip->sndtype)
		{
			// byte 0 = bits 15-8, byte 1 = bits 7-0
			if (! (offset & 0x01))
				es5505_w(chip, offset >> 1, (UINT16)data << 8, 0xff00);
			else
				es5505_w(chip, offset >> 1, data, 0x00ff);
		}
		else
		{
			es5506_w(chip, offset, data);
		}
	}
	else
	{
		chip->voice[offset & 0x1F].exbank = (UINT32)data << 20;
	}
}

static void es550x_w16(void* info, UINT8 offset, UINT16 data)
{
	ES5506_STATE* chip = (ES5506_STATE*)info;

	if (offset < 0x40)
	{
		if (! chip->sndtype)
		{
			es5505_w(chip, offset >> 1, data, 0xffff);
		}
		else
		{
			es5506_w(chip, offset | 0, (data & 0xFF00) >> 8);
			es5506_w(chip, offset | 1, (data & 0x00FF) >> 0);
		}
	}
	else
	{
		chip->voice[offset & 0x1F].exbank = (UINT32)data << 20;
	}
}

static void es5506_alloc_rom(void* info, UINT32 memsize)
{
	ES5506_STATE* chip = (ES5506_STATE*)info;

	// The region is only known from the data block's start offset: allocate in es5506_write_rom.
	chip->pending_rom_size = memsize;
}

static void es5506_write_rom(void* info, UINT32 offset, UINT32 length, const UINT8* data)
{
	ES5506_STATE* chip = (ES5506_STATE*)info;
	UINT8 curRgn = (offset >> 28) & 0x03;
	UINT8 is8bROM = (offset >> 31) & 0x01;
	UINT32 romWords;
	UINT32 curPos;

	offset &= 0x0FFFFFFF;
	// 8-bit ROM: one byte per sample word, 16-bit ROM: two bytes (big endian is not used by VGM: little endian)
	romWords = is8bROM ? chip->pending_rom_size : (chip->pending_rom_size / 2);
	if (romWords && chip->region_size[curRgn] != romWords)
	{
		chip->region_base[curRgn] = (UINT16*)realloc(chip->region_base[curRgn], romWords * sizeof(UINT16));
		chip->region_size[curRgn] = romWords;
		memset(chip->region_base[curRgn], 0x00, romWords * sizeof(UINT16));
	}
	if (chip->region_base[curRgn] == NULL)
		return;

	if (is8bROM)
	{
		if (offset >= chip->region_size[curRgn])
			return;
		if (offset + length > chip->region_size[curRgn])
			length = chip->region_size[curRgn] - offset;
		for (curPos = 0; curPos < length; curPos++)
			chip->region_base[curRgn][offset + curPos] = (UINT16)data[curPos] << 8;
	}
	else
	{
		offset /= 2;
		length /= 2;
		if (offset >= chip->region_size[curRgn])
			return;
		if (offset + length > chip->region_size[curRgn])
			length = chip->region_size[curRgn] - offset;
		for (curPos = 0; curPos < length; curPos++)
			chip->region_base[curRgn][offset + curPos] = (UINT16)(data[curPos * 2 + 0] | (data[curPos * 2 + 1] << 8));
	}
}

static void es5506_set_mute_mask(void* info, UINT32 MuteMask)
{
	ES5506_STATE* chip = (ES5506_STATE*)info;
	UINT8 CurChn;

	for (CurChn = 0; CurChn < 32; CurChn++)
		chip->voice[CurChn].Muted = (MuteMask >> CurChn) & 0x01;
}

static void es5506_set_srchg_cb(void* info, DEVCB_SRATE_CHG CallbackFunc, void* DataPtr)
{
	ES5506_STATE* chip = (ES5506_STATE*)info;

	// set Sample Rate Change Callback routine
	chip->SmpRateFunc = CallbackFunc;
	chip->SmpRateData = DataPtr;
}
