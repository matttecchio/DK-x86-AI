// sched.h: MAME 0.264 device_scheduler / device_execute_interface / emu_timer semantics (emu/schedule.cpp, emu/diexec.cpp),
// reduced to what the Donkey Kong board uses. Everything is plain data so a whole machine can be copied with memcpy.
#pragma once
#include "core.h"

constexpr u32 SUSPEND_REASON_HALT = 0x0001, SUSPEND_REASON_RESET = 0x0002, SUSPEND_REASON_TRIGGER = 0x0008,
              SUSPEND_REASON_TIMESLICE = 0x0020, SUSPEND_ANY_REASON = ~0u;

// input lines we use (numbering only matters for identity)
enum { LINE_IRQ0 = 0, LINE_BUSRQ = 1, LINE_NMI = 2, LINE_HALT = 3, NUM_LINES = 4 };

struct device_input {          // diexec.cpp device_input (queue of synced line changes)
	s32 queue[32];
	int qindex;
	int curstate;
};

struct Exec {                  // device_execute_interface
	u32 suspend, nextsuspend;
	u8 eatcycles, nexteatcycles;
	s32 trigger_id;
	u64 totalcycles;
	attotime localtime;
	u32 cycles_per_second;
	attoseconds_t attoseconds_per_cycle;
	int divshift;
	u32 divisor;
	int cycles_running, cycles_stolen;
	int *icountptr;
	device_input input[NUM_LINES];
	u32 clock, clkdiv;                 // device clock and execute_clocks_to_cycles divider
	attoseconds_t attoseconds_per_clock;
	// diexec.h cycles_to_attotime(): goes through the device clock (device_t::clocks_to_attotime), unlike the timeslice math
	attotime clk_cycles_to_attotime(u64 cycles) const {
		u64 numclocks = cycles * clkdiv;
		if (numclocks < clock) return attotime(0, attoseconds_t(numclocks) * attoseconds_per_clock);
		u32 rem; u32 q = divu_64x32_rem(numclocks, clock, rem);
		return attotime(q, u64(rem) * u64(attoseconds_per_clock));
	}
	void set_clock(u32 clk, u32 div) {
		clock = clk; clkdiv = div; attoseconds_per_clock = HZ_TO_ATTOSECONDS(clk);
		u32 cycles_per_sec = u32((u64(clk) + div - 1) / div);
		cycles_per_second = cycles_per_sec;
		attoseconds_per_cycle = HZ_TO_ATTOSECONDS(cycles_per_sec);
		s64 attos = attoseconds_per_cycle; divshift = 0;
		while (attos >= (1L << 31)) { divshift++; attos >>= 1; }
		divisor = u32(attos);
	}
	attotime cycles_to_attotime(u64 cycles) const {
		if (cycles < cycles_per_second) return attotime(0, attoseconds_t(cycles) * attoseconds_per_cycle);
		u32 rem; u32 secs = divu_64x32_rem(cycles, cycles_per_second, rem);
		return attotime(secs, u64(rem) * attoseconds_per_cycle);
	}
};

// timer kinds (the board dispatches on these)
enum TimerKind : u8 {
	TK_NONE, TK_VBLANK_BEGIN, TK_VBLANK_END, TK_SCANLINE0, TK_DK_SCANLINE, TK_SOUND_UPDATE, TK_WATCHDOG,
	TK_INPUT_QUEUE,            // arg = device*NUM_LINES + line  (empty_event_queue)
	TK_LATCH,                  // arg = latch index, param = (mask << 8) | data   (latch8 timerproc)
	TK_TRIGGER,                // timed trigger, param = trigger id
};

struct Timer {
	attotime start, expire, period;
	s32 param;
	u8 kind, arg, enabled, temporary, inuse;
	s16 next, prev;
};

constexpr int MAX_TIMERS = 32, NUM_EXEC = 3;   // DK never has more than ~10 live

struct QuantumSlot { attoseconds_t requested, actual; attotime expire; };

struct Scheduler {
	attotime basetime;
	s16 timer_head, inactive_head;
	Timer timers[MAX_TIMERS];
	QuantumSlot quantum[8]; int nquantum;
	attoseconds_t quantum_minimum;
	s8 exec_order[NUM_EXEC]; int nexec;
	s8 executing;              // index into exec[], -1 if none
	s16 callback_timer;        // -1 if none
	u8 callback_timer_modified;
	attotime callback_timer_expire_time;
	u8 suspend_changes_pending;
};
