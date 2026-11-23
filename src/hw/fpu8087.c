/* MezinXT - the Intel 8087 numeric data processor (fpu8087.h).
 *
 * Written from Intel's 8087 data sheet and the iAPX 86/88 numerics
 * description (formats, instruction set, exception responses, typical
 * execution times) and from first principles for the arithmetic: exact
 * integer algorithms (aligned add, schoolbook multiply, restoring divide,
 * square root and partial remainder digit by digit) on a 64-bit significand
 * with 128-bit intermediates, rounded once. The transcendental functions are
 * Taylor and arctangent/hyperbolic-arctangent series in a 128-bit software
 * float, rounded once to the extended format. MezinXT's own code; no
 * emulator or soft-float library consulted. */
#include "fpu8087.h"

#include <string.h>

#define BIAS 16383
#define EXP_MAX 0x7FFF
#define WRAP 24576                      /* the exponent adjustment of an unmasked over- or underflow */

enum { RC_NEAREST, RC_DOWN, RC_UP, RC_CHOP };
enum { C_ZERO, C_NORMAL, C_UNNORMAL, C_DENORMAL, C_INF, C_NAN };

/* --- 128-bit integers ---------------------------------------------------------------- */

typedef struct u128 { uint64_t hi, lo; } u128;

static u128 mk(uint64_t hi, uint64_t lo) { u128 r; r.hi = hi; r.lo = lo; return r; }
static int is0(u128 a) { return !(a.hi | a.lo); }
static int cmp128(u128 a, u128 b) {
    if (a.hi != b.hi) return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo) return a.lo < b.lo ? -1 : 1;
    return 0;
}
static u128 add128(u128 a, u128 b) { u128 r; r.lo = a.lo + b.lo; r.hi = a.hi + b.hi + (r.lo < a.lo); return r; }
static u128 sub128(u128 a, u128 b) { u128 r; r.lo = a.lo - b.lo; r.hi = a.hi - b.hi - (a.lo < b.lo); return r; }
static u128 shl128(u128 a, uint32_t n) {
    if (n == 0) return a;
    if (n >= 128) return mk(0, 0);
    if (n >= 64) return mk(a.lo << (n - 64), 0);
    return mk((a.hi << n) | (a.lo >> (64 - n)), a.lo << n);
}
static u128 shr128(u128 a, uint32_t n) {
    if (n == 0) return a;
    if (n >= 128) return mk(0, 0);
    if (n >= 64) return mk(0, a.hi >> (n - 64));
    return mk(a.hi >> n, (a.lo >> n) | (a.hi << (64 - n)));
}
/* Shifts right, OR-ing the bits shifted out into bit 0 (sticky). */
static u128 shr_jam(u128 a, uint32_t n) {
    if (n == 0) return a;
    if (n >= 128) return mk(0, !is0(a));
    u128 r = shr128(a, n);
    if (cmp128(shl128(r, n), a)) r.lo |= 1u;
    return r;
}
static unsigned clz64(uint64_t x) {
    unsigned n = 0;
    if (!x) return 64;
    if (!(x >> 32)) { n += 32; x <<= 32; }
    if (!(x >> 48)) { n += 16; x <<= 16; }
    if (!(x >> 56)) { n += 8; x <<= 8; }
    if (!(x >> 60)) { n += 4; x <<= 4; }
    if (!(x >> 62)) { n += 2; x <<= 2; }
    if (!(x >> 63)) n += 1;
    return n;
}
static unsigned clz128(u128 a) { return a.hi ? clz64(a.hi) : 64u + clz64(a.lo); }
/* The full product of two 64-bit numbers, from four 32-bit products. */
static u128 mul64(uint64_t a, uint64_t b) {
    const uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    return mk(p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), (mid << 32) | (p00 & 0xFFFFFFFFu));
}

/* --- values -------------------------------------------------------------------------- */

static mx_f80 f80(int sign, uint32_t exp, uint64_t m) {
    mx_f80 x;
    x.se = (uint16_t)((sign ? 0x8000u : 0u) | (exp & EXP_MAX));
    x.m = m;
    return x;
}
static const mx_f80 INDEFINITE = {0xC000000000000000u, 0xFFFFu};
static mx_f80 zero(int sign) { return f80(sign, 0, 0); }
static mx_f80 inf(int sign) { return f80(sign, EXP_MAX, 1ull << 63); }
static int sgn(mx_f80 x) { return x.se >> 15; }
static uint32_t expo(mx_f80 x) { return x.se & EXP_MAX; }

/* Exponent 7FFFh: infinity when the fraction (bits 62-0) is zero, else a
 * NaN. A zero significand is a zero whatever the exponent (pseudo-zeros
 * included). Exponent 0 otherwise a denormal; integer bit clear an
 * unnormal. */
static int classify(mx_f80 x) {
    const uint32_t e = expo(x);
    if (e == EXP_MAX) return (x.m << 1) ? C_NAN : C_INF;
    if (!x.m) return C_ZERO;
    if (e == 0) return C_DENORMAL;
    return x.m >> 63 ? C_NORMAL : C_UNNORMAL;
}

static mx_f80 from_u64(int sign, uint64_t v) {
    if (!v) return zero(sign);
    const unsigned z = clz64(v);
    return f80(sign, (uint32_t)(BIAS + 63 - (int)z), v << z);
}

/* A number unpacked: (-1)^sign * m / 2^127 * 2^(e - BIAS); a denormal's
 * exponent is the smallest, 1. */
typedef struct num { int sign; int32_t e; u128 m; } num;

static num unpack(mx_f80 x) {
    num n;
    n.sign = sgn(x);
    n.e = expo(x) ? (int32_t)expo(x) : 1;
    n.m = mk(x.m, 0);
    return n;
}

/* --- registers, flags ---------------------------------------------------------------- */

#define PHYS(f, i) (((unsigned)(f)->top + (i)) & 7u)

static int empty_at(const mx_fpu8087 *f, unsigned i) { return (f->empty >> PHYS(f, i)) & 1u; }
static mx_f80 get(const mx_fpu8087 *f, unsigned i) { return f->reg[PHYS(f, i)]; }
static void put(mx_fpu8087 *f, unsigned i, mx_f80 x) {
    f->reg[PHYS(f, i)] = x;
    f->empty &= (uint8_t)~(1u << PHYS(f, i));
}
static void pop(mx_fpu8087 *f) {
    f->empty |= (uint8_t)(1u << f->top);
    f->top = (uint8_t)((f->top + 1u) & 7u);
}
static void push(mx_fpu8087 *f, mx_f80 x) {
    f->top = (uint8_t)((f->top + 7u) & 7u);
    put(f, 0, x);
}

static int unmasked(const mx_fpu8087 *f, unsigned e) { return (e & ~(unsigned)f->cw & 0x3Fu) != 0; }
/* Raises exception flags; 1 when one of them is unmasked: the instruction
 * stops there, its operands unchanged. */
static int raise(mx_fpu8087 *f, unsigned e) {
    f->sw |= (uint16_t)e;
    return unmasked(f, e);
}
static int rc_of(const mx_fpu8087 *f) { return (f->cw >> 10) & 3; }
static int projective(const mx_fpu8087 *f) { return !(f->cw & MX_FPU_IC); }
/* Precision control: 24, 53 or 64 bits (the reserved setting 01 as 64). */
static int pc_bits(const mx_fpu8087 *f) {
    switch ((f->cw >> 8) & 3u) {
    case 0: return 24;
    case 2: return 53;
    default: return 64;
    }
}
static void set_cc(mx_fpu8087 *f, int c3, int c2, int c1, int c0) {
    f->sw = (uint16_t)((f->sw & ~(MX_FPU_C0 | MX_FPU_C1 | MX_FPU_C2 | MX_FPU_C3)) | (c0 ? MX_FPU_C0 : 0) | (c1 ? MX_FPU_C1 : 0) |
                       (c2 ? MX_FPU_C2 : 0) | (c3 ? MX_FPU_C3 : 0));
}
static int invalid(mx_fpu8087 *f, mx_f80 *res) {
    if (raise(f, MX_FPU_IE)) return 0;
    *res = INDEFINITE;
    return 1;
}
/* The NaN an operation passes on: the one with the larger significand. */
static mx_f80 nan_of(mx_f80 a, mx_f80 b) {
    const int na = classify(a) == C_NAN, nb = classify(b) == C_NAN;
    if (na && nb) return a.m >= b.m ? a : b;
    return na ? a : b;
}

/* --- rounding ------------------------------------------------------------------------ */

/* A destination: precision and exponent range, as extended exponents. */
typedef struct fmt { int p; int32_t emin, emax; } fmt;
static const fmt F_SINGLE = {24, BIAS - 126, BIAS + 127};
static const fmt F_DOUBLE = {53, BIAS - 1022, BIAS + 1023};

/* A rounded result: e an extended exponent; inf for infinity. */
typedef struct rnd { int sign, inf; int32_t e; uint64_t m; } rnd;

/* Rounds the 128-bit m to its top p bits by rc; carry when it reached 2^128.
 * Returns 1 when inexact. */
static int round_bits(u128 m, int p, int rc, int sign, uint64_t *out, int *carry) {
    const uint64_t lsb = p == 64 ? 1u : 1ull << (64 - p);
    const uint64_t kept = m.hi & ~(lsb - 1u);
    const uint64_t rest = m.hi & (lsb - 1u);
    const int inexact = rest || m.lo;
    int up = 0;
    if (inexact) {
        switch (rc) {
        case RC_NEAREST: {
            int half, below;
            if (p == 64) { half = (int)(m.lo >> 63); below = (m.lo << 1) != 0; }
            else { half = (rest & (lsb >> 1)) != 0; below = (rest & ((lsb >> 1) - 1u)) || m.lo; }
            up = half && (below || (kept & lsb));
            break;
        }
        case RC_DOWN: up = sign; break;
        case RC_UP: up = !sign; break;
        default: break;
        }
    }
    *out = kept + (up ? lsb : 0u);
    *carry = up && *out == 0;
    return inexact;
}

/* Rounds (-1)^sign * m/2^127 * 2^(e-BIAS) into fm by rc, normalized first
 * unless `keep` (unnormal arithmetic keeps the exponent). Over- and underflow
 * as the 8087 answers them: masked, infinity or the largest number by the
 * rounding, a denormal; unmasked, the exponent wrapped by 24576 into a
 * register, nothing into memory (returns 0). Raises U, O, P. Underflow is
 * tininess after rounding, flagged masked only with a loss of accuracy. */
static int pack(mx_fpu8087 *f, int sign, int32_t e, u128 m, const fmt *fm, int rc, int keep, int memory, rnd *out) {
    uint64_t r;
    int carry, inexact;
    out->sign = sign;
    out->inf = 0;
    if (is0(m)) { out->e = 0; out->m = 0; return 1; }
    if (!keep) {
        const unsigned z = clz128(m);
        m = shl128(m, z);
        e -= (int32_t)z;
    }
    if (e < fm->emin) {
        round_bits(m, fm->p, rc, sign, &r, &carry);
        if (e + carry < fm->emin) {
            if (unmasked(f, MX_FPU_UE)) {
                if (memory) { f->sw |= MX_FPU_UE; return 0; }
                inexact = round_bits(m, fm->p, rc, sign, &r, &carry);
                if (carry) { r = 1ull << 63; e++; }
                f->sw |= (uint16_t)(MX_FPU_UE | (inexact ? MX_FPU_PE : 0));
                out->e = e + WRAP;
                out->m = r;
                return 1;
            }
            m = shr_jam(m, (uint32_t)(fm->emin - e));
            inexact = round_bits(m, fm->p, rc, sign, &r, &carry);
            if (inexact) f->sw |= MX_FPU_UE | MX_FPU_PE;
            out->e = r ? fm->emin : 0;
            out->m = r;
            return 1;
        }
    }
    inexact = round_bits(m, fm->p, rc, sign, &r, &carry);
    if (carry) { r = 1ull << 63; e++; }
    if (e > fm->emax) {
        if (unmasked(f, MX_FPU_OE)) {
            if (memory) { f->sw |= MX_FPU_OE; return 0; }
            f->sw |= (uint16_t)(MX_FPU_OE | (inexact ? MX_FPU_PE : 0));
            out->e = e - WRAP;
            out->m = r;
            return 1;
        }
        f->sw |= MX_FPU_OE | MX_FPU_PE;
        if (rc == RC_NEAREST || (rc == RC_UP && !sign) || (rc == RC_DOWN && sign)) {
            out->inf = 1;
            out->e = EXP_MAX;
            out->m = 1ull << 63;
        } else {
            out->e = fm->emax;
            out->m = fm->p == 64 ? ~0ull : ~0ull << (64 - fm->p);
        }
        return 1;
    }
    if (inexact) f->sw |= MX_FPU_PE;
    out->e = e;
    out->m = r;
    return 1;
}

static mx_f80 reg_of(const rnd *r) {
    if (r->inf) return inf(r->sign);
    if (!r->m) return zero(r->sign);
    if (r->e == 1 && !(r->m >> 63)) return f80(r->sign, 0, r->m);       /* a denormal */
    return f80(r->sign, (uint32_t)r->e, r->m);
}

/* A result into a register at precision p by the control word's rounding. */
static int deliver(mx_fpu8087 *f, int sign, int32_t e, u128 m, int p, int keep, mx_f80 *res) {
    const fmt fm = {p, 1, EXP_MAX - 1};
    rnd r;
    pack(f, sign, e, m, &fm, rc_of(f), keep, 0, &r);
    *res = reg_of(&r);
    return 1;
}

/* --- arithmetic ---------------------------------------------------------------------- */

static int denormal(int c) { return c == C_DENORMAL; }

/* a + b (b negated when negb). The sum is aligned exactly (bits below 126
 * places jammed into a sticky bit) and rounded once. When the operand with
 * the larger exponent is unnormal (and no normal one shares it), the result
 * keeps that exponent: the 8087's unnormal arithmetic. */
static int op_add(mx_fpu8087 *f, mx_f80 a, mx_f80 b, int negb, mx_f80 *res) {
    int ca = classify(a), cb = classify(b);
    const int sa = sgn(a), sb = sgn(b) ^ negb;
    const int rc = rc_of(f);
    if (ca == C_NAN || cb == C_NAN) {
        if (raise(f, MX_FPU_IE)) return 0;
        *res = nan_of(a, b);
        return 1;
    }
    if (ca == C_INF || cb == C_INF) {
        if (ca == C_INF && cb == C_INF && (projective(f) || sa != sb)) return invalid(f, res);
        if ((denormal(ca) || denormal(cb)) && raise(f, MX_FPU_DE)) return 0;
        *res = inf(ca == C_INF ? sa : sb);
        return 1;
    }
    if ((denormal(ca) || denormal(cb)) && raise(f, MX_FPU_DE)) return 0;
    if (ca == C_ZERO && cb == C_ZERO) {
        *res = zero(sa == sb ? sa : rc == RC_DOWN);
        return 1;
    }
    num A = unpack(a), B = unpack(b);
    A.sign = sa;
    B.sign = sb;
    if (B.e > A.e) {
        const num t = A; A = B; B = t;
        const int c = ca; ca = cb; cb = c;
    }
    const int keep = (ca == C_UNNORMAL || ca == C_DENORMAL) && !(B.e == A.e && cb == C_NORMAL);
    int32_t e = A.e + 1;
    const u128 ma = shr128(A.m, 1), mb = shr_jam(shr128(B.m, 1), (uint32_t)(A.e - B.e));
    u128 m;
    int sign;
    if (A.sign == B.sign) { m = add128(ma, mb); sign = A.sign; }
    else if (cmp128(ma, mb) >= 0) { m = sub128(ma, mb); sign = A.sign; }
    else { m = sub128(mb, ma); sign = B.sign; }
    if (is0(m)) {
        *res = zero(rc == RC_DOWN);
        return 1;
    }
    if (keep && !(m.hi >> 63)) { m = shl128(m, 1); e--; }
    return deliver(f, sign, e, m, pc_bits(f), keep, res);
}

/* a * b: the full 128-bit product, normalized by at most one place (an
 * unnormal operand gives an unnormal product). */
static int op_mul(mx_fpu8087 *f, mx_f80 a, mx_f80 b, mx_f80 *res) {
    const int ca = classify(a), cb = classify(b), s = sgn(a) ^ sgn(b);
    if (ca == C_NAN || cb == C_NAN) {
        if (raise(f, MX_FPU_IE)) return 0;
        *res = nan_of(a, b);
        return 1;
    }
    if (ca == C_INF || cb == C_INF) {
        if (ca == C_ZERO || cb == C_ZERO) return invalid(f, res);
        if ((denormal(ca) || denormal(cb)) && raise(f, MX_FPU_DE)) return 0;
        *res = inf(s);
        return 1;
    }
    if ((denormal(ca) || denormal(cb)) && raise(f, MX_FPU_DE)) return 0;
    if (ca == C_ZERO || cb == C_ZERO) { *res = zero(s); return 1; }
    const num A = unpack(a), B = unpack(b);
    u128 m = mul64(A.m.hi, B.m.hi);
    int32_t e = A.e + B.e - BIAS + 1;
    if (!(m.hi >> 63)) { m = shl128(m, 1); e--; }
    return deliver(f, s, e, m, pc_bits(f), 1, res);
}

/* floor(a * 2^127 / b) for b with its top bit set, restoring division; the
 * remainder's presence in bit 0. */
static u128 div_mant(uint64_t a, uint64_t b) {
    u128 rem = mk(0, a), q = mk(0, 0);
    const u128 d = mk(0, b);
    for (int i = 127; i >= 0; --i) {
        if (cmp128(rem, d) >= 0) {
            rem = sub128(rem, d);
            if (i >= 64) q.hi |= 1ull << (i - 64);
            else q.lo |= 1ull << i;
        }
        rem = shl128(rem, 1);
    }
    if (!is0(rem)) q.lo |= 1u;
    return q;
}

/* a / b. An unnormal divisor is invalid; a denormal one is normalized. */
static int op_div(mx_fpu8087 *f, mx_f80 a, mx_f80 b, mx_f80 *res) {
    const int ca = classify(a), cb = classify(b), s = sgn(a) ^ sgn(b);
    if (ca == C_NAN || cb == C_NAN) {
        if (raise(f, MX_FPU_IE)) return 0;
        *res = nan_of(a, b);
        return 1;
    }
    if ((ca == C_INF && cb == C_INF) || (ca == C_ZERO && cb == C_ZERO) || cb == C_UNNORMAL) return invalid(f, res);
    if ((denormal(ca) || denormal(cb)) && raise(f, MX_FPU_DE)) return 0;
    if (ca == C_INF) { *res = inf(s); return 1; }
    if (cb == C_INF) { *res = zero(s); return 1; }
    if (cb == C_ZERO) {
        if (raise(f, MX_FPU_ZE)) return 0;
        *res = inf(s);
        return 1;
    }
    if (ca == C_ZERO) { *res = zero(s); return 1; }
    const num A = unpack(a);
    num B = unpack(b);
    const unsigned z = clz64(B.m.hi);
    B.m.hi <<= z;
    B.e -= (int32_t)z;
    u128 q = div_mant(A.m.hi, B.m.hi);
    int32_t e = A.e - B.e + BIAS;
    if (!(q.hi >> 63)) { q = shl128(q, 1); e--; }
    return deliver(f, s, e, q, pc_bits(f), 1, res);
}

/* The square root digit by digit: 67 bits of root and a sticky remainder. */
static int op_sqrt(mx_fpu8087 *f, mx_f80 a, mx_f80 *res) {
    const int c = classify(a);
    if (c == C_NAN) {
        if (raise(f, MX_FPU_IE)) return 0;
        *res = a;
        return 1;
    }
    if (c == C_ZERO) { *res = zero(sgn(a)); return 1; }
    if (c == C_INF) {
        if (projective(f) || sgn(a)) return invalid(f, res);
        *res = a;
        return 1;
    }
    if (sgn(a) || c == C_UNNORMAL) return invalid(f, res);
    if (c == C_DENORMAL && raise(f, MX_FPU_DE)) return 0;
    num A = unpack(a);
    const unsigned z = clz64(A.m.hi);
    A.m.hi <<= z;
    A.e -= (int32_t)z;
    /* value = M * 2^F with F even */
    int32_t F = A.e - BIAS - 63;
    u128 M = mk(0, A.m.hi);
    if (F % 2 != 0) { M = shl128(M, 1); F -= 1; }
    u128 rem = mk(0, 0), root = mk(0, 0);
    for (int i = 66; i >= 0; --i) {
        const int pair = i - 34;                    /* 33 pairs of M, then 34 of zeros */
        rem = shl128(rem, 2);
        if (pair >= 0) rem.lo |= shr128(M, (uint32_t)(2 * pair)).lo & 3u;
        u128 t = shl128(root, 2);
        t.lo |= 1u;
        root = shl128(root, 1);
        if (cmp128(rem, t) >= 0) { rem = sub128(rem, t); root.lo |= 1u; }
    }
    root = shl128(root, 1);
    if (!is0(rem)) root.lo |= 1u;
    /* root * 2^(F/2 - 35) */
    return deliver(f, 0, F / 2 - 35 + 127 + BIAS, root, pc_bits(f), 0, res);
}

/* x rounded to an integer by rc: magnitude in *mag, 0 when it needs more
 * than 64 bits. x finite. */
static int to_integer(mx_f80 x, int rc, uint64_t *mag, int *inexact) {
    const int sign = sgn(x);
    *inexact = 0;
    *mag = 0;
    if (!x.m) return 1;
    const int32_t E = (expo(x) ? (int32_t)expo(x) : 1) - BIAS;
    const int32_t sh = 63 - E;                     /* fraction bits */
    const uint64_t m = x.m;
    if (sh <= 0) {
        const uint32_t l = (uint32_t)(-sh);
        if (l >= 64 || (l && (m >> (64 - l)))) return 0;
        *mag = m << l;
        return 1;
    }
    uint64_t ip, frac;
    int half, above;
    if (sh >= 65) { ip = 0; half = 0; above = 0; frac = m; }
    else if (sh == 64) { ip = 0; frac = m; half = (int)(m >> 63); above = (m << 1) != 0; }
    else {
        ip = m >> sh;
        frac = m & ((1ull << sh) - 1u);
        half = (int)((frac >> (sh - 1)) & 1u);
        above = (frac & ((1ull << (sh - 1)) - 1u)) != 0;
    }
    *inexact = frac != 0;
    int up = 0;
    if (frac) {
        switch (rc) {
        case RC_NEAREST: up = half && (above || (ip & 1u)); break;
        case RC_DOWN: up = sign; break;
        case RC_UP: up = !sign; break;
        default: break;
        }
    }
    if (up) {
        ip++;
        if (!ip) return 0;
    }
    *mag = ip;
    return 1;
}

/* --- memory formats ------------------------------------------------------------------ */

static uint8_t rd8(mx_fpu8087 *f, uint32_t a) { return f->host.read(f->host.ctx, a & 0xFFFFFu); }
static void wr8(mx_fpu8087 *f, uint32_t a, uint8_t v) { f->host.write(f->host.ctx, a & 0xFFFFFu, v); }
static uint64_t rdn(mx_fpu8087 *f, uint32_t a, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= (uint64_t)rd8(f, a + i) << (8 * i);
    return v;
}
static void wrn(mx_fpu8087 *f, uint32_t a, uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) wr8(f, a + i, (uint8_t)(v >> (8 * i)));
}
static mx_f80 rd80(mx_fpu8087 *f, uint32_t a) {
    mx_f80 x;
    x.m = rdn(f, a, 8);
    x.se = (uint16_t)rdn(f, a + 8, 2);
    return x;
}
static void wr80(mx_fpu8087 *f, uint32_t a, mx_f80 x) {
    wrn(f, a, x.m, 8);
    wrn(f, a + 8, x.se, 2);
}

/* A short (dbl 0) or long real as a temporary real, exactly; a denormal
 * becomes an unnormal of the smallest exponent of its format. *cls is the
 * class of the source. */
static mx_f80 from_real(uint64_t v, int dbl, int *cls) {
    const unsigned ebits = dbl ? 11u : 8u, fbits = dbl ? 52u : 23u;
    const int32_t bias = dbl ? 1023 : 127;
    const int sign = (int)((v >> (ebits + fbits)) & 1u);
    const uint32_t e = (uint32_t)(v >> fbits) & ((1u << ebits) - 1u);
    const uint64_t frac = v & ((1ull << fbits) - 1u);
    const unsigned sh = 63u - fbits;
    if (e == (1u << ebits) - 1u) {
        *cls = frac ? C_NAN : C_INF;
        return f80(sign, EXP_MAX, (1ull << 63) | (frac << sh));
    }
    if (e == 0) {
        if (!frac) { *cls = C_ZERO; return zero(sign); }
        *cls = C_DENORMAL;
        return f80(sign, (uint32_t)(BIAS - bias + 1), frac << sh);
    }
    *cls = C_NORMAL;
    return f80(sign, (uint32_t)(BIAS - bias + (int32_t)e), (1ull << 63) | (frac << sh));
}

/* A register as a short or long real, rounded by the control word; 0 when
 * an unmasked over- or underflow leaves memory unchanged. A NaN keeps its
 * top fraction bits. */
static int to_real(mx_fpu8087 *f, mx_f80 x, int dbl, uint64_t *out) {
    const unsigned ebits = dbl ? 11u : 8u, fbits = dbl ? 52u : 23u;
    const int32_t bias = dbl ? 1023 : 127;
    const uint64_t sbit = (uint64_t)sgn(x) << (ebits + fbits), all = (uint64_t)((1u << ebits) - 1u) << fbits;
    const uint64_t fmask = (1ull << fbits) - 1u;
    switch (classify(x)) {
    case C_ZERO: *out = sbit; return 1;
    case C_INF: *out = sbit | all; return 1;
    case C_NAN: {
        uint64_t frac = (x.m & ~(1ull << 63)) >> (63u - fbits);
        if (!frac) frac = 1ull << (fbits - 1u);
        *out = sbit | all | frac;
        return 1;
    }
    default: break;
    }
    const num n = unpack(x);
    rnd r;
    if (!pack(f, n.sign, n.e, n.m, dbl ? &F_DOUBLE : &F_SINGLE, rc_of(f), 0, 1, &r)) return 0;
    if (r.inf) *out = sbit | all;
    else if (!r.m) *out = sbit;
    else if (!(r.m >> 63)) *out = sbit | (r.m >> (63u - fbits));
    else *out = sbit | ((uint64_t)(r.e - BIAS + bias) << fbits) | ((r.m >> (63u - fbits)) & fmask);
    return 1;
}

/* A register as a two's complement integer of `bits`; the integer
 * indefinite (only the sign bit) for a NaN, an infinity or a number out of
 * range. 0 when an unmasked invalid operation stops the store. */
static int to_int(mx_fpu8087 *f, mx_f80 x, unsigned bits, uint64_t *out) {
    const int c = classify(x);
    const uint64_t indef = 1ull << (bits - 1u), mask = bits == 64 ? ~0ull : (1ull << bits) - 1u;
    uint64_t mag = 0;
    int inexact = 0;
    int ok = c != C_NAN && c != C_INF && to_integer(x, rc_of(f), &mag, &inexact);
    if (ok) ok = mag <= (sgn(x) ? indef : indef - 1u);
    if (!ok) {
        if (raise(f, MX_FPU_IE)) return 0;
        *out = indef;
        return 1;
    }
    if (inexact) f->sw |= MX_FPU_PE;
    *out = (sgn(x) ? 0u - mag : mag) & mask;
    return 1;
}

static mx_f80 from_bcd(const uint8_t b[10]) {
    uint64_t v = 0;
    for (int i = 8; i >= 0; --i) v = v * 100u + (uint64_t)(b[i] >> 4) * 10u + (b[i] & 15u);
    return from_u64(b[9] >> 7, v);
}

/* A register as 18 packed BCD digits; the BCD indefinite out of range. */
static int to_bcd(mx_fpu8087 *f, mx_f80 x, uint8_t b[10]) {
    const int c = classify(x);
    uint64_t mag = 0;
    int inexact = 0;
    const int ok = c != C_NAN && c != C_INF && to_integer(x, rc_of(f), &mag, &inexact) && mag <= 999999999999999999ull;
    if (!ok) {
        if (raise(f, MX_FPU_IE)) return 0;
        memset(b, 0, 10);
        b[7] = 0xC0;
        b[8] = b[9] = 0xFF;
        return 1;
    }
    if (inexact) f->sw |= MX_FPU_PE;
    for (int i = 0; i < 9; ++i) {
        const unsigned lo = (unsigned)(mag % 10u);
        mag /= 10u;
        const unsigned hi = (unsigned)(mag % 10u);
        mag /= 10u;
        b[i] = (uint8_t)(hi << 4 | lo);
    }
    b[9] = sgn(x) ? 0x80u : 0u;
    return 1;
}

/* --- comparison, examination ---------------------------------------------------------- */

/* |a| against |b|, both finite and nonzero or infinite. */
static int cmp_mag(mx_f80 a, mx_f80 b) {
    const int ia = classify(a) == C_INF, ib = classify(b) == C_INF;
    if (ia || ib) return ia == ib ? 0 : ia ? 1 : -1;
    int32_t ea = expo(a) ? (int32_t)expo(a) : 1, eb = expo(b) ? (int32_t)expo(b) : 1;
    const unsigned za = clz64(a.m), zb = clz64(b.m);
    const uint64_t ma = a.m << za, mb = b.m << zb;
    ea -= (int32_t)za;
    eb -= (int32_t)zb;
    if (ea != eb) return ea < eb ? -1 : 1;
    return ma == mb ? 0 : ma < mb ? -1 : 1;
}

/* Sets C3 C2 C0 for a against b: 000 greater, 001 less, 100 equal, 111
 * unordered (a NaN; with projective closure an infinity against a number),
 * which is invalid. 0 when an unmasked exception stops the instruction. */
static int op_compare(mx_fpu8087 *f, mx_f80 a, mx_f80 b) {
    const int ca = classify(a), cb = classify(b);
    if (ca == C_NAN || cb == C_NAN || (projective(f) && (ca == C_INF) != (cb == C_INF))) {
        set_cc(f, 1, 1, 0, 1);
        return !raise(f, MX_FPU_IE);
    }
    if ((denormal(ca) || denormal(cb)) && raise(f, MX_FPU_DE)) return 0;
    int r;
    if (ca == C_ZERO && cb == C_ZERO) r = 0;
    else if (ca == C_INF && cb == C_INF && projective(f)) r = 0;
    else if (ca == C_ZERO) r = sgn(b) ? 1 : -1;
    else if (cb == C_ZERO) r = sgn(a) ? -1 : 1;
    else if (sgn(a) != sgn(b)) r = sgn(a) ? -1 : 1;
    else r = sgn(a) ? -cmp_mag(a, b) : cmp_mag(a, b);
    set_cc(f, r == 0, 0, 0, r < 0);
    return 1;
}

/* FXAM: the 8087's sixteen codes in C3 C2 C1 C0 (C1 the sign). */
static void op_xam(mx_fpu8087 *f) {
    const mx_f80 x = get(f, 0);
    const int s = sgn(x);
    if (empty_at(f, 0)) { set_cc(f, 1, 0, s, 1); return; }
    const uint32_t e = expo(x);
    if (e == EXP_MAX) set_cc(f, 0, (x.m << 1) == 0, s, 1);         /* infinity 0101, NaN 0001 */
    else if (e == 0) set_cc(f, 1, x.m != 0, s, 0);                  /* zero 1000, denormal 1100 */
    else set_cc(f, 0, (int)(x.m >> 63), s, 0);                      /* normal 0100, unnormal 0000 */
}

/* --- 128-bit software float for the transcendental functions ------------------------ */

/* (-1)^s * m/2^127 * 2^e, m normalized (or zero). About 126 good bits. */
typedef struct wf { int s; int32_t e; u128 m; } wf;

static wf wf_make(int s, int32_t e, u128 m) {
    wf r;
    r.s = s;
    if (is0(m)) { r.e = 0; r.m = m; return r; }
    const unsigned z = clz128(m);
    r.m = shl128(m, z);
    r.e = e - (int32_t)z;
    return r;
}
static int wf_zero(wf a) { return is0(a.m); }
static wf wf_from(mx_f80 x) {
    const num n = unpack(x);
    return wf_make(n.sign, n.e - BIAS, n.m);
}
static wf wf_int(int64_t v) { return wf_make(v < 0, 127, mk(0, v < 0 ? 0u - (uint64_t)v : (uint64_t)v)); }
static wf wf_neg(wf a) { a.s ^= 1; return a; }
static wf wf_ldexp(wf a, int32_t n) { if (!wf_zero(a)) a.e += n; return a; }
static wf wf_add(wf a, wf b) {
    if (wf_zero(a)) return b;
    if (wf_zero(b)) return a;
    if (b.e > a.e || (b.e == a.e && cmp128(b.m, a.m) > 0)) { const wf t = a; a = b; b = t; }
    const u128 ma = shr128(a.m, 1), mb = shr_jam(shr128(b.m, 1), (uint32_t)(a.e - b.e));
    return wf_make(a.s, a.e + 1, a.s == b.s ? add128(ma, mb) : sub128(ma, mb));
}
static wf wf_sub(wf a, wf b) { return wf_add(a, wf_neg(b)); }
static wf wf_mul(wf a, wf b) {
    if (wf_zero(a) || wf_zero(b)) return wf_make(0, 0, mk(0, 0));
    const u128 p0 = mul64(a.m.lo, b.m.lo), p1 = mul64(a.m.lo, b.m.hi), p2 = mul64(a.m.hi, b.m.lo), p3 = mul64(a.m.hi, b.m.hi);
    const u128 mid = add128(p1, p2);
    const uint64_t c1 = cmp128(mid, p1) < 0;
    const uint64_t t = p0.hi + mid.lo, c2 = t < p0.hi;
    u128 top = add128(add128(p3, mk(c1, mid.hi)), mk(0, c2));
    if (t | p0.lo) top.lo |= 1u;
    return wf_make(a.s ^ b.s, a.e + b.e + 1, top);
}
static wf wf_div(wf a, wf b) {
    if (wf_zero(a) || wf_zero(b)) return wf_make(0, 0, mk(0, 0));
    u128 rem = a.m, q = mk(0, 0);
    int carry = 0;
    for (int i = 127; i >= 0; --i) {
        if (carry || cmp128(rem, b.m) >= 0) {
            rem = sub128(rem, b.m);
            if (i >= 64) q.hi |= 1ull << (i - 64);
            else q.lo |= 1ull << i;
        }
        carry = (int)(rem.hi >> 63);
        rem = shl128(rem, 1);
    }
    if (!is0(rem) || carry) q.lo |= 1u;
    return wf_make(a.s ^ b.s, a.e - b.e, q);
}
static int wf_small(wf term, wf sum) { return wf_zero(term) || term.e < sum.e - 132; }

static const wf WF_PI = {0, 1, {0xC90FDAA22168C234u, 0xC4C6628B80DC1CD1u}};
static const wf WF_LN2 = {0, -1, {0xB17217F7D1CF79ABu, 0xC9E3B39803F2F6AFu}};
#define SQRT2_HI 0xB504F333F9DE6484u

/* e^t - 1 for |t| below 1: the Taylor series. */
static wf wf_expm1(wf t) {
    wf sum = t, term = t;
    for (int64_t n = 2; n < 200; ++n) {
        term = wf_div(wf_mul(term, t), wf_int(n));
        if (wf_small(term, sum)) break;
        sum = wf_add(sum, term);
    }
    return sum;
}

/* atanh(s) = s + s^3/3 + s^5/5 + ..., |s| below 1/2. */
static wf wf_atanh(wf s) {
    const wf s2 = wf_mul(s, s);
    wf sum = s, pw = s;
    for (int64_t k = 3; k < 400; k += 2) {
        pw = wf_mul(pw, s2);
        const wf term = wf_div(pw, wf_int(k));
        if (wf_small(term, sum)) break;
        sum = wf_add(sum, term);
    }
    return sum;
}

/* atan(z) for z >= 0: 1/z above 1, (z-1)/(z+1) from 1/2, then the series. */
static wf wf_atan(wf z) {
    const wf one = wf_int(1);
    int inv = 0, quarter = 0;
    if (z.e > 0 || (z.e == 0 && cmp128(z.m, one.m) > 0)) { z = wf_div(one, z); inv = 1; }
    if (z.e >= -1) { z = wf_div(wf_sub(z, one), wf_add(z, one)); quarter = 1; }
    const wf z2 = wf_mul(z, z);
    wf sum = z, pw = z;
    for (int64_t k = 3; k < 400; k += 2) {
        pw = wf_neg(wf_mul(pw, z2));
        const wf term = wf_div(pw, wf_int(k));
        if (wf_small(term, sum)) break;
        sum = wf_add(sum, term);
    }
    if (quarter) sum = wf_add(wf_ldexp(WF_PI, -2), sum);
    if (inv) sum = wf_sub(wf_ldexp(WF_PI, -1), sum);
    return sum;
}

/* log2(x) for x > 0: x = m * 2^E with m in [1/sqrt 2, sqrt 2), ln m = 2 atanh((m-1)/(m+1)). */
static wf wf_log2(wf x) {
    int32_t E = x.e;
    wf m = x;
    m.e = 0;
    if (m.m.hi > SQRT2_HI) { m.e = -1; E++; }
    const wf one = wf_int(1);
    const wf l = wf_div(wf_ldexp(wf_atanh(wf_div(wf_sub(m, one), wf_add(m, one))), 1), WF_LN2);
    return wf_add(l, wf_int(E));
}

/* log2(1 + x) for x > -1: 2 atanh(x/(2+x)) / ln 2 near zero. */
static wf wf_log2p1(wf x) {
    if (x.e >= -1) return wf_log2(wf_add(wf_int(1), x));
    return wf_div(wf_ldexp(wf_atanh(wf_div(x, wf_add(wf_int(2), x))), 1), WF_LN2);
}

/* sin and cos of |r| <= pi/4 by their series. */
static void wf_sincos(wf r, wf *s, wf *c) {
    const wf r2 = wf_mul(r, r);
    wf ss = r, term = r;
    for (int64_t n = 2; n < 200; n += 2) {
        term = wf_neg(wf_div(wf_mul(term, r2), wf_int(n * (n + 1))));
        if (wf_small(term, ss)) break;
        ss = wf_add(ss, term);
    }
    wf cc = wf_int(1);
    term = cc;
    for (int64_t n = 1; n < 200; n += 2) {
        term = wf_neg(wf_div(wf_mul(term, r2), wf_int(n * (n + 1))));
        if (wf_small(term, cc)) break;
        cc = wf_add(cc, term);
    }
    *s = ss;
    *c = cc;
}

/* A transcendental result into a register: 64 bits, the control word's rounding. */
static mx_f80 wf_result(mx_fpu8087 *f, wf w) {
    mx_f80 r;
    if (wf_zero(w)) return zero(w.s);
    deliver(f, w.s, w.e + BIAS, w.m, 64, 0, &r);
    return r;
}

/* --- the instructions ---------------------------------------------------------------- */

/* Stack checks: an empty source (underflow) or a full destination for a
 * push (overflow) is invalid; masked, the result is the indefinite. */
static int need(mx_fpu8087 *f, unsigned n) {
    for (unsigned i = 0; i < n; ++i) if (empty_at(f, i)) return 0;
    return 1;
}
static int room(const mx_fpu8087 *f) { return empty_at(f, 7); }

/* The operation of reg on a = ST(0) and b; the result into ST(dest), then
 * `pops` pops. reg: 0 add, 1 mul, 2 compare, 3 compare and pop, 4 a-b,
 * 5 b-a, 6 a/b, 7 b/a. */
static void binary(mx_fpu8087 *f, unsigned reg, mx_f80 b, int b_empty, unsigned dest, unsigned pops) {
    const mx_f80 a = get(f, 0);
    mx_f80 r = a;
    int ok = 1;
    if (empty_at(f, 0) || b_empty) {
        if (raise(f, MX_FPU_IE)) return;
        if (reg == 2 || reg == 3) set_cc(f, 1, 1, 0, 1);
        else put(f, dest, INDEFINITE);
    } else {
        switch (reg) {
        case 0: ok = op_add(f, a, b, 0, &r); break;
        case 1: ok = op_mul(f, a, b, &r); break;
        case 2: case 3: ok = op_compare(f, a, b); break;
        case 4: ok = op_add(f, a, b, 1, &r); break;
        case 5: ok = op_add(f, b, a, 1, &r); break;
        case 6: ok = op_div(f, a, b, &r); break;
        default: ok = op_div(f, b, a, &r); break;
        }
        if (!ok) return;
        if (reg != 2 && reg != 3) put(f, dest, r);
    }
    while (pops--) pop(f);
}

/* Loads a value onto the stack (src_ok 0: the source was empty). */
static void load(mx_fpu8087 *f, mx_f80 x, int src_ok) {
    if (!room(f) || !src_ok) {
        if (raise(f, MX_FPU_IE)) return;
        x = INDEFINITE;
    }
    push(f, x);
}

/* FLD of a short or long real: a NaN is invalid, a denormal raises D. */
static void load_real(mx_fpu8087 *f, uint64_t v, int dbl) {
    int cls;
    const mx_f80 x = from_real(v, dbl, &cls);
    if (!room(f)) { load(f, x, 1); return; }
    if (cls == C_NAN && raise(f, MX_FPU_IE)) return;
    if (cls == C_DENORMAL && raise(f, MX_FPU_DE)) return;
    push(f, x);
}

static void store_st(mx_fpu8087 *f, unsigned i, int popit) {
    if (empty_at(f, 0)) {
        if (raise(f, MX_FPU_IE)) return;
        put(f, i, INDEFINITE);
    } else put(f, i, get(f, 0));
    if (popit) pop(f);
}

/* FST/FIST/FBSTP to memory: kind 0 short real, 1 long real, 2-4 word,
 * short, long integer, 5 BCD, 6 temporary real. */
static void store_mem(mx_fpu8087 *f, uint32_t ea, int kind, int popit) {
    static const unsigned SIZES[7] = {4, 8, 2, 4, 8, 10, 10};
    uint8_t b[10];
    uint64_t v = 0;
    int ok = 1;
    if (empty_at(f, 0)) {
        if (raise(f, MX_FPU_IE)) return;
        switch (kind) {
        case 0: v = 0xFFC00000u; break;
        case 1: v = 0xFFF8000000000000u; break;
        case 2: v = 0x8000u; break;
        case 3: v = 0x80000000u; break;
        case 4: v = 1ull << 63; break;
        case 5: memset(b, 0, 10); b[7] = 0xC0; b[8] = b[9] = 0xFF; break;
        default: wrn(f, ea, INDEFINITE.m, 8); v = INDEFINITE.se; ea += 8; break;
        }
    } else {
        const mx_f80 x = get(f, 0);
        switch (kind) {
        case 0: ok = to_real(f, x, 0, &v); break;
        case 1: ok = to_real(f, x, 1, &v); break;
        case 2: ok = to_int(f, x, 16, &v); break;
        case 3: ok = to_int(f, x, 32, &v); break;
        case 4: ok = to_int(f, x, 64, &v); break;
        case 5: ok = to_bcd(f, x, b); break;
        default: wr80(f, ea, x); break;
        }
        if (!ok) return;
    }
    if (kind == 5) for (unsigned i = 0; i < 10; ++i) wr8(f, ea + i, b[i]);
    else if (kind != 6) wrn(f, ea, v, SIZES[kind]);
    else if (empty_at(f, 0)) wrn(f, ea, v, 2);
    if (popit) pop(f);
}

static void op_prem(mx_fpu8087 *f) {
    const mx_f80 a = get(f, 0), b = get(f, 1);
    mx_f80 r;
    if (!need(f, 2)) {
        if (invalid(f, &r)) put(f, 0, r);
        return;
    }
    const int ca = classify(a), cb = classify(b);
    if (ca == C_NAN || cb == C_NAN) {
        if (!raise(f, MX_FPU_IE)) put(f, 0, nan_of(a, b));
        return;
    }
    if (ca == C_INF || cb == C_ZERO || cb == C_UNNORMAL) {
        if (invalid(f, &r)) put(f, 0, r);
        return;
    }
    if ((denormal(ca) || denormal(cb)) && raise(f, MX_FPU_DE)) return;
    if (cb == C_INF || ca == C_ZERO) { set_cc(f, 0, 0, 0, 0); return; }
    num A = unpack(a), B = unpack(b);
    const unsigned za = clz64(A.m.hi), zb = clz64(B.m.hi);
    A.m.hi <<= za;
    A.e -= (int32_t)za;
    B.m.hi <<= zb;
    B.e -= (int32_t)zb;
    const int32_t d = A.e - B.e;
    if (d < 0) { set_cc(f, 0, 0, 0, 0); return; }
    const int partial = d >= 64;
    const int32_t steps = partial ? 63 : d;
    u128 rem = mk(0, A.m.hi);
    const u128 dv = mk(0, B.m.hi);
    uint64_t q = 0;
    for (int32_t i = 0; i <= steps; ++i) {
        q <<= 1;
        if (cmp128(rem, dv) >= 0) { rem = sub128(rem, dv); q |= 1u; }
        if (i < steps) rem = shl128(rem, 1);
    }
    /* rem * 2^(B.e - BIAS - 63 + (partial ? d - 63 : 0)) */
    const int32_t e = B.e + 64 + (partial ? d - 63 : 0);
    if (is0(rem)) r = zero(A.sign);
    else deliver(f, A.sign, e, rem, 64, 0, &r);
    put(f, 0, r);
    if (partial) set_cc(f, 0, 1, 0, 0);
    else set_cc(f, (int)((q >> 1) & 1u), 0, (int)(q & 1u), (int)((q >> 2) & 1u));
}

static void op_rndint(mx_fpu8087 *f) {
    const mx_f80 a = get(f, 0);
    mx_f80 r;
    if (!need(f, 1)) { if (invalid(f, &r)) put(f, 0, r); return; }
    const int c = classify(a);
    if (c == C_NAN) { if (!raise(f, MX_FPU_IE)) put(f, 0, a); return; }
    if (c == C_INF || c == C_ZERO) return;
    if (denormal(c) && raise(f, MX_FPU_DE)) return;
    if ((int32_t)expo(a) - BIAS >= 63) return;
    uint64_t mag;
    int inexact;
    to_integer(a, rc_of(f), &mag, &inexact);
    if (inexact) f->sw |= MX_FPU_PE;
    put(f, 0, from_u64(sgn(a), mag));
}

static void op_scale(mx_fpu8087 *f) {
    const mx_f80 a = get(f, 0), b = get(f, 1);
    mx_f80 r;
    if (!need(f, 2)) { if (invalid(f, &r)) put(f, 0, r); return; }
    const int ca = classify(a), cb = classify(b);
    if (ca == C_NAN || cb == C_NAN) { if (!raise(f, MX_FPU_IE)) put(f, 0, nan_of(a, b)); return; }
    if (cb == C_INF) {
        if ((ca == C_ZERO && !sgn(b)) || (ca == C_INF && sgn(b))) { if (invalid(f, &r)) put(f, 0, r); return; }
        if (ca != C_ZERO && ca != C_INF) put(f, 0, sgn(b) ? zero(sgn(a)) : inf(sgn(a)));
        return;
    }
    if ((denormal(ca) || denormal(cb)) && raise(f, MX_FPU_DE)) return;
    if (ca == C_ZERO || ca == C_INF) return;
    uint64_t mag;
    int inexact;
    if (!to_integer(b, RC_CHOP, &mag, &inexact) || mag > 0x100000u) mag = 0x100000u;
    const num A = unpack(a);
    deliver(f, A.sign, A.e + (sgn(b) ? -(int32_t)mag : (int32_t)mag), A.m, 64, ca == C_UNNORMAL, &r);
    put(f, 0, r);
}

static void op_xtract(mx_fpu8087 *f) {
    const mx_f80 a = get(f, 0);
    if (!need(f, 1) || !room(f)) {
        if (raise(f, MX_FPU_IE)) return;
        put(f, 0, INDEFINITE);
        push(f, INDEFINITE);
        return;
    }
    const int c = classify(a);
    if (c == C_NAN) { if (!raise(f, MX_FPU_IE)) push(f, a); return; }
    if (denormal(c) && raise(f, MX_FPU_DE)) return;
    if (c == C_INF) { put(f, 0, inf(0)); push(f, a); return; }
    if (c == C_ZERO) { put(f, 0, zero(0)); push(f, zero(sgn(a))); return; }
    const int32_t e = (expo(a) ? (int32_t)expo(a) : 1) - BIAS;
    put(f, 0, from_u64(e < 0, (uint64_t)(e < 0 ? -e : e)));
    push(f, f80(sgn(a), BIAS, a.m));
}

/* F2XM1: 2^x - 1, the 8087's range 0 <= x <= 0.5 (computed beyond it too). */
static void op_f2xm1(mx_fpu8087 *f) {
    const mx_f80 a = get(f, 0);
    mx_f80 r;
    if (!need(f, 1)) { if (invalid(f, &r)) put(f, 0, r); return; }
    const int c = classify(a);
    if (c == C_NAN) { if (!raise(f, MX_FPU_IE)) put(f, 0, a); return; }
    if (c == C_UNNORMAL) { if (invalid(f, &r)) put(f, 0, r); return; }
    if (denormal(c) && raise(f, MX_FPU_DE)) return;
    if (c == C_ZERO) return;
    if (c == C_INF) { put(f, 0, sgn(a) ? f80(1, BIAS, 1ull << 63) : a); return; }
    const wf x = wf_from(a);
    wf y;
    if (x.e < 0) y = wf_expm1(wf_mul(x, WF_LN2));
    else {
        uint64_t mag;
        int inexact;
        if (!to_integer(a, RC_CHOP, &mag, &inexact) || mag > 0x10000u) mag = 0x10000u;
        const int32_t n = sgn(a) ? -(int32_t)mag : (int32_t)mag;
        const wf frac = wf_sub(x, wf_int(n));
        y = wf_sub(wf_ldexp(wf_add(wf_expm1(wf_mul(frac, WF_LN2)), wf_int(1)), n), wf_int(1));
    }
    put(f, 0, wf_result(f, y));
}

/* FYL2X (p1 0): ST(1) * log2 ST(0); FYL2XP1: ST(1) * log2(ST(0) + 1); the
 * result replaces ST(1), ST(0) is popped. */
static void op_yl2x(mx_fpu8087 *f, int p1) {
    const mx_f80 x = get(f, 0), y = get(f, 1);
    mx_f80 r;
    if (!need(f, 2)) { if (invalid(f, &r)) { put(f, 1, r); pop(f); } return; }
    const int cx = classify(x), cy = classify(y);
    if (cx == C_NAN || cy == C_NAN) { if (!raise(f, MX_FPU_IE)) { put(f, 1, nan_of(x, y)); pop(f); } return; }
    if (cx == C_UNNORMAL || cy == C_UNNORMAL) { if (invalid(f, &r)) { put(f, 1, r); pop(f); } return; }
    /* the argument of the logarithm against 0 (or -1) and 1 (or 0) */
    const int one_point = p1 ? cx == C_ZERO : (cx == C_NORMAL && x.m == (1ull << 63) && x.se == BIAS);
    const int neg = p1 ? (sgn(x) && cx != C_ZERO && (cx == C_INF || cmp_mag(x, f80(0, BIAS, 1ull << 63)) >= 0)) : (sgn(x) && cx != C_ZERO);
    const int at_zero = p1 ? (sgn(x) && cx != C_INF && cx != C_ZERO && cmp_mag(x, f80(0, BIAS, 1ull << 63)) == 0) : cx == C_ZERO;
    int below_one = 0;                              /* log negative */
    if (neg && !at_zero) { if (invalid(f, &r)) { put(f, 1, r); pop(f); } return; }
    if ((denormal(cx) || denormal(cy)) && raise(f, MX_FPU_DE)) return;
    if (at_zero) {
        if (cy == C_ZERO) { if (invalid(f, &r)) { put(f, 1, r); pop(f); } return; }
        if (cy != C_INF && raise(f, MX_FPU_ZE)) return;
        put(f, 1, inf(!sgn(y)));
        pop(f);
        return;
    }
    if (cx == C_INF) {
        if (cy == C_ZERO) { if (invalid(f, &r)) { put(f, 1, r); pop(f); } return; }
        put(f, 1, inf(sgn(y)));
        pop(f);
        return;
    }
    const wf lx = p1 ? wf_log2p1(wf_from(x)) : wf_log2(wf_from(x));
    below_one = lx.s && !wf_zero(lx);
    if (one_point || wf_zero(lx)) {
        if (cy == C_INF) { if (invalid(f, &r)) { put(f, 1, r); pop(f); } return; }
        put(f, 1, zero(sgn(y) ^ (p1 ? sgn(x) : 0)));
        pop(f);
        return;
    }
    if (cy == C_INF) { put(f, 1, inf(sgn(y) ^ below_one)); pop(f); return; }
    if (cy == C_ZERO) { put(f, 1, zero(sgn(y) ^ below_one)); pop(f); return; }
    put(f, 1, wf_result(f, wf_mul(wf_from(y), lx)));
    pop(f);
}

/* FPATAN: arctan(ST(1)/ST(0)) into ST(1), pop. The 8087's range is
 * 0 <= ST(1) < ST(0) < infinity; other quadrants as the full arctangent. */
static void op_patan(mx_fpu8087 *f) {
    const mx_f80 x = get(f, 0), y = get(f, 1);
    mx_f80 r;
    if (!need(f, 2)) { if (invalid(f, &r)) { put(f, 1, r); pop(f); } return; }
    const int cx = classify(x), cy = classify(y);
    if (cx == C_NAN || cy == C_NAN) { if (!raise(f, MX_FPU_IE)) { put(f, 1, nan_of(x, y)); pop(f); } return; }
    if (cx == C_UNNORMAL || cy == C_UNNORMAL) { if (invalid(f, &r)) { put(f, 1, r); pop(f); } return; }
    if ((denormal(cx) || denormal(cy)) && raise(f, MX_FPU_DE)) return;
    wf a;
    if (cy == C_ZERO) a = sgn(x) ? WF_PI : wf_make(0, 0, mk(0, 0));
    else if (cx == C_ZERO) a = wf_ldexp(WF_PI, -1);
    else if (cy == C_INF) a = cx == C_INF ? (sgn(x) ? wf_mul(WF_PI, wf_div(wf_int(3), wf_int(4))) : wf_ldexp(WF_PI, -2)) : wf_ldexp(WF_PI, -1);
    else if (cx == C_INF) a = sgn(x) ? WF_PI : wf_make(0, 0, mk(0, 0));
    else {
        wf ay = wf_from(y), ax = wf_from(x);
        ay.s = ax.s = 0;
        a = wf_atan(wf_div(ay, ax));
        if (sgn(x)) a = wf_sub(WF_PI, a);
    }
    a.s = sgn(y);
    put(f, 1, wf_result(f, a));
    pop(f);
}

/* FPTAN: Y = tan ST(0) replaces ST(0), X = 1 is pushed. The 8087's range is
 * 0 <= ST(0) <= pi/4; beyond it the argument is reduced by pi/2 (|x| below
 * 2^62, else invalid). */
static void op_ptan(mx_fpu8087 *f) {
    const mx_f80 a = get(f, 0);
    if (!need(f, 1) || !room(f)) {
        if (raise(f, MX_FPU_IE)) return;
        put(f, 0, INDEFINITE);
        push(f, INDEFINITE);
        return;
    }
    const int c = classify(a);
    if (c == C_NAN) { if (!raise(f, MX_FPU_IE)) push(f, a); return; }
    if (c == C_INF || c == C_UNNORMAL || (c != C_ZERO && (int32_t)expo(a) - BIAS >= 62)) {
        if (raise(f, MX_FPU_IE)) return;
        put(f, 0, INDEFINITE);
        push(f, INDEFINITE);
        return;
    }
    if (denormal(c) && raise(f, MX_FPU_DE)) return;
    const mx_f80 one = f80(0, BIAS, 1ull << 63);
    if (c == C_ZERO) { push(f, one); return; }
    wf x = wf_from(a);
    int odd = 0;
    const wf quarter = wf_ldexp(WF_PI, -2);
    wf ax = x;
    ax.s = 0;
    if (wf_sub(ax, quarter).s == 0 && !wf_zero(wf_sub(ax, quarter))) {
        /* k = nearest integer of x / (pi/2) */
        const wf half_pi = wf_ldexp(WF_PI, -1);
        const wf k = wf_div(x, half_pi);
        int64_t n = 0;
        if (k.e >= 0) {
            const u128 ip = shr128(k.m, (uint32_t)(127 - k.e));
            n = (int64_t)ip.lo;
            const u128 frac = shl128(k.m, (uint32_t)(k.e + 1));
            if (frac.hi >> 63) n++;
        } else if (k.e == -1) n = 1;
        if (k.s) n = -n;
        x = wf_sub(x, wf_mul(wf_int(n), half_pi));
        odd = (int)(n & 1);
    }
    wf s, co;
    wf_sincos(x, &s, &co);
    put(f, 0, wf_result(f, odd ? wf_neg(wf_div(co, s)) : wf_div(s, co)));
    push(f, one);
}

/* --- control ------------------------------------------------------------------------- */

uint16_t mx_fpu_tag_word(const mx_fpu8087 *f) {
    uint16_t tw = 0;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned t;
        const mx_f80 x = f->reg[i];
        if ((f->empty >> i) & 1u) t = 3;
        else if (expo(x) == EXP_MAX || (expo(x) == 0 && x.m)) t = 2;
        else if (expo(x) == 0) t = 1;
        else t = 0;
        tw |= (uint16_t)(t << (2 * i));
    }
    return tw;
}

static uint16_t ir(const mx_fpu8087 *f) { return (f->sw & ~f->cw & 0x3Fu) ? MX_FPU_IR : 0u; }

uint16_t mx_fpu_status_word(const mx_fpu8087 *f, uint64_t clock) {
    return (uint16_t)((f->sw & 0x473Fu) | ir(f) | (unsigned)f->top << 11 | (clock < f->busy_until ? MX_FPU_B : 0u));
}

static void update_int(mx_fpu8087 *f) {
    const uint8_t level = (uint8_t)(ir(f) && !(f->cw & MX_FPU_IEM));
    if (level == f->int_line) return;
    f->int_line = level;
    if (f->host.interrupt) f->host.interrupt(f->host.ctx, level);
}

static void finit(mx_fpu8087 *f) {
    f->cw = 0x03FF;
    f->sw = 0;
    f->top = 0;
    f->empty = 0xFF;
    f->ip = f->dp = 0;
    f->opcode = 0;
}

static void store_env(mx_fpu8087 *f, uint32_t ea) {
    const uint16_t w[7] = {
        f->cw, mx_fpu_status_word(f, f->now), mx_fpu_tag_word(f), (uint16_t)f->ip,
        (uint16_t)(((f->ip >> 16) & 0xFu) << 12 | (f->opcode & 0x7FFu)), (uint16_t)f->dp, (uint16_t)(((f->dp >> 16) & 0xFu) << 12),
    };
    for (unsigned i = 0; i < 7; ++i) wrn(f, ea + 2 * i, w[i], 2);
}

static void load_env(mx_fpu8087 *f, uint32_t ea) {
    uint16_t w[7];
    for (unsigned i = 0; i < 7; ++i) w[i] = (uint16_t)rdn(f, ea + 2 * i, 2);
    f->cw = w[0];
    f->sw = w[1] & 0x473Fu;
    f->top = (uint8_t)((w[1] >> 11) & 7u);
    f->empty = 0;
    for (unsigned i = 0; i < 8; ++i) if (((w[2] >> (2 * i)) & 3u) == 3u) f->empty |= (uint8_t)(1u << i);
    f->ip = w[3] | (uint32_t)(w[4] >> 12) << 16;
    f->opcode = w[4] & 0x7FFu;
    f->dp = w[5] | (uint32_t)(w[6] >> 12) << 16;
}

/* --- decoding ------------------------------------------------------------------------ */

/* Typical clocks of the 8087 data sheet's execution table (memory forms with
 * their transfers, as round figures). */
enum {
    T_ADD = 85, T_MUL = 97, T_DIV = 198, T_COM = 45, T_SQRT = 183, T_PREM = 125, T_RND = 45, T_SCALE = 35, T_XTRACT = 50,
    T_2XM1 = 500, T_YL2X = 950, T_YL2XP1 = 850, T_PTAN = 450, T_PATAN = 650
};

/* Memory forms. *control: a control instruction (the pointers stay). */
static unsigned mem_op(mx_fpu8087 *f, unsigned esc, unsigned reg, uint32_t ea, int *control) {
    *control = 0;
    if (!(esc & 1u)) {
        /* D8 short real, DA short integer, DC long real, DE word integer */
        static const unsigned EXTRA[4] = {20, 40, 25, 35};
        mx_f80 b;
        int cls = C_NORMAL;
        switch (esc) {
        case 0: b = from_real(rdn(f, ea, 4), 0, &cls); break;
        case 2: { const uint32_t v = (uint32_t)rdn(f, ea, 4); b = from_u64(v >> 31, v >> 31 ? 0u - (uint64_t)(int64_t)(int32_t)v : v); break; }
        case 4: b = from_real(rdn(f, ea, 8), 1, &cls); break;
        default: { const uint16_t v = (uint16_t)rdn(f, ea, 2); b = from_u64(v >> 15, v >> 15 ? (uint64_t)(0x10000u - v) : v); break; }
        }
        if (cls == C_DENORMAL && !empty_at(f, 0) && raise(f, MX_FPU_DE)) return T_ADD;
        binary(f, reg, b, 0, 0, reg == 3);
        static const unsigned BASE[8] = {T_ADD, T_MUL, T_COM, T_COM, T_ADD, T_ADD, T_DIV, T_DIV};
        return BASE[reg] + EXTRA[esc >> 1];
    }
    switch (esc << 3 | reg) {
    case 1 << 3 | 0: load_real(f, rdn(f, ea, 4), 0); return 43;
    case 1 << 3 | 2: store_mem(f, ea, 0, 0); return 87;
    case 1 << 3 | 3: store_mem(f, ea, 0, 1); return 89;
    case 1 << 3 | 4: *control = 1; load_env(f, ea); return 45;
    case 1 << 3 | 5: *control = 1; f->cw = (uint16_t)rdn(f, ea, 2); return 10;
    case 1 << 3 | 6: *control = 1; store_env(f, ea); return 45;
    case 1 << 3 | 7: *control = 1; wrn(f, ea, f->cw, 2); return 15;
    case 3 << 3 | 0: { const uint32_t v = (uint32_t)rdn(f, ea, 4); load(f, from_u64(v >> 31, v >> 31 ? 0u - (uint64_t)(int64_t)(int32_t)v : v), 1); return 57; }
    case 3 << 3 | 2: store_mem(f, ea, 3, 0); return 88;
    case 3 << 3 | 3: store_mem(f, ea, 3, 1); return 90;
    case 3 << 3 | 5: load(f, rd80(f, ea), 1); return 57;
    case 3 << 3 | 7: store_mem(f, ea, 6, 1); return 55;
    case 5 << 3 | 0: load_real(f, rdn(f, ea, 8), 1); return 46;
    case 5 << 3 | 2: store_mem(f, ea, 1, 0); return 100;
    case 5 << 3 | 3: store_mem(f, ea, 1, 1); return 102;
    case 5 << 3 | 4:
        *control = 1;
        load_env(f, ea);
        for (unsigned i = 0; i < 8; ++i) f->reg[PHYS(f, i)] = rd80(f, ea + 14 + 10 * i);
        return 210;
    case 5 << 3 | 6:
        *control = 1;
        store_env(f, ea);
        for (unsigned i = 0; i < 8; ++i) wr80(f, ea + 14 + 10 * i, get(f, i));
        finit(f);
        return 210;
    case 5 << 3 | 7: *control = 1; wrn(f, ea, mx_fpu_status_word(f, f->now), 2); return 15;
    case 7 << 3 | 0: { const uint16_t v = (uint16_t)rdn(f, ea, 2); load(f, from_u64(v >> 15, v >> 15 ? (uint64_t)(0x10000u - v) : v), 1); return 51; }
    case 7 << 3 | 2: store_mem(f, ea, 2, 0); return 86;
    case 7 << 3 | 3: store_mem(f, ea, 2, 1); return 88;
    case 7 << 3 | 4: {
        uint8_t b[10];
        for (unsigned i = 0; i < 10; ++i) b[i] = rd8(f, ea + i);
        load(f, from_bcd(b), 1);
        return 300;
    }
    case 7 << 3 | 5: { const uint64_t v = rdn(f, ea, 8); load(f, from_u64((int)(v >> 63), v >> 63 ? 0u - v : v), 1); return 64; }
    case 7 << 3 | 6: store_mem(f, ea, 5, 1); return 530;
    case 7 << 3 | 7: store_mem(f, ea, 4, 1); return 96;
    default: *control = 1; return 0;            /* reserved: nothing */
    }
}

static unsigned reg_op(mx_fpu8087 *f, unsigned esc, unsigned reg, unsigned i, int *control) {
    static const mx_f80 CONSTANTS[7] = {
        {0x8000000000000000u, 0x3FFF}, {0xD49A784BCD1B8AFEu, 0x4000}, {0xB8AA3B295C17F0BCu, 0x3FFF}, {0xC90FDAA22168C235u, 0x4000},
        {0x9A209A84FBCFF799u, 0x3FFD}, {0xB17217F7D1CF79ACu, 0x3FFE}, {0, 0},
    };
    static const unsigned BASE[8] = {T_ADD, T_MUL, T_COM, T_COM, T_ADD, T_ADD, T_DIV, T_DIV};
    *control = 0;
    switch (esc) {
    case 0: binary(f, reg, get(f, i), empty_at(f, i), 0, reg == 3); return BASE[reg];
    case 4: if (reg == 2 || reg == 3) break; binary(f, reg, get(f, i), empty_at(f, i), i, 0); return BASE[reg];
    case 6:
        if (reg == 3) { if (i != 1) break; binary(f, 3, get(f, 1), empty_at(f, 1), 0, 2); return T_COM; }
        if (reg == 2) break;
        binary(f, reg, get(f, i), empty_at(f, i), i, 1);
        return BASE[reg];
    case 1:
        switch (reg) {
        case 0: load(f, get(f, i), !empty_at(f, i)); return 20;
        case 1: {
            mx_f80 a = get(f, 0), b = get(f, i);
            if (empty_at(f, 0) || empty_at(f, i)) {
                if (raise(f, MX_FPU_IE)) return 12;
                if (empty_at(f, 0)) a = INDEFINITE;
                if (empty_at(f, i)) b = INDEFINITE;
            }
            put(f, 0, b);
            put(f, i, a);
            return 12;
        }
        case 2: if (i == 0) return 13; break;                       /* FNOP */
        case 4: {
            mx_f80 a = get(f, 0), r;
            if (i == 5) { op_xam(f); return 17; }
            if (i == 0 || i == 1) {
                if (empty_at(f, 0)) { if (invalid(f, &r)) put(f, 0, r); return 15; }
                a.se = (uint16_t)(i == 0 ? a.se ^ 0x8000u : a.se & 0x7FFFu);
                put(f, 0, a);
                return i ? 14 : 15;
            }
            if (i == 4) {
                if (empty_at(f, 0)) { if (!raise(f, MX_FPU_IE)) set_cc(f, 1, 1, 0, 1); return 42; }
                op_compare(f, a, zero(0));
                return 42;
            }
            break;
        }
        case 5: if (i < 7) { load(f, CONSTANTS[i], 1); return i == 6 ? 14 : i == 0 ? 18 : 20; } break;
        case 6:
            switch (i) {
            case 0: op_f2xm1(f); return T_2XM1;
            case 1: op_yl2x(f, 0); return T_YL2X;
            case 2: op_ptan(f); return T_PTAN;
            case 3: op_patan(f); return T_PATAN;
            case 4: op_xtract(f); return T_XTRACT;
            case 6: f->top = (uint8_t)((f->top + 7u) & 7u); return 9;
            case 7: f->top = (uint8_t)((f->top + 1u) & 7u); return 9;
            default: break;
            }
            break;
        case 7:
            switch (i) {
            case 0: op_prem(f); return T_PREM;
            case 1: op_yl2x(f, 1); return T_YL2XP1;
            case 2: {
                mx_f80 r;
                if (!need(f, 1)) { if (invalid(f, &r)) put(f, 0, r); }
                else if (op_sqrt(f, get(f, 0), &r)) put(f, 0, r);
                return T_SQRT;
            }
            case 4: op_rndint(f); return T_RND;
            case 5: op_scale(f); return T_SCALE;
            default: break;
            }
            break;
        default: break;
        }
        break;
    case 3:
        if (reg != 4) break;
        *control = 1;
        switch (i) {
        case 0: f->cw = (uint16_t)(f->cw & ~(unsigned)MX_FPU_IEM); return 5;          /* FENI */
        case 1: f->cw |= MX_FPU_IEM; return 5;                      /* FDISI */
        case 2: f->sw = (uint16_t)(f->sw & ~0x3Fu); return 5;                /* FCLEX */
        case 3: finit(f); return 5;                                 /* FINIT */
        default: return 0;                                          /* FSETPM and the rest: nothing */
        }
    case 5:
        switch (reg) {
        case 0: f->empty |= (uint8_t)(1u << PHYS(f, i)); return 11;
        case 2: store_st(f, i, 0); return 18;
        case 3: store_st(f, i, 1); return 20;
        default: break;
        }
        break;
    default: break;
    }
    *control = 1;                               /* reserved: nothing */
    return 0;
}

unsigned mx_fpu_execute(mx_fpu8087 *f, uint8_t opcode, uint8_t modrm, uint32_t address, uint32_t insn_address) {
    const unsigned esc = opcode & 7u, mod = modrm >> 6, reg = (modrm >> 3) & 7u, rm = modrm & 7u;
    int control;
    const unsigned clocks = mod != 3 ? mem_op(f, esc, reg, address, &control) : reg_op(f, esc, reg, rm, &control);
    if (!control) {
        f->ip = insn_address & 0xFFFFFu;
        f->opcode = (uint16_t)(esc << 8 | modrm);
        if (mod != 3) f->dp = address & 0xFFFFFu;
    }
    update_int(f);
    return clocks;
}

/* The instructions the 8087 takes while busy (the no-wait control forms
 * that do not wait for it): FINIT, FCLEX, FENI, FDISI, FSTSW, FSTCW. */
static int immediate(uint8_t opcode, uint8_t modrm) {
    const unsigned esc = opcode & 7u, reg = (modrm >> 3) & 7u;
    if ((modrm >> 6) == 3) return esc == 3 && reg == 4;
    return (esc == 1 && reg == 7) || (esc == 5 && reg == 7);
}

void mx_fpu_esc(mx_fpu8087 *f, uint8_t opcode, uint8_t modrm, uint32_t address, uint32_t insn_address, uint64_t clock) {
    f->now = clock;
    const int now = immediate(opcode, modrm);
    const uint64_t start = now || clock >= f->busy_until ? clock : f->busy_until;
    const unsigned n = mx_fpu_execute(f, opcode, modrm, address, insn_address);
    if (!now) f->busy_until = start + n;
    else if ((modrm >> 6) == 3 && (modrm & 7u) == 3u) f->busy_until = clock + n;   /* FINIT ends what was running */
}

int mx_fpu_busy(const mx_fpu8087 *f, uint64_t clock) { return clock < f->busy_until; }

mx_f80 mx_fpu_st(const mx_fpu8087 *f, unsigned i) { return get(f, i & 7u); }
int mx_fpu_st_empty(const mx_fpu8087 *f, unsigned i) { return empty_at(f, i & 7u); }

void mx_fpu_reset(mx_fpu8087 *f) {
    memset(f->reg, 0, sizeof f->reg);
    finit(f);
    f->busy_until = 0;
    f->now = 0;
    update_int(f);
}

void mx_fpu_init(mx_fpu8087 *f, const mx_fpu_host *host) {
    memset(f, 0, sizeof *f);
    f->host = *host;
    mx_fpu_reset(f);
}
