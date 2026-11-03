// dk.h: the Donkey Kong (dkong, TKG-04 two-board) machine: MAME 0.264 dkong2b driver + scheduler, as one flat struct.
#pragma once
#include "sched.h"
#include "z80.h"
#include "mcs48.h"
#include "i8257.h"

struct Latch8 { u8 value; };

struct Machine {
	Scheduler sch;
	z80_device z80;
	i8257_device dma;
	mcs48_cpu_device snd;
	u8 ram[0xc00], spr[0x400], vid[0x400];
	Latch8 ls175, dev6h, vp2;
	u8 nmi_mask, dma_latch, flip, sprite_bank, palette_bank, grid_on, grid_col;
	attotime vblank_start_time, vblank_end_time;
	u64 frame_number;
	u8 in0, in1, in2, dsw0;           // live input port values (IN2 bit 6 is replaced by the sound CPU status on read)
	s16 t_vblank_begin, t_vblank_end, t_scanline0, t_dkscan, t_sound;
};

extern Machine M;
extern void (*dk_frame_hook)();       // called from vblank_begin's frame_update, like MAME's frame_done / inp playback
extern FILE *dk_slog;                 // when set, scheduler events are logged in the instrumented-MAME SCHEDLOG format

bool dk_load_roms(const char *dir);   // c_5et_g.bin ... s_3j_b.bin
void dk_power_on();                   // machine start + reset, exactly as MAME orders it
void dk_timeslice();                  // one device_scheduler::timeslice()
void dk_run_frames(int n);            // run until n more vblank_begin events have fired
void dk_fixup();                      // re-point internal pointers after copying a Machine
u8 dk_rd(u16 a);                      // side-effect-free RAM peek for the search (6000-77ff only)
