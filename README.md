# libvgm (soltune fork)

A fork of [ValleyBell/libvgm](https://github.com/ValleyBell/libvgm). Use the branch **`chip-player`**:
upstream/master with the branches below merged on top, rebuilt whenever upstream moves.

## Differences from upstream

- **ES5505 / ES5506** (Ensoniq OTIS, Taito F3 etc.): new core ported from current MAME; the 0xD6 write is read
  big-endian as the VGM spec says, and the ROM size is taken from the data block. Upstream plays these silent.
  (branch `es5505`)
- **Taito Zoom: ZSG-2 PCM + TMS57002 DSP** (Ray Storm, RayCrisis, Taito FX-1B / G-NET): new cores ported from
  MAME 0.288. **Not in the VGM spec — fork-only, provisional numbers:** header clock at 0x1FC (header 0x200
  bytes), command `0xFE aa aa dd dd` (big-endian), sample ROM data block 0xBF. Files using them play only here.
  (branch `zoom`)
- **Game Boy (MAME core):** the noise LFSR no longer locks up in 7-bit mode, and a triggered envelope waits a full period.
  (branch `fix/gb-noise-envelope-retrigger`)
- **Fork maintenance:** `fork/FORK.md`, `fork/sync.sh` (take in upstream, rebuild `chip-player`),
  `fork/regress.py` (render-and-compare check). (branch `fork-tools`)

Build as upstream does: `cmake -B build && cmake --build build`.
