/* The 8087 on a handful of known answers: addition, division and the pop forms,
 * the square root, a rounded quotient, a masked and an unmasked zero divide with
 * its interrupt request, and an integer store that overflows. MezinXT's own
 * code; behaviour from Intel's 8087 data sheet. */
#include "fpu8087.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long va_ = (a), vb_ = (b); if (va_ != vb_) { printf("FAIL %s:%d: %s = %llx, expected %llx\n", __FILE__, __LINE__, #a, va_, vb_); failures++; } } while (0)

static uint8_t mem[1u << 20];
static mx_fpu8087 fpu;
static int int_level;

static uint8_t h_read(void *ctx, uint32_t a) { (void)ctx; return mem[a & 0xFFFFFu]; }
static void h_write(void *ctx, uint32_t a, uint8_t v) { (void)ctx; mem[a & 0xFFFFFu] = v; }
static void h_int(void *ctx, int level) {
    (void)ctx;
    CHECK(level != int_level);
    int_level = level;
}

enum { M_OP = 0x1000 };

/* Runs ESC opcode/modrm, the operand at M_OP for a memory form. */
static unsigned run(uint8_t opcode, uint8_t modrm) { return mx_fpu_execute(&fpu, opcode, modrm, M_OP, 0x12345); }
static void put80(uint32_t a, mx_f80 x) {
    for (unsigned i = 0; i < 8; ++i) mem[a + i] = (uint8_t)(x.m >> (8 * i));
    mem[a + 8] = (uint8_t)x.se;
    mem[a + 9] = (uint8_t)(x.se >> 8);
}
static void put16(uint32_t a, uint16_t v) { mem[a] = (uint8_t)v; mem[a + 1] = (uint8_t)(v >> 8); }
static uint64_t get64(uint32_t a, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= (uint64_t)mem[a + i] << (8 * i);
    return v;
}
static void fld(mx_f80 x) { put80(M_OP, x); run(0xDB, 0x2E); }         /* FLD TBYTE [M_OP] (mod 00, rm 110) */
static void fldcw(uint16_t cw) { put16(M_OP, cw); run(0xD9, 0x2E); }
static uint16_t sw(void) { return mx_fpu_status_word(&fpu, 0); }
static int same(mx_f80 x, mx_f80 y) { return x.m == y.m && x.se == y.se; }

static void fresh(void) {
    const mx_fpu_host host = {NULL, h_read, h_write, h_int};
    int_level = 0;
    mx_fpu_init(&fpu, &host);
}

static const mx_f80 ONE = {0x8000000000000000u, 0x3FFF}, TWO = {0x8000000000000000u, 0x4000}, FOUR = {0x8000000000000000u, 0x4001};

int main(void) {
    /* 1 + 1 = 2; 1 / 2 = 0.5 with the pop; the square root of 4 */
    fresh();
    fld(ONE);
    fld(ONE);
    run(0xD8, 0xC1);                                       /* FADD ST(0),ST(1) */
    CHECK(same(mx_fpu_st(&fpu, 0), TWO));
    fresh();
    fld(ONE);
    fld(TWO);
    run(0xDE, 0xF9);                                       /* FDIVP ST(1),ST: ST(1)/ST */
    CHECK(mx_fpu_st(&fpu, 0).se == 0x3FFE && mx_fpu_st(&fpu, 0).m == 1ull << 63 && mx_fpu_st_empty(&fpu, 1));
    fresh();
    fld(FOUR);
    run(0xD9, 0xFA);                                       /* FSQRT */
    CHECK(same(mx_fpu_st(&fpu, 0), TWO));
    /* 1 / 3 rounded to the nearest at full precision */
    fresh();
    fld((mx_f80){0xC000000000000000u, 0x4000});
    fld(ONE);
    run(0xD8, 0xF1);                                       /* FDIV ST(0),ST(1) */
    CHECK(mx_fpu_st(&fpu, 0).se == 0x3FFD && mx_fpu_st(&fpu, 0).m == 0xAAAAAAAAAAAAAAABu);
    /* masked zero divide: infinity and the flag; unmasked: unchanged, the request and the INT pin */
    fresh();
    fld((mx_f80){0, 0});
    fld(ONE);
    run(0xD8, 0xF1);
    CHECK(mx_fpu_st(&fpu, 0).se == 0x7FFF && mx_fpu_st(&fpu, 0).m == 1ull << 63);
    CHECK_EQ(sw() & 0xBF, MX_FPU_ZE);
    CHECK_EQ(int_level, 0);
    fresh();
    fld((mx_f80){0, 0});
    fld(ONE);
    fldcw(0x0373);
    run(0xD8, 0xF1);
    CHECK(same(mx_fpu_st(&fpu, 0), ONE));
    CHECK_EQ(sw() & 0xBF, MX_FPU_ZE | MX_FPU_IR);
    CHECK_EQ(int_level, 1);
    /* FIST of 40000 to a word overflows: the integer indefinite */
    fresh();
    fld((mx_f80){0x9C40000000000000u, 0x400E});
    run(0xDF, 0x16);
    CHECK_EQ(get64(M_OP, 2), 0x8000);
    CHECK_EQ(sw() & 0x3F, MX_FPU_IE);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_fpu8087: all passed\n");
    return 0;
}
