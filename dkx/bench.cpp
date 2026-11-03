// bench: the search workload on dkx -- from a gameplay snapshot, play 60 frames of random inputs, restore, repeat.
#include "dk.h"
#include <vector>
#include <chrono>
#include <random>
static std::vector<u8> P; static long n = 0; static bool scripted = true; static std::mt19937 rng(1);
static void hook() {
	n++;
	if (scripted) { size_t o = size_t(n) * 3; if (o + 3 <= P.size()) { M.in0 = P[o]; M.in1 = P[o + 1]; M.in2 = P[o + 2] & ~0x40; } }
	else M.in0 = u8(rng() & 0x1f);
}
int main(int, char **argv) {
	dk_load_roms(argv[1]);
	FILE *f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); P.resize(ftell(f)); fseek(f, 0, SEEK_SET); if (!fread(P.data(), 1, P.size(), f)) return 1; fclose(f);
	dk_frame_hook = hook; dk_power_on();
	while (n < 3000) dk_timeslice();
	static Machine root; root = M; scripted = false;
	auto t0 = std::chrono::steady_clock::now(); long frames = 0;
	for (int r = 0; r < 500; r++) { M = root; dk_fixup(); long n0 = n; while (n < n0 + 60) dk_timeslice(); frames += 60; n = n0; }
	double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	printf("dkx worker: %.0f frames/s\n", frames / dt);
}
