// dkx: a lean, MAME-0.264-exact Donkey Kong board emulator.
// core.h: integer types, PAIR, and MAME's attotime arithmetic (emu/attotime.h), reproduced exactly.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef s64 attoseconds_t; typedef s32 seconds_t;

// little-endian host
union PAIR {
	struct { u8 l, h, h2, h3; } b;
	struct { u16 l, h; } w;
	struct { s8 l, h, h2, h3; } sb;
	struct { s16 l, h; } sw;
	u32 d;
	s32 sd;
};

constexpr attoseconds_t ATTOSECONDS_PER_SECOND_SQRT = 1'000'000'000;
constexpr attoseconds_t ATTOSECONDS_PER_SECOND = ATTOSECONDS_PER_SECOND_SQRT * ATTOSECONDS_PER_SECOND_SQRT;
constexpr seconds_t ATTOTIME_MAX_SECONDS = 1'000'000'000;
inline constexpr attoseconds_t HZ_TO_ATTOSECONDS(u32 x) { return attoseconds_t(ATTOSECONDS_PER_SECOND / x); }

inline u32 divu_64x32(u64 a, u32 b) { return u32(a / b); }
inline u32 divu_64x32_rem(u64 a, u32 b, u32 &rem) { rem = u32(a % b); return u32(a / b); }
inline u64 mulu_32x32(u32 a, u32 b) { return u64(a) * u64(b); }
inline s64 mul_32x32(s32 a, s32 b) { return s64(a) * s64(b); }

struct attotime {
	seconds_t s = 0;
	attoseconds_t a = 0;
	constexpr attotime() = default;
	constexpr attotime(seconds_t sec, attoseconds_t atto) : s(sec), a(atto) {}
	static constexpr attotime never() { return attotime(ATTOTIME_MAX_SECONDS, 0); }
	static constexpr attotime zero() { return attotime(0, 0); }
	constexpr bool is_never() const { return s >= ATTOTIME_MAX_SECONDS; }
	constexpr bool is_zero() const { return s == 0 && a == 0; }
	constexpr attoseconds_t as_attoseconds() const {
		return (s == 0) ? a : (s == -1) ? (a - ATTOSECONDS_PER_SECOND) : (s > 0) ? ATTOSECONDS_PER_SECOND : -ATTOSECONDS_PER_SECOND;
	}
	static attotime from_hz(u32 f) { return (f > 1) ? attotime(0, HZ_TO_ATTOSECONDS(f)) : (f == 1) ? attotime(1, 0) : never(); }
	static attotime from_usec(s64 usec) { return attotime(usec / 1000000, (usec % 1000000) * (ATTOSECONDS_PER_SECOND / 1000000)); }
	attotime &operator*=(u32 factor) {
		if (s >= ATTOTIME_MAX_SECONDS) return *this = never();
		if (factor == 0) return *this = zero();
		u32 attolo; u32 attohi = divu_64x32_rem(a, ATTOSECONDS_PER_SECOND_SQRT, attolo);
		u64 temp = mulu_32x32(attolo, factor); u32 reslo;
		temp = divu_64x32_rem(temp, ATTOSECONDS_PER_SECOND_SQRT, reslo);
		temp += mulu_32x32(attohi, factor); u32 reshi;
		temp = divu_64x32_rem(temp, ATTOSECONDS_PER_SECOND_SQRT, reshi);
		temp += mulu_32x32(s, factor);
		if (temp >= ATTOTIME_MAX_SECONDS) return *this = never();
		s = seconds_t(temp); a = attoseconds_t(reslo) + mul_32x32(reshi, ATTOSECONDS_PER_SECOND_SQRT);
		return *this;
	}
	double as_double() const { return double(s) + double(a) * 1e-18; }
};
inline constexpr attotime operator+(const attotime &l, const attotime &r) {
	if (l.s >= ATTOTIME_MAX_SECONDS || r.s >= ATTOTIME_MAX_SECONDS) return attotime::never();
	attotime res(l.s + r.s, l.a + r.a);
	if (res.a >= ATTOSECONDS_PER_SECOND) { res.a -= ATTOSECONDS_PER_SECOND; res.s++; }
	if (res.s >= ATTOTIME_MAX_SECONDS) return attotime::never();
	return res;
}
inline constexpr attotime operator-(const attotime &l, const attotime &r) {
	if (l.s >= ATTOTIME_MAX_SECONDS) return attotime::never();
	attotime res(l.s - r.s, l.a - r.a);
	if (res.a < 0) { res.a += ATTOSECONDS_PER_SECOND; res.s--; }
	return res;
}
inline attotime &operator+=(attotime &l, const attotime &r) { return l = l + r; }
inline attotime &operator-=(attotime &l, const attotime &r) { return l = l - r; }
inline constexpr bool operator==(const attotime &l, const attotime &r) { return l.s == r.s && l.a == r.a; }
inline constexpr bool operator!=(const attotime &l, const attotime &r) { return !(l == r); }
inline constexpr bool operator<(const attotime &l, const attotime &r) { return l.s < r.s || (l.s == r.s && l.a < r.a); }
inline constexpr bool operator<=(const attotime &l, const attotime &r) { return l.s < r.s || (l.s == r.s && l.a <= r.a); }
inline constexpr bool operator>(const attotime &l, const attotime &r) { return r < l; }
inline constexpr bool operator>=(const attotime &l, const attotime &r) { return r <= l; }
inline attotime atto_max(const attotime &x, const attotime &y) { return x < y ? y : x; }

// line states (emu/didisasm etc.)
enum { CLEAR_LINE = 0, ASSERT_LINE = 1, HOLD_LINE = 2 };
