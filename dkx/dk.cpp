// dk.cpp: scheduler (emu/schedule.cpp + emu/diexec.cpp semantics) and the dkong2b board (mame/nintendo/dkong*.cpp, emu/screen.cpp).
#include "dk.h"
#include <string>

Machine M;
void (*dk_frame_hook)() = nullptr;
FILE *dk_slog = nullptr;
static u8 mainrom[0x10000], sndrom[0x1800];

enum { DEV_MAIN = 0, DEV_DMA = 1, DEV_SND = 2 };
static const char *const TAGS[NUM_EXEC] = { ":maincpu", ":dma8257", ":soundcpu" };
static inline Exec *ex(int i) { return i == DEV_MAIN ? (Exec *)&M.z80 : i == DEV_DMA ? (Exec *)&M.dma : (Exec *)&M.snd; }

// ---------------------------------------------------------------- scheduler core

static attotime local_time(int i) {
	Exec *e = ex(i);
	if (M.sch.executing == i) return e->localtime + e->clk_cycles_to_attotime(u64(e->cycles_running - *e->icountptr));
	return e->localtime;
}
static attotime now() {
	Scheduler &s = M.sch;
	if (s.callback_timer >= 0) return s.callback_timer_expire_time;
	return s.executing >= 0 ? local_time(s.executing) : s.basetime;
}
static attotime head_expire() { return M.sch.timer_head >= 0 ? M.sch.timers[M.sch.timer_head].expire : attotime::never(); }

static void exec_abort(int i) {       // device_execute_interface::abort_timeslice
	if (M.sch.executing != i) return;
	Exec *e = ex(i);
	int delta = *e->icountptr;
	e->cycles_stolen += delta; e->cycles_running -= delta; *e->icountptr -= delta;
}
static void sched_abort() { if (M.sch.executing >= 0) exec_abort(M.sch.executing); }

static void timer_list_insert(int i) {
	Scheduler &s = M.sch; Timer &t = s.timers[i];
	if (!t.expire.is_never() && t.enabled) {
		int prev = -1;
		for (int cur = s.timer_head; cur >= 0; prev = cur, cur = s.timers[cur].next) {
			if (s.timers[cur].expire > t.expire) {
				t.prev = prev; t.next = cur;
				if (prev >= 0) s.timers[prev].next = i; else s.timer_head = i;
				s.timers[cur].prev = i;
				return;
			}
		}
		if (prev >= 0) s.timers[prev].next = i; else s.timer_head = i;
		t.prev = prev; t.next = -1;
	} else {
		if (s.inactive_head >= 0) s.timers[s.inactive_head].prev = i;
		t.next = s.inactive_head; t.prev = -1; s.inactive_head = i;
	}
}
static void timer_list_remove(int i) {
	Scheduler &s = M.sch; Timer &t = s.timers[i];
	if (t.prev >= 0) s.timers[t.prev].next = t.next;
	else if (i == s.timer_head) s.timer_head = t.next;
	else s.inactive_head = t.next;
	if (t.next >= 0) s.timers[t.next].prev = t.prev;
}
static int timer_alloc(u8 kind, u8 arg, bool temporary) {
	for (int i = 0; i < MAX_TIMERS; i++) if (!M.sch.timers[i].inuse) {
		Timer &t = M.sch.timers[i];
		t = Timer{}; t.inuse = 1; t.kind = kind; t.arg = arg; t.temporary = temporary; t.next = t.prev = -1;
		return i;
	}
	fprintf(stderr, "dkx: out of timers\n"); abort();
}
// emu_timer::init for a temporary timer (timer_set / synchronize)
static void timer_temp(u8 kind, u8 arg, s32 param, attotime delay) {
	int i = timer_alloc(kind, arg, true);
	Timer &t = M.sch.timers[i];
	t.param = param; t.period = attotime::never();
	t.start = now(); t.expire = t.start + delay; t.enabled = !t.expire.is_never();
	timer_list_insert(i);
	if (i == M.sch.timer_head) sched_abort();
}
// permanent timer allocation: emu_timer::init with never, then inactive
static int timer_perm(u8 kind) {
	int i = timer_alloc(kind, 0, false);
	Timer &t = M.sch.timers[i];
	t.period = attotime::never(); t.start = now(); t.expire = attotime::never(); t.enabled = 0;
	timer_list_insert(i);
	return i;
}
static void timer_adjust(int i, attotime delay, s32 param = 0, attotime period = attotime::never()) {
	Scheduler &s = M.sch; Timer &t = s.timers[i];
	if (s.callback_timer == i) s.callback_timer_modified = 1;
	t.param = param; t.enabled = 1;
	if (delay.s < 0) delay = attotime::zero();
	t.start = now(); t.expire = t.start + delay; t.period = period;
	timer_list_remove(i); timer_list_insert(i);
	if (i == s.timer_head) sched_abort();
}

static void rebuild_execute_list();
static void add_quantum(attoseconds_t quantum, attotime duration) {
	Scheduler &s = M.sch;
	attotime curtime = now(), expire = curtime + duration;
	// drop expired, find the last slot whose requested quantum <= ours
	int n = 0; QuantumSlot keep[8] = {};
	for (int k = 0; k < s.nquantum; k++) if (!(curtime >= s.quantum[k].expire)) keep[n++] = s.quantum[k];
	memcpy(s.quantum, keep, sizeof(keep)); s.nquantum = n;
	int ia = -1;
	for (int k = 0; k < s.nquantum; k++) if (s.quantum[k].requested <= quantum) ia = k;
	if (ia >= 0 && s.quantum[ia].requested == quantum) s.quantum[ia].expire = atto_max(s.quantum[ia].expire, expire);
	else {
		for (int k = s.nquantum; k > ia + 1; k--) s.quantum[k] = s.quantum[k - 1];
		QuantumSlot &q = s.quantum[ia + 1];
		q.requested = quantum; q.actual = quantum > s.quantum_minimum ? quantum : s.quantum_minimum; q.expire = expire;
		s.nquantum++;
	}
}

static void suspend(int i, u32 reason, bool eat) {
	Exec *e = ex(i); e->nextsuspend |= reason; e->nexteatcycles = eat;
	M.sch.suspend_changes_pending = 1; exec_abort(i);
}
static void resume(int i, u32 reason) {
	Exec *e = ex(i); e->nextsuspend &= ~reason;
	M.sch.suspend_changes_pending = 1; exec_abort(i);
}
static void exec_trigger(int i, int id) {
	exec_abort(i);
	Exec *e = ex(i);
	if ((e->nextsuspend & SUSPEND_REASON_TRIGGER) != 0 && e->trigger_id == id) { resume(i, SUSPEND_REASON_TRIGGER); e->trigger_id = 0; }
}
static void trigger_all(int id) { for (int k = 0; k < M.sch.nexec; k++) exec_trigger(M.sch.exec_order[k], id); }
void dma_trigger_self(int id) { exec_trigger(DEV_DMA, id); }
void dma_suspend_until_trigger(int id) { suspend(DEV_DMA, SUSPEND_REASON_TRIGGER, true); M.dma.trigger_id = id; }
void dma_abort_timeslice() { exec_abort(DEV_DMA); }
void sched_eat_all_cycles() {
	for (int k = 0; k < M.sch.nexec; k++) {
		int i = M.sch.exec_order[k];
		if (M.sch.executing == i) { Exec *e = ex(i); *e->icountptr = (1000000000 > *e->icountptr) ? 0 : (*e->icountptr - 1000000000); }
	}
}

// device_input::set_state_synced
static void set_input_line(int dev, int line, int state) {
	device_input &in = ex(dev)->input[line];
	int idx = in.qindex++;
	if (idx >= 32) { fprintf(stderr, "dkx: input queue overflow\n"); abort(); }
	in.queue[idx] = state & 0xff;
	if (idx == 0) timer_temp(TK_INPUT_QUEUE, u8(dev * NUM_LINES + line), 0, attotime::zero());
}
static void dev_execute_set_input(int dev, int line, int state) {
	if (dev == DEV_MAIN) M.z80.execute_set_input(line, state);
	else if (dev == DEV_SND) M.snd.execute_set_input(line, state);
}
static void empty_event_queue(int dev, int line) {
	device_input &in = ex(dev)->input[line];
	for (int k = 0; k < in.qindex; k++) {
		in.curstate = in.queue[k] & 0xff;
		if (line == LINE_HALT) {
			if (in.curstate == ASSERT_LINE) suspend(dev, SUSPEND_REASON_HALT, true);
			else if (in.curstate == CLEAR_LINE) resume(dev, SUSPEND_REASON_HALT);
		} else {
			dev_execute_set_input(dev, line, in.curstate == CLEAR_LINE ? CLEAR_LINE : ASSERT_LINE);
			if (in.curstate != CLEAR_LINE) trigger_all(-2000 + dev);   // signal_interrupt_trigger (nobody waits on it)
		}
	}
	in.qindex = 0;
}

static void rebuild_execute_list() {
	Scheduler &s = M.sch;
	if (s.nquantum == 0) add_quantum(attotime::from_hz(60).a, attotime::never());
	int n = 0;
	for (int i = 0; i < NUM_EXEC; i++) if (ex(i)->suspend == 0) s.exec_order[n++] = s8(i);
	for (int i = 0; i < NUM_EXEC; i++) if (ex(i)->suspend != 0) s.exec_order[n++] = s8(i);
	s.nexec = n;
}
static void apply_suspend_changes() {
	u32 changed = 0;
	for (int k = 0; k < M.sch.nexec; k++) {
		Exec *e = ex(M.sch.exec_order[k]);
		changed |= e->suspend ^ e->nextsuspend;
		e->suspend = e->nextsuspend;
		e->nextsuspend &= ~SUSPEND_REASON_TIMESLICE;
		e->eatcycles = e->nexteatcycles;
	}
	if (changed) rebuild_execute_list(); else M.sch.suspend_changes_pending = 0;
}

static void run_dev(int i) {
	if (i == DEV_MAIN) M.z80.execute_run(); else if (i == DEV_DMA) M.dma.execute_run(); else M.snd.execute_run();
}

static void fire(Timer &t);
static void execute_timers() {
	Scheduler &s = M.sch;
	while (s.timer_head >= 0 && s.timers[s.timer_head].expire <= s.basetime) {
		int i = s.timer_head; Timer &t = s.timers[i];
		bool was_enabled = t.enabled;
		if (t.period.is_zero() || t.period.is_never()) t.enabled = 0;
		s.callback_timer_modified = 0; s.callback_timer = s16(i); s.callback_timer_expire_time = t.expire;
		if (was_enabled) fire(t);
		if (!s.callback_timer_modified) {
			if (!t.temporary) { t.start = t.expire; t.expire += t.period; timer_list_remove(i); timer_list_insert(i); }
			else { timer_list_remove(i); t.inuse = 0; }
		}
	}
	s.callback_timer = -1;
}

void dk_timeslice() {
	Scheduler &s = M.sch;
	while (s.basetime >= s.quantum[0].expire) { memmove(&s.quantum[0], &s.quantum[1], sizeof(QuantumSlot) * (s.nquantum - 1)); s.nquantum--; }
	while (s.basetime < head_expire()) {
		attotime target = s.basetime + attotime(0, s.quantum[0].actual);
		if (head_expire() < target) target = head_expire();
		if (dk_slog) fprintf(dk_slog, "S %d %lld %d %lld\n", s.basetime.s, (long long)s.basetime.a, target.s, (long long)target.a);
		if (s.suspend_changes_pending) apply_suspend_changes();
		for (int k = 0; k < s.nexec; k++) {
			int i = s.exec_order[k]; Exec *e = ex(i);
			if ((e->suspend == 0 || e->eatcycles) && target.s >= e->localtime.s) {
				attoseconds_t delta = target.a - e->localtime.a;
				if (delta < 0 && target.s > e->localtime.s) delta += ATTOSECONDS_PER_SECOND;
				if (delta >= e->attoseconds_per_cycle) {
					// divu_64x32(delta >> divshift, divisor) with this machine's fixed clocks, written with constants
					int ran = e->cycles_running = (i == DEV_SND) ? int(u32((u64(delta) >> 11) / 1220703125u)) : int(u32((u64(delta) >> 8) / 1271565755u));
					if (e->suspend == 0) {
						e->cycles_stolen = 0; s.executing = s8(i);
						*e->icountptr = e->cycles_running;
						run_dev(i);
						ran -= *e->icountptr; ran -= e->cycles_stolen;
					}
					e->totalcycles += ran;
					attotime dt;
					if (u32(ran) < e->cycles_per_second) dt = attotime(0, e->attoseconds_per_cycle * ran);
					else { u32 rem; s32 secs = divu_64x32_rem(u64(ran), e->cycles_per_second, rem); dt = attotime(secs, u64(rem) * e->attoseconds_per_cycle); }
					e->localtime += dt;
					if (dk_slog) fprintf(dk_slog, "R %s %u %d %d %llu %d %lld\n", TAGS[i], e->suspend, e->cycles_running, ran,
						(unsigned long long)e->totalcycles, e->localtime.s, (long long)e->localtime.a);
					if (e->localtime < target) target = atto_max(e->localtime, s.basetime);
				}
			}
		}
		s.executing = -1;
		s.basetime = target;
	}
	execute_timers();
}

// ---------------------------------------------------------------- screen (emu/screen.cpp, set_raw(6.144 MHz, 384, 0, 256, 264, 16, 240))

static const int HTOTAL = 384, VTOTAL = 264, VIS_TOP = 16, VIS_BOTTOM = 239;
static const attoseconds_t FRAME_PERIOD = HZ_TO_ATTOSECONDS(6144000) * HTOTAL * VTOTAL;
static const attoseconds_t SCANTIME = FRAME_PERIOD / VTOTAL;
static const attoseconds_t PIXELTIME = FRAME_PERIOD / (VTOTAL * HTOTAL);
static const attoseconds_t VBLANK_PERIOD = SCANTIME * (VTOTAL - (VIS_BOTTOM - VIS_TOP + 1));

static attotime time_until_pos(int vpos, int hpos = 0) {
	vpos += VTOTAL - (VIS_BOTTOM + 1); vpos %= VTOTAL;
	attoseconds_t targetdelta = attoseconds_t(vpos) * SCANTIME + attoseconds_t(hpos) * PIXELTIME;
	attoseconds_t curdelta = (now() - M.vblank_start_time).as_attoseconds();
	if (targetdelta <= curdelta + PIXELTIME / 2) targetdelta += FRAME_PERIOD;
	while (targetdelta <= curdelta) targetdelta += FRAME_PERIOD;
	return attotime(0, targetdelta - curdelta);
}
[[maybe_unused]] static int screen_vpos() {
	attoseconds_t delta = (now() - M.vblank_start_time).as_attoseconds();
	delta += PIXELTIME / 2;
	int vpos = int(delta / SCANTIME);
	return (VIS_BOTTOM + 1 + vpos) % VTOTAL;
}
static int screen_vblank() { return (now() < M.vblank_end_time) ? 1 : 0; }
static attotime time_until_vblank_end() {
	attotime target = M.vblank_end_time;
	if (!screen_vblank()) target += attotime(0, FRAME_PERIOD);
	return target - now();
}

static void vblank_irq(int state) { if (state && M.nmi_mask) set_input_line(DEV_MAIN, LINE_NMI, ASSERT_LINE); }

static void vblank_begin() {
	M.vblank_start_time = now();
	M.vblank_end_time = M.vblank_start_time + attotime(0, VBLANK_PERIOD);
	if (dk_frame_hook) dk_frame_hook();       // video_manager::frame_update: input update, Lua frame_done
	vblank_irq(1);
	timer_adjust(M.t_vblank_begin, time_until_pos(VIS_BOTTOM + 1));
	timer_adjust(M.t_vblank_end, time_until_vblank_end());
}
static void vblank_end() { vblank_irq(0); M.frame_number++; }

// ---------------------------------------------------------------- latches (devices/machine/latch8.cpp)

enum { L_LS175 = 0, L_6H = 1, L_VP2 = 2 };
static Latch8 &latch(int i) { return i == L_LS175 ? M.ls175 : i == L_6H ? M.dev6h : M.vp2; }
static void latch_write(int i, u8 data) { timer_temp(TK_LATCH, u8(i), (0xff << 8) | data, attotime::zero()); }
static void latch_bit0_w(int i, int offset, u8 data) {
	u8 mask = u8(1 << offset), masked = u8((data & 1) << offset);
	timer_temp(TK_LATCH, u8(i), (mask << 8) | masked, attotime::zero());
}
static u8 ls175_read() { return u8((M.ls175.value & ~0xf0) ^ 0x0f); }
static u8 vp2_read() {
	u8 res = M.vp2.value;
	res = u8((res & ~(1 << 5)) | (((M.dev6h.value >> 3) & 1) << 5));      // read_cb<5> = 6H bit3_r
	return u8(res ^ 0x20);
}

// ---------------------------------------------------------------- timer dispatch

static void fire(Timer &t) {
	if (dk_slog) {
		static const char *const N[] = { "?", "screen_device::vblank_begin", "screen_device::vblank_end", "screen_device::first_scanline_tick",
			"dkong_state::scanline_callback", "sound_manager::update", "watchdog", "device_execute_interface::device_input::empty_event_queue",
			"latch8_device::timerproc", "device_scheduler::timed_trigger" };
		fprintf(dk_slog, "T %s %d %d %lld\n", N[t.kind], t.param, t.expire.s, (long long)t.expire.a);
	}
	switch (t.kind) {
	case TK_VBLANK_BEGIN: vblank_begin(); break;
	case TK_VBLANK_END: vblank_end(); break;
	case TK_SCANLINE0: timer_adjust(M.t_scanline0, time_until_pos(0)); break;         // reset_partial_updates
	case TK_DK_SCANLINE: {
		int scanline = (t.param + 1) % VTOTAL;
		timer_adjust(M.t_dkscan, time_until_pos(scanline), scanline);
		break;
	}
	case TK_SOUND_UPDATE: break;
	case TK_INPUT_QUEUE: empty_event_queue(t.arg / NUM_LINES, t.arg % NUM_LINES); break;
	case TK_LATCH: {
		Latch8 &l = latch(t.arg); u8 nv = t.param & 0xff, mask = (t.param >> 8) & 0xff;
		l.value = u8((l.value & ~mask) | (nv & mask));
		break;
	}
	case TK_TRIGGER: trigger_all(t.param); break;
	}
}

// ---------------------------------------------------------------- main CPU memory map (dkong_map)

static u8 in2_read() {
	u8 r = u8((M.in2 & ~0x40) | ((((~M.vp2.value) >> 4) & 1) << 6));     // bit 6 = virtual_p2 bit4_q_r (sound CPU status)
	return r;
}
static void p8257_drq_w(u8 data) {
	M.dma.dreq0_w(data & 1);
	M.dma.dreq1_w(data & 1);
	sched_abort();
	add_quantum(0, attotime::from_usec(100));
}
static u8 *rd_page[256];             // 256-byte pages of plain memory (nullptr = handler)
static u8 *wr_page[256];
static void build_pages() {
	for (int p = 0; p < 256; p++) { rd_page[p] = wr_page[p] = nullptr; }
	for (int p = 0x00; p < 0x50; p++) rd_page[p] = mainrom + p * 256;
	for (int p = 0x60; p < 0x6c; p++) rd_page[p] = wr_page[p] = M.ram + (p - 0x60) * 256;
	for (int p = 0x70; p < 0x74; p++) rd_page[p] = wr_page[p] = M.spr + (p - 0x70) * 256;
	for (int p = 0x74; p < 0x78; p++) rd_page[p] = wr_page[p] = M.vid + (p - 0x74) * 256;
}
static u8 mem_rd_slow(u16 a);
static void mem_wr_slow(u16 a, u8 v);
u8 z80_mem_rd(u16 a) { u8 *p = rd_page[a >> 8]; return p ? p[a & 0xff] : mem_rd_slow(a); }
void z80_mem_wr(u16 a, u8 v) { u8 *p = wr_page[a >> 8]; if (p) p[a & 0xff] = v; else mem_wr_slow(a, v); }
static u8 mem_rd_slow(u16 a) {
	if (a < 0x5000) return mainrom[a];
	if (a >= 0x6000 && a < 0x6c00) return M.ram[a - 0x6000];
	if (a >= 0x7000 && a < 0x7400) return M.spr[a - 0x7000];
	if (a >= 0x7400 && a < 0x7800) return M.vid[a - 0x7400];
	if (a >= 0x7800 && a < 0x7810) return M.dma.read(a - 0x7800);
	if (a == 0x7c00) return M.in0;
	if (a == 0x7c80) return M.in1;
	if (a == 0x7d00) return in2_read();
	if (a == 0x7d80) return M.dsw0;
	return 0;
}
static void mem_wr_slow(u16 a, u8 v) {
	if (a >= 0x6000 && a < 0x6c00) { M.ram[a - 0x6000] = v; return; }
	if (a >= 0x7000 && a < 0x7400) { M.spr[a - 0x7000] = v; return; }
	if (a >= 0x7400 && a < 0x7800) { M.vid[a - 0x7400] = v; return; }
	if (a >= 0x7800 && a < 0x7810) { M.dma.write(a - 0x7800, v); return; }
	switch (a) {
	case 0x7c00: latch_write(L_LS175, v); return;
	case 0x7c80: M.grid_col = (v & 7) ^ 7; return;
	case 0x7d80: set_input_line(DEV_SND, LINE_IRQ0, v ? ASSERT_LINE : CLEAR_LINE); return;
	case 0x7d81: M.grid_on = v & 1; return;
	case 0x7d82: M.flip = v & 1; return;
	case 0x7d83: M.sprite_bank = v & 1; return;
	case 0x7d84: M.nmi_mask = v & 1; if (!M.nmi_mask) set_input_line(DEV_MAIN, LINE_NMI, CLEAR_LINE); return;
	case 0x7d85: p8257_drq_w(v); return;
	case 0x7d86: case 0x7d87: { int b = a - 0x7d86; M.palette_bank = u8((M.palette_bank & ~(1 << b)) | ((v & 1) << b)); return; }
	}
	if (a >= 0x7d00 && a < 0x7d08) { latch_bit0_w(L_6H, a - 0x7d00, v); return; }
}
u8 z80_io_rd(u16) { return 0; }
void z80_io_wr(u16, u8) {}

// 8257 hooks
void dma_out_hrq(int state) {                     // dkong_state::busreq_w
	set_input_line(DEV_MAIN, LINE_BUSRQ, state);
	set_input_line(DEV_MAIN, LINE_HALT, state);
	M.dma.hlda_w(state);
}
u8 dma_memr(u16 a) { return z80_mem_rd(a); }
void dma_memw(u16 a, u8 v) { z80_mem_wr(a, v); }
u8 dma_ior(int ch, u16) { return ch == 1 ? M.dma_latch : 0; }
void dma_iow(int ch, u16, u8 v) { if (ch == 0) M.dma_latch = v; }

// ---------------------------------------------------------------- sound CPU (dkong2b_audio)

static u8 tune_r(u8 offset) {
	u8 page = vp2_read() & 0x47;
	if (page & 0x40) return u8((ls175_read() & 0x0f) | (0 << 4));
	return sndrom[0x1000 + (page & 7) * 256 + offset];
}
u8 snd_rom_rd(u16 a) { return sndrom[a & 0xfff]; }
u8 snd_ext_rd(u8 a) { return tune_r(a); }
void snd_ext_wr(u8, u8) {}
u8 snd_port_rd(int p) { return p == 2 ? vp2_read() : 0xff; }
void snd_port_wr(int p, u8 v) { if (p == 2) latch_write(L_VP2, v); }
int snd_test_rd(int t) { return t == 0 ? (((~M.dev6h.value) >> 5) & 1) : (((~M.dev6h.value) >> 4) & 1); }
u8 snd_bus_rd() { return tune_r(0); }
void snd_bus_wr(u8) {}

// ---------------------------------------------------------------- start / reset

static bool load(const char *dir, const char *name, u8 *dst, int len) {
	std::string p = std::string(dir) + "/" + name;
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) { fprintf(stderr, "dkx: missing %s\n", p.c_str()); return false; }
	size_t n = fread(dst, 1, len, f); fclose(f);
	return int(n) == len;
}
bool dk_load_roms(const char *dir) {
	memset(mainrom, 0, sizeof(mainrom));
	return load(dir, "c_5et_g.bin", mainrom + 0x0000, 0x1000) && load(dir, "c_5ct_g.bin", mainrom + 0x1000, 0x1000) &&
		load(dir, "c_5bt_g.bin", mainrom + 0x2000, 0x1000) && load(dir, "c_5at_g.bin", mainrom + 0x3000, 0x1000) &&
		load(dir, "s_3i_b.bin", sndrom + 0x0000, 0x0800) && load(dir, "s_3i_b.bin", sndrom + 0x0800, 0x0800) &&
		load(dir, "s_3j_b.bin", sndrom + 0x1000, 0x0800);
}

void dk_fixup() {
	M.z80.icountptr = &M.z80.m_icount; M.dma.icountptr = &M.dma.m_icount; M.snd.icountptr = &M.snd.m_icount;
	M.snd.m_regptr = &M.snd.m_ram[(M.snd.m_psw & 0x10) ? 24 : 0];
}

void dk_power_on() {
	memset((void *)&M, 0, sizeof(M));
	build_pages();
	Scheduler &s = M.sch;
	s.timer_head = s.inactive_head = -1; s.executing = -1; s.callback_timer = -1;
	M.dsw0 = 0x80;

	// device construction + start (clock set -> compute_perfect_interleave)
	M.z80.set_clock(3072000, 1); M.dma.set_clock(3072000, 1); M.snd.set_clock(6000000, 15);
	for (int i = 0; i < NUM_EXEC; i++) { ex(i)->suspend = SUSPEND_REASON_RESET; ex(i)->nextsuspend = 0; }
	rebuild_execute_list();
	{   // compute_perfect_interleave over the execute list: second-smallest minimum quantum
		attoseconds_t mq[3] = { M.z80.attoseconds_per_cycle * 2, M.dma.attoseconds_per_cycle * 1, M.snd.attoseconds_per_cycle * 1 };
		attoseconds_t smallest = mq[s.exec_order[0]], perfect = ATTOSECONDS_PER_SECOND - 1;
		for (int k = 1; k < s.nexec; k++) {
			attoseconds_t c = mq[s.exec_order[k]];
			if (c < smallest) { perfect = smallest; smallest = c; } else if (c < perfect) perfect = c;
		}
		s.quantum_minimum = perfect;
		for (int k = 0; k < s.nquantum; k++) s.quantum[k].actual = s.quantum[k].requested > perfect ? s.quantum[k].requested : perfect;
	}
	if (M.z80.divshift != 8 || M.z80.divisor != 1271565755u || M.dma.divshift != 8 || M.dma.divisor != 1271565755u || M.snd.divshift != 11 || M.snd.divisor != 1220703125u) { fprintf(stderr, "dkx: unexpected clock divisors\n"); abort(); }
	M.z80.start(); M.dma.construct(); M.dma.start(); M.dma.m_reverse_rw = 1;
	// mcs48 device_start zero-fill (MB8884: no internal ROM, I8048 features)
	M.snd.m_feature_mask = 0x03; M.snd.m_int_rom_size = 0; M.snd.m_ea = 1; M.snd.icountptr = &M.snd.m_icount;
	M.snd.m_regptr = &M.snd.m_ram[0];

	// timer allocation order mirrors MAME start: sound manager, screen, then dkong video_start
	M.t_sound = timer_perm(TK_SOUND_UPDATE);
	timer_adjust(M.t_sound, attotime::from_hz(50), 0, attotime::from_hz(50));
	M.t_vblank_begin = timer_perm(TK_VBLANK_BEGIN);
	M.t_vblank_end = timer_perm(TK_VBLANK_END);
	M.t_scanline0 = timer_perm(TK_SCANLINE0);
	M.vblank_start_time = attotime::zero(); M.vblank_end_time = attotime(0, VBLANK_PERIOD);
	timer_adjust(M.t_vblank_begin, time_until_pos(VIS_BOTTOM + 1));     // configure(): not yet a full frame since vblank start
	timer_adjust(M.t_scanline0, time_until_pos(0));                     // configure(): vpos() != 0
	M.t_dkscan = timer_perm(TK_DK_SCANLINE);
	timer_adjust(M.t_dkscan, time_until_pos(0));

	// machine reset, device order: maincpu, dma8257, (screen, latches), soundcpu
	for (int i = 0; i < NUM_EXEC; i++) { ex(i)->totalcycles = 0; resume(i, SUSPEND_ANY_REASON); }
	M.z80.reset();
	M.dma.device_reset();
	M.ls175.value = M.dev6h.value = M.vp2.value = 0;
	M.snd.device_reset();
}

void dk_run_frames(int n) {
	u64 target = M.frame_number + n;
	while (M.frame_number < target) dk_timeslice();
}

u8 dk_rd(u16 a) {
	if (a >= 0x6000 && a < 0x6c00) return M.ram[a - 0x6000];
	if (a >= 0x7000 && a < 0x7400) return M.spr[a - 0x7000];
	if (a >= 0x7400 && a < 0x7800) return M.vid[a - 0x7400];
	return 0;
}
