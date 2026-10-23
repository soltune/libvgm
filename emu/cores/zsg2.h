#ifndef __ZSG2_H__
#define __ZSG2_H__

#include "../EmuStructs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Taito Zoom sound board: ZOOM ZSG-2 PCM + TMS57002 "DASP" effects DSP
//
// register writes (DEVRW_A16D16):
//   0x000-0x3FF  ZSG-2 word registers (MAME zsg2_device::write offset)
//   0x400        TMS57002 host data port (low 8 bits)
//   0x401        TMS57002 control (MN10200 port 1: bit 0 = PLOAD line, bit 1 = CLOAD line, active low)
//   0x480-0x4FF  Zoom board register (main CPU reg_data_w; 0x484/0x485 = output gain L/R)
// option bit 0: bypass the DSP (output the ZSG-2 direct L/R mix)
//
// VGM (fork only, PROVISIONAL numbers - the chip is not in the VGM spec):
//   header 0x1FC = clock (25 MHz), so the header is 0x200 bytes long
//   command 0xFE aa aa dd dd = register write (address and data big-endian)
//   data block type 0xBF = sample ROM (32-bit little-endian words)
// The numbers sit at the far end of each range so that chips added upstream
// (which are allocated from the low end) do not get mistaken for this one.
extern const DEV_DECL sndDev_ZSG2;

#ifdef __cplusplus
}
#endif

#endif	// __ZSG2_H__
