// trace: run dkx from power-on with the per-frame input values from a MAME reference trace (ref/trace.lua format),
// writing the same per-frame records (regs, ports, RAM) and optionally the scheduler log, for differential testing.
#include "dk.h"
#include <vector>
#include <cstdlib>

static std::vector<u8> ref; static size_t RS = 4 + 8 + 18 * 2 + 4 + 3072 + 2048;
static FILE *out; static int nframe = 0, NF = 0;

static void hook() {
	nframe++;
	z80_device &z = M.z80;
	u16 regs[18] = { z.m_pc.w.l, z.m_sp.w.l, z.m_af.w.l, z.m_bc.w.l, z.m_de.w.l, z.m_hl.w.l, z.m_ix.w.l, z.m_iy.w.l,
		z.m_af2.w.l, z.m_bc2.w.l, z.m_de2.w.l, z.m_hl2.w.l, z.m_wz.w.l, u16((z.m_r & 0x7f) | (z.m_r2 & 0x80)), z.m_i, z.m_im, z.m_iff1, z.m_iff2 };
	u32 n = nframe; double t = M.vblank_start_time.as_double();
	fwrite(&n, 4, 1, out); fwrite(&t, 8, 1, out); fwrite(regs, 2, 18, out);
	u8 ports[4] = { M.in0, M.in1, z80_mem_rd(0x7d00), M.dsw0 };   // IN2 read has no side effects we model
	fwrite(ports, 1, 4, out); fwrite(M.ram, 1, 0xc00, out); fwrite(M.spr, 1, 0x400, out); fwrite(M.vid, 1, 0x400, out);
	// MAME runs the Lua frame hook before the input update in the same frame_update, so the reference's record n+1
	// holds the input that this frame update applies
	size_t o = size_t(nframe) * RS + 12 + 36;
	if (o + 4 <= ref.size()) { M.in0 = ref[o]; M.in1 = ref[o + 1]; M.in2 = ref[o + 2] & ~0x40; }
}

int main(int argc, char **argv) {
	if (argc < 5) { fprintf(stderr, "usage: trace ROMDIR REF.bin OUT.bin NFRAMES [SCHEDLOG]\n"); return 1; }
	if (!dk_load_roms(argv[1])) return 1;
	FILE *f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); ref.resize(ftell(f)); fseek(f, 0, SEEK_SET); fread(ref.data(), 1, ref.size(), f); fclose(f);
	out = fopen(argv[3], "wb"); NF = atoi(argv[4]);
	if (argc > 5) dk_slog = fopen(argv[5], "w");
	dk_frame_hook = hook;
	dk_power_on();
	while (nframe < NF) dk_timeslice();
	fclose(out); if (dk_slog) fclose(dk_slog);
	return 0;
}
