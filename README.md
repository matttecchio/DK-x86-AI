# DK-x86-AI

A Donkey Kong arcade emulator for x86 that runs **cycle-for-cycle identical to MAME 0.264**, and the tool-assisted search that uses it.

`dkx` emulates the original `dkong` board (TKG-04, two-board kit) with no video or sound. The game, its random numbers and its timing behave exactly as in MAME. That means a route found with dkx plays back the same way in MAME, so it can be used for real runs and recordings. dkx is several times faster than MAME, and a save state is a memory copy rather than a file.

## Results

| Check | Result |
|---|---|
| Scheduler events, first 180 frames from power-on, against an instrumented MAME build | **591,906 events identical**: every timeslice, every CPU's cycle count, every timer |
| Z80 registers, input ports and all RAM, every frame for 3,000 frames (attract, coin, start, play) | **identical** |
| A complete 2,010,900-point game, 848,483 frames, power-on to GAME OVER on the L22 kill screen | **byte-identical** score and board-start trace to stock MAME |
| Each of the 120 board transitions of that game, replayed separately from saved states | **120 / 120 match** MAME (score, lives, level, timer, clear frame) |
| A route found by the dkx search, replayed in stock MAME | same score and clear frame |
| Snapshot restored mid-game | continues bit-identically |

Speed on a Ryzen 9 7900X (22 threads available):

| | MAME 0.264 | dkx |
|---|---|---|
| Emulation, one core | ~2,800 frames/s | **~10,300 frames/s** (170× real time) |
| Search workload, one core (restore state, play 60 frames of scripted input, repeat) | 1,615 frames/s | **~10,000 frames/s** (6.5×) |
| Full search, 20–22 workers, barrel board L21 | 109–129 rollouts/s | **~950–1,090 rollouts/s** (8–9×) |
| Save state | ~1.8 KB file via MAME's save system | 1.5 KB zlib file; restoring in memory takes 65 ns |

## Why exactness is hard here

While Donkey Kong waits for each frame's interrupt, its main loop increments the random seed (`$6019`) about every 202 CPU cycles. Every frame, an Intel 8257 DMA chip copies sprite RAM and stalls the Z80 for about 3,070 cycles. In MAME that stall is 1–5 cycles longer or shorter depending on how MAME's scheduler slices time between the Z80, the DMA chip and the sound CPU (an MB8884, 8035-class). A 2-cycle error changes the seed in about 3% of frames, so an emulator that is merely *very close* sends the barrels and fireballs down a different path within half a second.

So dkx doesn't approximate MAME. It reproduces the parts of MAME that decide timing:

- **Z80 and sound CPU cores:** generated mechanically from MAME's `z80.cpp` and `mcs48.cpp` (`dkx/gen/`). The opcode code is copied verbatim; only memory access and callbacks are rewired.
- **8257 DMA:** ported line for line, including how it uses the scheduler (triggers, suspend-until-trigger, timeslice aborts, `eat_all_cycles`).
- **Scheduler:** MAME's `device_scheduler` and `device_execute_interface` behaviour. That covers:
  - timeslices, the quantum list and the "perfect interleave" quantum;
  - the timer list with its tie-breaking order;
  - synchronised input-line changes and suspend / eat-cycles;
  - execute-list reordering;
  - the subtle difference between the cycle↔time conversions used for slicing and for `local_time()`.

  All of it uses MAME's exact attosecond arithmetic.
- **Board:** the memory map, the three sound latches (writes are synchronised exactly as in MAME), the sound-CPU glue, the screen's vblank and scanline timers, and the per-scanline driver timer.

All machine state is one flat struct, so a snapshot is a `memcpy`.

## Layout

```
dkx/        the emulator and command-line tool
  core.h sched.h dk.h dk.cpp i8257.h z80.* mcs48.* unity.cpp
  dkxtool.cpp   dkx mkfresh | advance | seedgen | worker: the same behaviour as the Lua scripts in search/
  play.cpp      replay a whole game from per-frame inputs and write a score/board trace
  trace.cpp     per-frame state records and scheduler log, for diffing against MAME
  snaptest.cpp  snapshot exactness and restore cost      bench.cpp  search-workload throughput
  tools_chain.py  rebuild all board-start states in dkx and check every transition against MAME's
  gen/          generators for z80.* and mcs48.* from a MAME 0.264 source tree
search/     the Go-Explore search: gcoord.py (coordinator), chain.py (board-by-board driver),
            and the MAME Lua scripts (gworker, seedgen, advance, mkfresh, fullrec, playlog)
ref/        MAME-side tracers used for verification (per-frame state, instruction trace, inputs)
patches/    mame0264-schedlog.patch: environment-gated scheduler logging for MAME (SCHEDLOG=<file>)
```

## Using it

You need your own `dkong` ROM set (not included), g++ with C++17, zlib, Python 3 and stock MAME 0.264. MAME stays the final verifier of every route.

```sh
# build
cd dkx && make
mkdir -p roms && unzip /path/to/dkong.zip -d roms      # c_5et_g.bin ... s_3j_b.bin

# first board state, in both MAME (official) and dkx (search)
cd ../search
mame dkong -rompath /path/to/roms -state_directory st -autoboot_script mkfresh.lua \
     -video none -sound none -nothrottle            # with META=meta_000.txt in the environment
META=../dkx/roots/meta_000.txt DKXST=../dkx/roots ../dkx/build/dkx mkfresh

# search every board with dkx workers; MAME verifies each chosen route, and dkx must agree with it exactly
DKX=../dkx/build/dkx DKXROOTS=../dkx/roots DK_ROMPATH=/path/to/roms python3 chain.py
```

Without `DKX`, the same scripts run the original all-MAME search. Settings live in `search/aims.json` and the environment variables documented at the top of `gcoord.py` and `chain.py`.

### dkx modes

All settings come from the environment, with the same names the Lua scripts use.

| Mode | Equivalent | Settings |
|---|---|---|
| `dkx mkfresh` | `mkfresh.lua` | `META`, `DKXST` |
| `dkx advance` | `advance.lua` | `S0 START NEXT PATHF META [SACR] DKXST` |
| `dkx seedgen` | `seedgen.lua` | `BT T0 L0 S0 INP OUT PFX [STEP] STD` |
| `dkx worker` | `gworker.lua` | `WID JOBDIR STD RS L0 T0 BT [S0] [SACR]`; same job/result file protocol, `NS=0` only |

Route files are one character per frame. Subtract 48 from the character code to get a mask: 1 = left, 2 = right, 4 = up, 8 = down, 16 = jump. DK's IN0 port puts right at bit 0 and left at bit 1, and dkx handles the mapping.

### Reproducing the verification

- Apply `patches/mame0264-schedlog.patch` to a MAME 0.264 tree, build it, and run with `SCHEDLOG=file`. Then diff that file against the scheduler log from `dkx/build/trace`.
- `ref/trace.lua` and `ref/portlog.lua` record per-frame state and inputs from stock MAME, for `dkx/build/trace` and `dkx/build/play`.
- One timing detail matters when comparing against MAME: Lua frame hooks run before MAME's own input update in the same frame. So the input that Lua records at frame n is the one that was applied at frame n−1.

## Not included

- No ROMs, save states, traces or input recordings.
- The routes from the 2,010,900 run are not part of this repository.

## License

BSD-3-Clause; see [LICENSE](LICENSE). `dkx` is derived from MAME source files that are themselves BSD-3-Clause. Their authors are credited in `LICENSE` and in the generated files.
