# soltune/libvgm fork

Upstream is ValleyBell/libvgm (remote `upstream`). This fork adds chips and fixes on top of it.

## Branches

| branch | what | upstreamable |
|---|---|---|
| `fix/gb-noise-envelope-retrigger` | Game Boy (MAME core) noise / envelope fixes | yes |
| `es5505` | ES5505/ES5506 core (Taito F3), 0xD6 read as big-endian, ROM size | yes |
| `zoom` | Taito Zoom: ZSG-2 PCM + TMS57002 DSP (Ray Storm, RayCrisis, G-NET/FX-1B) | not yet (provisional numbers) |
| `fork-tools` | this file, `sync.sh`, `regress.py` | no |
| `chip-player` | **integration branch for my own use**: upstream/master + `--no-ff` merges of the above | no |

Each feature branch is a short series rebased on `upstream/master`, so it can be offered upstream as
is. `chip-player` is never edited by hand: `fork/sync.sh` recreates it (and tags the previous one
`backup/chip-player-*`), so pushing it needs `git push --force origin chip-player`.
Fix things on the feature branch, then run `fork/sync.sh`.

## Updating from upstream

    fork/sync.sh          # fetch upstream, rebase every branch, rebuild chip-player, build, regress

On a conflict the script stops in the middle of the rebase or merge. The usual conflict is a chip
table in `player/vgmplayer*.cpp` / `emu/SoundDevs.h`: keep upstream's lines and re-append ours at
the end, then `git rebase --continue` (or commit the merge) and run the script again.

`fork/regress.py` renders `fork/regress.list` and compares with the references in
`~/tools/libvgm-regress` (`--update` to accept the current output).

## ZSG-2 numbering (provisional)

The ZSG-2 is not in the VGM spec. Its numbers sit at the far end of each range, away from where
upstream allocates new chips, so an upstream addition can never be mistaken for it:

| | ZSG-2 | upstream's next free (2026-10) |
|---|---|---|
| header clock | 0x1FC (header 0x200 bytes) | 0x100 |
| write command | 0xFE aa aa dd dd | 0xE2 |
| ROM data block | 0xBF | 0x97 |
| libvgm DEVID / VGM chip index | 0x30, last in the tables | internal only, not stored in files |

The DEVID and the chip index are not in the files: when upstream adds a chip, move ours to the next
index. If the chip is ever given official numbers, renumber the finished files with
`makevgm/mame/zoom/tools/zoom_remap.py --to HDR:CMD:BLK` (no re-capture).
