// snaptest: snapshot = memcpy of the whole Machine. Check that a restored machine continues bit-identically, and time it.
#include "dk.h"
#include <vector>
#include <chrono>
static std::vector<u8> P; static long n = 0;
static void hook() { n++; size_t o = size_t(n) * 3; if (o + 3 <= P.size()) { M.in0 = P[o]; M.in1 = P[o + 1]; M.in2 = P[o + 2] & ~0x40; } }
static void run_to(long f) { while (n < f) dk_timeslice(); }
int main(int, char **argv) {
	dk_load_roms(argv[1]);
	FILE *f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); P.resize(ftell(f)); fseek(f, 0, SEEK_SET); if (!fread(P.data(), 1, P.size(), f)) return 1; fclose(f);
	dk_frame_hook = hook; dk_power_on();
	run_to(3000);                                   // into gameplay
	static Machine snap; long n0 = n; snap = M;     // snapshot
	run_to(n0 + 1500); static Machine a; a = M;     // continue 1500 frames
	M = snap; dk_fixup(); n = n0;                   // restore
	run_to(n0 + 1500);
	printf("sizeof(Machine) = %zu bytes; restored run identical: %s\n", sizeof(Machine), memcmp(&a, &M, sizeof(M)) == 0 ? "YES" : "NO");
	auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < 1000000; i++) { M = snap; dk_fixup(); }
	double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	printf("restore: %.0f ns each\n", dt * 1e3);
	return 0;
}
