// play: run a whole game from power-on with per-frame inputs (ref/portlog.lua dump) and write the playlog.lua trace.
#include "dk.h"
#include <vector>
#include <chrono>

static std::vector<u8> P; static FILE *L; static long n = 0, last = 0, ps = -1; static int pm = -1;
static int r(u16 a) { return dk_rd(a); }
static int bcd(int v) { return (v >> 4) * 10 + (v & 15); }
static long score() {
	long v = bcd(r(0x60B4)) * 10000L + bcd(r(0x60B3)) * 100 + bcd(r(0x60B2));
	while (v < last - 400000) v += 1000000;
	last = v; return v;
}
static void hook() {
	n++;
	int m = r(0x600A);
	if (m == 0x0C && pm != 0x0C) fprintf(L, "start %ld %d %d %d %d frame=%ld\n", score(), r(0x6228), r(0x6229), r(0x6227), r(0x62B1), n);
	pm = m;
	long s = score(); if (s != ps) { fprintf(L, "score %ld frame=%ld\n", s, n); ps = s; }
	size_t o = size_t(n) * 3;            // record n+1 is the input this frame update applies
	if (o + 3 <= P.size()) { M.in0 = P[o]; M.in1 = P[o + 1]; M.in2 = P[o + 2] & ~0x40; }
}
int main(int argc, char **argv) {
	if (argc < 4) { fprintf(stderr, "usage: play ROMDIR PORTS.bin LOG\n"); return 1; }
	if (!dk_load_roms(argv[1])) return 1;
	FILE *f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); P.resize(ftell(f)); fseek(f, 0, SEEK_SET); fread(P.data(), 1, P.size(), f); fclose(f);
	L = fopen(argv[3], "w");
	dk_frame_hook = hook;
	auto t0 = std::chrono::steady_clock::now();
	dk_power_on();
	long nf = long(P.size() / 3);
	while (n < nf) dk_timeslice();
	double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	fprintf(L, "stop %ld frame=%ld\n", score(), n); fclose(L);
	fprintf(stderr, "%ld frames in %.2f s = %.0f frames/s (%.0fx real time)\n", n, dt, n / dt, n / dt / 60.606);
	return 0;
}
