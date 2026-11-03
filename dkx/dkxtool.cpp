// dkx: command-line front end. Each mode reproduces one of the MAME Lua scripts in ~/dkong-ai/fullgame exactly,
// on the MAME-exact emulator core, with MAME's frame-hook semantics:
//   - the hook runs at vblank_begin (video_manager::frame_update), before the input ports are latched for the frame;
//   - save/load requested from the hook happen after the current timeslice (running_machine::handle_saveload);
//   - input port values are not part of a state, so the value set before a load carries into the loaded machine.
//
//   dkx mkfresh   META=..  DKXST=dir                         (mkfresh.lua)
//   dkx advance   S0 START NEXT PATHF META [SACR] DKXST=dir   (advance.lua)
//   dkx seedgen   BT T0 L0 S0 INP OUT PFX [STEP] STD=dir      (seedgen.lua, loads STD/root.sta)
//   dkx worker    WID JOBDIR STD RS L0 T0 BT [S0] [SACR]      (gworker.lua job protocol; NS must be 0)
#include "dk.h"
#include <string>
#include <vector>
#include <cstdlib>
#include <cmath>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

static std::string env(const char *k, const char *d = "") { const char *v = getenv(k); return v ? v : d; }
static long envl(const char *k, long d) { const char *v = getenv(k); return v ? atol(v) : d; }

// ---------------------------------------------------------------- runtime: MAME frame-hook semantics

static int pend = 0; static std::string pend_file;      // 1 = load, 2 = save
static u8 cur_in0 = 0, cur_in2 = 0; static bool quit = false;
static void (*script)() = nullptr;
// route masks are 1 = left, 2 = right, 4 = up, 8 = down, 16 = jump (the Lua field order); DK's IN0 has right at bit 0, left at bit 1
static void apply(int m) { cur_in0 = u8(((m & 1) << 1) | ((m & 2) >> 1) | (m & 0x1c)); }
static void hook() { script(); M.in0 = cur_in0; M.in2 = cur_in2; }   // ioport frame update follows the Lua hook

static const u32 MAGIC = 0x32584b44;  // "DKX2": header {magic, raw size, packed size} + zlib(level 1) Machine image
static u8 packbuf[sizeof(Machine) + 1024];
static bool save_state(const std::string &path) {
	static Machine img; img = M;          // pointers into the process are re-made on load; zero them so files are deterministic
	img.z80.icountptr = img.dma.icountptr = img.snd.icountptr = nullptr; img.snd.m_regptr = nullptr;
	uLongf plen = sizeof(packbuf) - 12;
	if (compress2(packbuf + 12, &plen, (const Bytef *)&img, sizeof(img), 1) != Z_OK) return false;
	u32 hdr[3] = { MAGIC, u32(sizeof(Machine)), u32(plen) }; memcpy(packbuf, hdr, 12);
	std::string tmp = path + ".tmp";
	int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return false;
	bool ok = write(fd, packbuf, 12 + plen) == ssize_t(12 + plen);
	close(fd);
	return ok && rename(tmp.c_str(), path.c_str()) == 0;
}
static Machine loadbuf;
static bool load_state(const std::string &path) {
	int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0) return false;
	ssize_t n = read(fd, packbuf, sizeof(packbuf)); close(fd);
	u32 hdr[3]; if (n < 12) return false; memcpy(hdr, packbuf, 12);
	if (hdr[0] != MAGIC || hdr[1] != sizeof(Machine) || ssize_t(12 + hdr[2]) != n) return false;
	uLongf rlen = sizeof(Machine);
	if (uncompress((Bytef *)&loadbuf, &rlen, packbuf + 12, hdr[2]) != Z_OK || rlen != sizeof(Machine)) return false;
	memcpy(&M, &loadbuf, sizeof(M)); dk_fixup();
	M.in0 = cur_in0; M.in2 = cur_in2;
	return true;
}
static void run_until_quit() {
	while (!quit) {
		dk_timeslice();
		if (pend) {
			int p = pend; pend = 0;
			if (p == 2) save_state(pend_file); else if (!load_state(pend_file)) fprintf(stderr, "dkx: cannot load %s\n", pend_file.c_str());
		}
	}
}
static int r(u16 a) { return dk_rd(a); }
static int bcd(int v) { return (v >> 4) * 10 + (v & 15); }
static long SREF = 0;
static long score() {
	long v = bcd(r(0x60B4)) * 10000L + bcd(r(0x60B3)) * 100 + bcd(r(0x60B2));
	while (v < SREF - 400000) v += 1000000;
	return v;
}
static std::string stpath(const std::string &dir, const std::string &name) { return dir + "/" + name + ".sta"; }

// ---------------------------------------------------------------- mkfresh.lua

static long fr_n = 0; static FILE *fr_meta; static std::string fr_dir;
static void mkfresh_frame() {
	fr_n++;
	if (fr_n >= 800 && fr_n < 860) cur_in2 |= 0x80; else if (fr_n == 860) cur_in2 &= ~0x80;
	if (fr_n >= 1000 && fr_n < 1060) cur_in2 |= 0x04; else if (fr_n == 1060) cur_in2 &= ~0x04;
	if (fr_n > 1060 && r(0x600A) == 0x0C) {
		pend = 2; pend_file = stpath(fr_dir, "b000");
		fprintf(fr_meta, "0 %ld %d %d %d %d %ld\n", score(), r(0x6228), r(0x6229), r(0x6227), r(0x62B1), fr_n); fclose(fr_meta);
		quit = true;
	}
}

// ---------------------------------------------------------------- advance.lua

static std::string adv_P; static FILE *adv_f; static long adv_n = 0, adv_s0 = 0; static int adv_lives0 = 0;
static bool adv_cleared = false, adv_sacr = false, adv_left = false; static long adv_clr[3]; static long adv_died[2] = { -1, 0 };
static std::string adv_next, adv_dir;
static int P(const std::string &s, long k) { return s[k - 1] - 48; }   // Lua P:byte(k) - 48
static void adv_fail(const char *msg) { fputs(msg, adv_f); fclose(adv_f); quit = true; }
static void advance_frame() {
	long n = adv_n; long len = long(adv_P.size());
	if (n == 1) { adv_lives0 = r(0x6228); adv_s0 = score(); }
	int m = r(0x600A);
	char buf[256];
	if (adv_sacr) {
		if (adv_died[0] < 0) {
			if (m == 0x16) return adv_fail("FAIL cleared in sacrifice mode\n");
			if (m != 0x0C || r(0x6200) == 0 || r(0x639D) != 0 || r(0x6228) < adv_lives0) {
				adv_died[0] = n; adv_died[1] = score();
				if (n < len - 1) { snprintf(buf, sizeof buf, "FAIL early death at %ld of %ld\n", n, len); return adv_fail(buf); }
			}
			if (n < len) apply(P(adv_P, n + 1)); else apply(0);
			if (n > len + 5 && adv_died[0] < 0) return adv_fail("FAIL no death by path end\n");
		} else {
			apply(0);
			if (m != 0x0C) adv_left = true;
			if (adv_left && m == 0x0C && r(0x6228) == adv_lives0 - 1) {
				pend = 2; pend_file = stpath(adv_dir, adv_next);
				fprintf(adv_f, "%s %ld %d %d %d %d %ld gain=%ld deathf=%ld pathlen=%ld lives0=%d sac=1\n", adv_next.c_str(), score(), r(0x6228), r(0x6229), r(0x6227), r(0x62B1), n,
					(score() - adv_s0) % 1000000, adv_died[0], len, adv_lives0);
				fclose(adv_f); quit = true; return;
			}
			if (adv_lives0 == 1 && n > adv_died[0] + 600) {
				fprintf(adv_f, "GAMEOVER %ld gain=%ld deathf=%ld end=%ld\n", score(), (score() - adv_s0) % 1000000, adv_died[0], n); fclose(adv_f); quit = true; return;
			}
			if (n > adv_died[0] + 3000) return adv_fail("FAIL board did not restart\n");
		}
		adv_n++; return;
	}
	if (n <= len) {
		if (m == 0x16 && !adv_cleared) { adv_cleared = true; adv_clr[0] = n; adv_clr[1] = score(); adv_clr[2] = r(0x62B1); }
		if (!adv_cleared && (m != 0x0C || r(0x6200) == 0)) { snprintf(buf, sizeof buf, "FAIL dead at %ld mode=%02X\n", n, m); return adv_fail(buf); }
		if (n < len) apply(P(adv_P, n + 1)); else apply(0);
	} else {
		if (m == 0x16 && !adv_cleared) { adv_cleared = true; adv_clr[0] = n; adv_clr[1] = score(); adv_clr[2] = r(0x62B1); }
		apply(0);
		if (!adv_cleared) return adv_fail("FAIL no clear by path end\n");
		if (m == 0x0C) {
			pend = 2; pend_file = stpath(adv_dir, adv_next);
			fprintf(adv_f, "%s %ld %d %d %d %d %ld gain=%ld clearf=%ld pathlen=%ld lives0=%d\n", adv_next.c_str(), score(), r(0x6228), r(0x6229), r(0x6227), r(0x62B1), n,
				(score() - adv_s0) % 1000000, adv_clr[0], len, adv_lives0);
			fclose(adv_f); quit = true; return;
		}
		if (n > len + 3000) return adv_fail("FAIL no next board\n");
	}
	adv_n++;
}

static std::string read_line2(const std::string &path) {
	FILE *f = fopen(path.c_str(), "r"); if (!f) { fprintf(stderr, "dkx: cannot read %s\n", path.c_str()); exit(1); }
	std::string a, b; int c; int line = 0;
	while ((c = fgetc(f)) != EOF) { if (c == '\n') { if (++line == 2) break; continue; } if (c == '\r') continue; (line == 0 ? a : b) += char(c); }
	fclose(f); return b;
}

// ---------------------------------------------------------------- shared: candidate fields (gworker.lua / seedgen.lua)

static int BT = 1, T0 = 80, L0 = 3;
static void cand_fields(char *buf, size_t sz) {
	int hc = (r(0x6A18) > 0 ? 1 : 0) + (r(0x6A1C) > 0 ? 1 : 0);
	int nb = 0, nn = 0, my = r(0x6205), pg = 0;
	if (BT == 1) {
		for (int b = 0; b <= 9; b++) { int a = 0x6700 + 0x20 * b; if (r(a) & 1) { nb++; int dy = r(a + 5) - my; if (dy > -20 && dy < 12) nn++; } }
	} else {
		for (int b = 0; b <= 4; b++) { int a = 0x6400 + 0x20 * b; if (r(a) & 1) { nb++; int dy = r(a + 5) - my; if (dy > -20 && dy < 12) nn++; } }
		for (int a : { 0x6A0C, 0x6A10, 0x6A14 }) if (r(a) == 0) pg++;
		if (BT == 4) { pg += 4 * r(0x6290); for (int k = 0; k <= 7; k++) if (r(0x6292 + k) != 0) pg += (64 << k); }
	}
	snprintf(buf, sz, "%ld %d %d %d %d %d %d %d %d %d %d %d", score(), r(0x62B1), r(0x6203), r(0x6205), r(0x6217), r(0x6395) * 256 + r(0x6394), hc, r(0x6216), r(0x6215), nb, nn, pg);
}

// ---------------------------------------------------------------- seedgen.lua

static std::string sg_P, sg_pfx, sg_std; static FILE *sg_f; static long sg_n = 0, sg_step = 12;
static void seedgen_frame() {
	long n = sg_n; int m = r(0x600A);
	if (m == 0x16) { fprintf(sg_f, "CLEAR %ld %ld %d\n", n, score(), r(0x62B1)); fclose(sg_f); quit = true; return; }
	if (m != 0x0C || r(0x6200) == 0 || r(0x639D) != 0 || r(0x6228) < L0 || r(0x62B1) > T0 || n >= long(sg_P.size())) {
		fprintf(sg_f, "END %ld\n", n); fclose(sg_f); quit = true; return;
	}
	if (n % sg_step == 0 && r(0x6216) == 0) {
		char name[128], f[256]; snprintf(name, sizeof name, "%s_%ld", sg_pfx.c_str(), n);
		pend = 2; pend_file = stpath(sg_std, name);
		cand_fields(f, sizeof f);
		fprintf(sg_f, "S %ld %s %s\n", n, name, f);
	}
	apply(P(sg_P, n + 1));
	sg_n++;
}

// ---------------------------------------------------------------- gworker.lua (NS = 0)

struct Act { int mask; double w; int minh, maxh; };
static Act ACTS[8] = { {0, 2, 1, 40}, {1, 3, 2, 40}, {2, 3, 2, 40}, {4, 2, 4, 90}, {8, 0.4, 2, 16}, {16, 1.2, 0, 0}, {17, 1.4, 0, 0}, {18, 1.4, 0, 0} };
static double WSUM;
static u64 rs;                                           // xorshift64*: the search's own randomness (need not match Lua's)
static inline u64 rnext() { rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27; return rs * 2685821657736338717ULL; }
static inline double rnd() { return double(rnext() >> 11) * (1.0 / 9007199254740992.0); }
static inline int rint_(int a, int b) { return a + int(rnext() % u64(b - a + 1)); }   // Lua math.random(a, b)
static const Act &pick() { double x = rnd() * WSUM; for (auto &a : ACTS) { x -= a.w; if (x <= 0) return a; } return ACTS[0]; }
static std::vector<u8> inp; static std::vector<u8> ends;
static void mkroll() {
	inp.clear(); ends.clear();
	for (int k = rint_(2, 5); k > 0; k--) {
		const Act &a = pick();
		if (a.mask >= 16) {
			for (int p = rint_(1, 3); p > 0; p--) inp.push_back(u8(a.mask));
			static const int AFTER[4] = { 0, 1, 2, 0 };
			int which = rint_(0, 3); int after = which == 3 ? a.mask - 16 : AFTER[which];
			for (int q = rint_(0, 36); q > 0; q--) inp.push_back(u8(after));
		} else for (int q = rint_(a.minh, a.maxh); q > 0; q--) inp.push_back(u8(a.mask));
		ends.resize(inp.size() + 1, 0); ends[inp.size()] = 1;
	}
	ends.resize(inp.size() + 1, 0);
}
static std::string W_ID, JOBDIR, STD; static bool SACR = false;
static std::string out;                                   // result lines for the current job
static long cc = 0, idx = 0; static int phase = 0;        // 0 idle, 1 run, 2 ended
static const char *node_id;
struct Cand { long idx; std::string line; };
static std::vector<Cand> cands; static bool have_w = false; static long wrec[3];
static void enc_append(std::string &s, long n) { for (long i = 0; i < n; i++) s += char(48 + inp[i]); }
static void worker_frame() {
	if (phase != 1) return;
	idx++;
	int mode = r(0x600A);
	if (mode == 0x16) {
		if (SACR) { out += "D\n"; phase = 2; return; }
		have_w = true; wrec[0] = score(); wrec[1] = r(0x62B1); wrec[2] = idx; phase = 2; return;
	}
	if (mode != 0x0C || r(0x6200) == 0 || r(0x639D) != 0 || r(0x6228) < L0 || r(0x62B1) > T0) {
		if (SACR) { have_w = true; wrec[0] = score(); wrec[1] = 0; wrec[2] = idx; phase = 2; return; }
		out += "D\n"; phase = 2; return;
	}
	if (ends[idx]) {
		cc++;
		char name[64]; snprintf(name, sizeof name, "w%s_%ld", W_ID.c_str(), cc);
		pend = 2; pend_file = stpath(STD, name);
		char f[256]; cand_fields(f, sizeof f);
		std::string line = std::string("C ") + node_id + " " + name + " " + f;
		cands.push_back({ idx, line });
	}
	if (idx >= long(inp.size())) { phase = 2; return; }
	apply(inp[idx]);
}
static void run_rollout() {                // from a just-loaded node state until the rollout ends
	while (phase == 1) {
		dk_timeslice();
		if (pend) { int p = pend; pend = 0; if (p == 2) save_state(pend_file); }
	}
}
static bool file_exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }
static void worker_main() {
	W_ID = env("WID"); JOBDIR = env("JOBDIR"); STD = env("STD");
	rs = u64(envl("RS", 1)) * 0x9E3779B97F4A7C15ULL + 1;
	if (envl("NS", 0) != 0) { fprintf(stderr, "dkx worker: only NS=0 is supported\n"); exit(1); }
	if (BT == 2 || BT == 4) ACTS[4] = { 8, 2, 4, 90 };
	WSUM = 0; for (auto &a : ACTS) WSUM += a.w;
	script = worker_frame;
	std::string jobf = JOBDIR + "/w" + W_ID + ".job", tmpf = JOBDIR + "/w" + W_ID + ".tmp", resf = JOBDIR + "/w" + W_ID + ".res";
	char *buf = (char *)malloc(1 << 20);
	for (;;) {
		int fd = open(jobf.c_str(), O_RDONLY);
		if (fd < 0) { struct timespec ts = { 0, 50000 }; nanosleep(&ts, nullptr); continue; }
		ssize_t len = read(fd, buf, (1 << 20) - 1); close(fd); unlink(jobf.c_str());
		if (len < 0) len = 0;
		buf[len] = 0;
		out.clear();
		for (char *line = strtok(buf, "\n"); line; line = strtok(nullptr, "\n")) {
			if (!strcmp(line, "Q")) return;
			char id[64], file[128], pf[128], pi[1 << 16]; int n;
			if (sscanf(line, "J %63s %127s %d %127s %65535s", id, file, &n, pf, pi) != 5) continue;
			node_id = id;
			std::string sp = stpath(STD, file);
			for (int roll = 1; roll <= n; roll++) {
				if (!file_exists(sp)) { out += std::string("M ") + file + "\n"; break; }
				mkroll(); idx = 0; cands.clear(); have_w = false;
				apply(inp[0]);
				if (!load_state(sp)) { out += std::string("M ") + file + "\n"; break; }
				phase = 1; run_rollout();
				for (auto &c : cands) { out += c.line; out += " 0.000 "; enc_append(out, c.idx); out += "\n"; }
				if (have_w) {
					char w[160]; snprintf(w, sizeof w, "W %s %ld %ld 0.000 ", node_id, wrec[0], wrec[1]);
					out += w; enc_append(out, wrec[2]); out += "\n";
				}
			}
		}
		out += "END\n";
		int wfd = open(tmpf.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (wfd >= 0) { if (write(wfd, out.data(), out.size()) < 0) {} close(wfd); rename(tmpf.c_str(), resf.c_str()); }
	}
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: dkx mkfresh|advance|seedgen|worker  (settings via environment, see source)\n"); return 1; }
	std::string mode = argv[1];
	// ROM files: $DKX_ROMS, else roms/ beside the build directory (dkx/build/dkx -> dkx/roms)
	std::string romdir;
	if (getenv("DKX_ROMS")) romdir = getenv("DKX_ROMS");
	else {
		char exe[4096]; ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1); exe[k > 0 ? k : 0] = 0;
		std::string e = exe; e = e.substr(0, e.rfind('/')); romdir = e.substr(0, e.rfind('/')) + "/roms";
	}
	if (!dk_load_roms(romdir.c_str())) return 1;
	BT = int(envl("BT", 1)); T0 = int(envl("T0", 80)); L0 = int(envl("L0", 3)); SREF = envl("S0", 0); SACR = env("SACR") == "1";
	dk_frame_hook = hook;
	if (mode == "mkfresh") {
		fr_meta = fopen(env("META").c_str(), "w"); fr_dir = env("DKXST", ".");
		script = mkfresh_frame; dk_power_on(); run_until_quit();
	} else if (mode == "advance") {
		adv_P = read_line2(env("PATHF")); adv_f = fopen(env("META").c_str(), "w"); adv_sacr = SACR;
		adv_dir = env("DKXST", "."); adv_next = env("NEXT");
		script = advance_frame;
		// advance.lua: frame 0 applies P[1] and loads START; the hook then runs once per frame from n = 1
		dk_power_on(); apply(P(adv_P, 1));
		if (!load_state(stpath(adv_dir, env("START")))) { fprintf(stderr, "dkx: cannot load START\n"); return 1; }
		adv_n = 1; run_until_quit();
	} else if (mode == "seedgen") {
		sg_P = read_line2(env("INP")); sg_f = fopen(env("OUT").c_str(), "w"); sg_pfx = env("PFX", "sd"); sg_step = envl("STEP", 12); sg_std = env("STD");
		script = seedgen_frame;
		dk_power_on(); apply(P(sg_P, 1));
		if (!load_state(stpath(sg_std, "root"))) { fprintf(stderr, "dkx: cannot load root\n"); return 1; }
		sg_n = 1; run_until_quit();
	} else if (mode == "worker") {
		dk_power_on(); worker_main();
	} else { fprintf(stderr, "dkx: unknown mode %s\n", mode.c_str()); return 1; }
	return 0;
}
