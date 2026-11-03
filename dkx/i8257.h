// i8257.h: Intel 8257 DMA controller, ported from MAME 0.264 src/devices/machine/i8257.cpp (BSD-3-Clause, Curt Coder, Carl).
// Same state machine, same scheduler interactions (trigger / suspend_until_trigger / abort_timeslice / eat_all_cycles).
#pragma once
#include "sched.h"

// board hooks (dk.cpp)
void dma_out_hrq(int state);
u8 dma_memr(u16 a); void dma_memw(u16 a, u8 v);
u8 dma_ior(int ch, u16 a); void dma_iow(int ch, u16 a, u8 v);
void dma_trigger_self(int id);          // device_execute_interface::trigger on the 8257
void dma_suspend_until_trigger(int id); // device_execute_interface::suspend_until_trigger(id, true)
void dma_abort_timeslice();             // device_execute_interface::abort_timeslice on the 8257
void sched_eat_all_cycles();            // device_scheduler::eat_all_cycles

struct i8257_device : Exec {
	enum { STATE_SI, STATE_S0, STATE_S1, STATE_S2, STATE_S3, STATE_SW, STATE_S4 };
	enum { REGISTER_ADDRESS = 0, REGISTER_WORD_COUNT, REGISTER_STATUS = 8, REGISTER_MODE = REGISTER_STATUS };
	enum { MODE_TRANSFER_VERIFY = 0, MODE_TRANSFER_WRITE = 1, MODE_TRANSFER_READ = 2 };
	int m_icount;
	int m_reverse_rw;
	bool m_tc;
	int m_msb, m_hreq, m_hack, m_ready, m_state, m_current_channel, m_last_channel;
	u8 m_transfer_mode, m_status, m_request, m_temp;
	struct { u16 m_address; u16 m_count; u8 m_mode; } m_channel[4];

	static int bit(int x, int n) { return (x >> n) & 1; }
	int MODE_CHAN_ENABLE(int x) const { return bit(m_transfer_mode, x); }
	int MODE_ROTATING_PRIORITY() const { return bit(m_transfer_mode, 4); }
	int MODE_EXTENDED_WRITE() const { return bit(m_transfer_mode, 5); }
	int MODE_TC_STOP() const { return bit(m_transfer_mode, 6); }
	int MODE_AUTOLOAD() const { return bit(m_transfer_mode, 7); }
	int MODE_TRANSFER_MASK() const { return m_channel[m_current_channel].m_mode; }

	void construct() {
		m_icount = 0; m_reverse_rw = 0; m_tc = false; m_msb = 0; m_hreq = CLEAR_LINE; m_hack = 0; m_ready = 1;
		m_state = 0; m_current_channel = 0; m_last_channel = 0; m_transfer_mode = 0; m_status = 0; m_request = 0; m_temp = 0;
		memset(m_channel, 0, sizeof(m_channel));
	}
	void start() { icountptr = &m_icount; }

	void dma_request(int channel, int state) {
		if (state) m_request |= 1 << channel; else m_request &= ~(1 << channel);
		dma_trigger_self(1);
	}
	bool is_request_active(int channel) const { return bit(m_request, channel) && MODE_CHAN_ENABLE(channel); }
	void set_hreq(int state) {
		if (m_hreq != state) { dma_out_hrq(state); m_hreq = state; dma_abort_timeslice(); }
	}
	void set_tc(int state) { if (m_tc != bool(state)) m_tc = state; }
	void set_dack() {}
	void dma_read() {
		u16 offset = m_channel[m_current_channel].m_address;
		switch (MODE_TRANSFER_MASK()) {
		case MODE_TRANSFER_VERIFY: case MODE_TRANSFER_WRITE: m_temp = dma_ior(m_current_channel, offset); break;
		case MODE_TRANSFER_READ: m_temp = dma_memr(offset); break;
		}
	}
	void dma_write() {
		u16 offset = m_channel[m_current_channel].m_address;
		switch (MODE_TRANSFER_MASK()) {
		case MODE_TRANSFER_VERIFY: (void)dma_memr(offset); break;
		case MODE_TRANSFER_WRITE: dma_memw(offset, m_temp); break;
		case MODE_TRANSFER_READ: dma_iow(m_current_channel, offset, m_temp); break;
		}
	}
	void advance() {
		bool tc = m_tc;
		bool al = (MODE_AUTOLOAD() && (m_current_channel == 2));
		set_tc(0);
		if (tc) {
			m_status |= 1 << m_current_channel;
			if (al) { m_channel[2].m_address = m_channel[3].m_address; m_channel[2].m_count = m_channel[3].m_count; m_channel[2].m_mode = m_channel[3].m_mode; }
			else if (MODE_TC_STOP()) m_transfer_mode &= ~(1 << m_current_channel);
		}
		if (!(al && tc)) {
			m_channel[m_current_channel].m_count--;
			m_channel[m_current_channel].m_count &= 0x3fff;
			m_channel[m_current_channel].m_address++;
		}
	}
	void device_reset() {
		m_state = STATE_SI; m_transfer_mode = 0; m_status = 0; m_msb = 0; m_current_channel = -1; m_last_channel = 3; m_hreq = -1; m_tc = 0;
		for (auto &e : m_channel) { e.m_address = 0; e.m_count = 0; e.m_mode = 0; }
		set_hreq(0);
		set_dack();
	}
	bool next_channel() {
		int priority[] = { 0, 1, 2, 3 };
		if (MODE_ROTATING_PRIORITY()) {
			int last_channel = m_last_channel;
			for (int channel = 3; channel >= 0; channel--) { priority[channel] = last_channel; last_channel--; if (last_channel < 0) last_channel = 3; }
		}
		for (auto &e : priority) if (is_request_active(e)) { m_current_channel = m_last_channel = e; return true; }
		return false;
	}
	void execute_run() {
		do {
			switch (m_state) {
			case STATE_SI:
				set_tc(0);
				if (next_channel()) m_state = STATE_S0;
				else { m_icount = 0; dma_suspend_until_trigger(1); }
				break;
			case STATE_S0:
				set_hreq(1);
				if (m_hack) m_state = STATE_S1;
				else { m_icount = 0; dma_suspend_until_trigger(1); }
				break;
			case STATE_S1: set_tc(0); m_state = STATE_S2; break;
			case STATE_S2: set_dack(); m_state = STATE_S3; break;
			case STATE_S3:
				dma_read();
				if (MODE_EXTENDED_WRITE()) dma_write();
				if (m_ready) {
					m_state = STATE_S4;
					if ((m_channel[m_current_channel].m_count == 0) && (MODE_TRANSFER_MASK() != MODE_TRANSFER_READ)) set_tc(1);
				} else m_state = STATE_SW;
				break;
			case STATE_SW:
				if (m_ready) {
					m_state = STATE_S4;
					if ((m_channel[m_current_channel].m_count == 0) && (MODE_TRANSFER_MASK() != MODE_TRANSFER_READ)) set_tc(1);
				}
				break;
			case STATE_S4:
				if (!MODE_EXTENDED_WRITE()) dma_write();
				if ((m_channel[m_current_channel].m_count == 0) && (MODE_TRANSFER_MASK() == MODE_TRANSFER_READ)) set_tc(1);
				advance();
				if (m_hack && next_channel()) m_state = STATE_S1;
				else { set_hreq(0); m_current_channel = -1; m_state = STATE_SI; set_dack(); }
				break;
			}
			m_icount--;
		} while (m_icount > 0);
	}
	u8 read(u32 offset) {      // side effects always enabled (CPU reads)
		u8 data = 0;
		if (!bit(offset, 3)) {
			int channel = (offset >> 1) & 0x03;
			switch (offset & 0x01) {
			case REGISTER_ADDRESS: data = m_msb ? (m_channel[channel].m_address >> 8) : (m_channel[channel].m_address & 0xff); break;
			case REGISTER_WORD_COUNT:
				if (m_msb) {
					data = (m_channel[channel].m_count >> 8);
					if (m_reverse_rw && m_channel[channel].m_mode) data |= (m_channel[channel].m_mode == 1) ? 0x80 : 0x40;
					else data |= (m_channel[channel].m_mode << 6);
				} else data = m_channel[channel].m_count & 0xff;
				break;
			}
			m_msb = !m_msb;
		} else if (offset == REGISTER_STATUS) { data = m_status; m_status &= 0xf0; }
		return data;
	}
	void write(u32 offset, u8 data) {
		if (!bit(offset, 3)) {
			int channel = (offset >> 1) & 0x03;
			switch (offset & 0x01) {
			case REGISTER_ADDRESS:
				if (m_msb) {
					m_channel[channel].m_address = (data << 8) | (m_channel[channel].m_address & 0xff);
					if (MODE_AUTOLOAD() && (channel == 2)) m_channel[3].m_address = (data << 8) | (m_channel[3].m_address & 0xff);
				} else {
					m_channel[channel].m_address = (m_channel[channel].m_address & 0xff00) | data;
					if (MODE_AUTOLOAD() && (channel == 2)) m_channel[3].m_address = (m_channel[3].m_address & 0xff00) | data;
				}
				break;
			case REGISTER_WORD_COUNT:
				if (m_msb) {
					m_channel[channel].m_count = ((data & 0x3f) << 8) | (m_channel[channel].m_count & 0xff);
					m_channel[channel].m_mode = (data >> 6);
					if (m_reverse_rw && m_channel[channel].m_mode) m_channel[channel].m_mode = (m_channel[channel].m_mode == 1) ? 2 : 1;
					if (MODE_AUTOLOAD() && (channel == 2)) {
						m_channel[3].m_count = ((data & 0x3f) << 8) | (m_channel[3].m_count & 0xff);
						m_channel[3].m_mode = m_channel[2].m_mode;
					}
				} else {
					m_channel[channel].m_count = (m_channel[channel].m_count & 0xff00) | data;
					if (MODE_AUTOLOAD() && (channel == 2)) m_channel[3].m_count = (m_channel[3].m_count & 0xff00) | data;
				}
				break;
			}
			m_msb = !m_msb;
		} else if (offset == REGISTER_MODE) m_transfer_mode = data;
		if ((m_transfer_mode & m_request & 0x0f) != 0) { sched_eat_all_cycles(); dma_trigger_self(1); }
	}
	void hlda_w(int state) { m_hack = state; dma_trigger_self(1); }
	void dreq0_w(int state) { dma_request(0, state); }
	void dreq1_w(int state) { dma_request(1, state); }
};
