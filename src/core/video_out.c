/* The CGA card's analog video outputs (video_out.h): the output stages of schematic sheet 10 solved from their
 * components, once for the RGB jacks J3-J5 and once for the composite jack J2. MezinXT's own.
 *
 * Both stages are the same circuit: LS244 outputs (U43, U44) drive resistors that meet at the base of a 2N2222
 * emitter follower; the emitter has 180 ohm to ground and 75 ohm in series with the jack, into the monitor's 75 ohm
 * input. Each LS244 output is either high (VOH) or low (VOL) and always connected, so the base is fed by a Thevenin
 * source (the conductance-weighted mean of the output levels, through the parallel resistors); the follower subtracts
 * the base-emitter drop and divides by the base current's share of its load; it conducts only above one drop.
 * Black is then 0 and the highest level 255. Resulting levels (rounded), for checking against the circuit:
 *   J3-J5, neither line / one / both:          0, 111, 255
 *   J2, by RGBI index 0-15:  0 7 153 160 71 79 225 232 23 30 176 184 95 102 248 255
 * The composite levels depend only on the resistors (every level is far above the cut-off, where the follower is
 * linear); the RGB middle level also on VOH, VOL and the base-emitter drop.
 *
 * The second part is the composite monitor's video bandwidth: the picture along a scanline through a Gaussian
 * low-pass filter (video_out.h). */
#include "video_out.h"

#include <math.h>
#include <string.h>

/* The circuit: every value the levels come from. Ohms and volts. */
static const struct {
    /* Assumptions (no value is printed on the schematic): typical 74LS244 output levels (data sheet, VOH at IOH
     * -3 mA, VOL at IOL 12 mA), the 2N2222's base-emitter drop at the 10-30 mA the stage draws, and its gain at
     * that current (a gain only scales the output and cancels in the normalisation). */
    double voh, vol, vbe, beta;
    /* The output stage common to the four followers: R6/R15/R17/R19 to ground, R7/R14/R16/R18 in series
     * (parts list: 180 and 75 ohm), and the 75 ohm monitor input (manual p. 71-74). */
    double r_emitter, r_series, r_monitor;
    /* J3-J5: R8-R13, a colour line (Rx, Gx, Bx from U43) and the intensity line (Iout, Ix, Iy from U44) each. */
    double r_rgb;
    /* J2: R1 (red, U43 pin 18), R2 (green, 16), R3 (blue, 14), R4 (intensity, 12) and R5 (composite sync, pin 9);
     * the lines come from U40 Q0-Q3 in that order. R1 is the sheet's "1K" or the parts list's "1K2". */
    double r_comp[4], r_sync;
} cga = {3.4, 0.35, 0.7, 100.0, 180.0, 75.0, 75.0, 470.0, {1200.0, 560.0, 12000.0, 3700.0}, 180.0};

/* The emitter voltage of a follower whose base is fed through resistors of total conductance `g` (siemens) with
 * `gv` the sum of conductance times the voltage each is driven to: its Thevenin voltage gv / g behind 1 / g. */
static double emitter(double g, double gv) {
    const double vth = gv / g;
    if (vth <= cga.vbe) return 0.0;                                 /* off: nothing reaches the jack */
    const double load = cga.r_emitter * (cga.r_series + cga.r_monitor) / (cga.r_emitter + cga.r_series + cga.r_monitor);
    const double gb = 1.0 / ((cga.beta + 1.0) * load);              /* the base seen from the Thevenin source */
    return (vth - cga.vbe) * g / (g + gb);
}

static unsigned to_level(double fraction) {
    if (fraction <= 0.0) return 0;
    if (fraction >= 1.0) return 255;
    return (unsigned)(fraction * 255.0 + 0.5);
}

unsigned mx_cga_rgb_level(unsigned lines_on) {
    const double g = 2.0 / cga.r_rgb;
    if (lines_on > 2) lines_on = 2;
    const double v = emitter(g, ((double)lines_on * cga.voh + (double)(2 - lines_on) * cga.vol) / cga.r_rgb);
    return to_level(v / emitter(g, 2.0 * cga.voh / cga.r_rgb));
}

uint32_t mx_cga_rgb_color(unsigned index) {
    const unsigned i = index & 15u, intensity = i >> 3 & 1u;
    return mx_cga_rgb_level((i >> 2 & 1u) + intensity) << 16 | mx_cga_rgb_level((i >> 1 & 1u) + intensity) << 8 |
           mx_cga_rgb_level((i & 1u) + intensity);
}

unsigned mx_cga_composite_level(unsigned index) {
    /* The picture is drawn with the composite sync high (inactive) through R5; U40's Q0-Q3 are red, green, blue and
     * intensity: RGBI bits 2, 1, 0 and 3. */
    const unsigned i = index & 15u, bits[4] = {i >> 2 & 1u, i >> 1 & 1u, i & 1u, i >> 3 & 1u};
    double g = 1.0 / cga.r_sync, gv_black = cga.voh / cga.r_sync, gv_white = gv_black, gv = gv_black;
    for (unsigned k = 0; k < 4; ++k) {
        const double gk = 1.0 / cga.r_comp[k];
        g += gk;
        gv += gk * (bits[k] ? cga.voh : cga.vol);
        gv_black += gk * cga.vol;
        gv_white += gk * cga.voh;
    }
    const double black = emitter(g, gv_black), white = emitter(g, gv_white);
    return to_level((emitter(g, gv) - black) / (white - black));
}

/* --- the composite monitor's bandwidth (video_out.h) ---------------------------------------------------------- */

#define PI 3.14159265358979323846

/* The weight of the dot `m` places away: the mean over a dot of the Gaussian's response (standard deviation `s`, in dots)
 * to a held dot. A held dot is a rectangle one dot wide and the displayed value a mean over one dot, so the weight is the
 * triangle (1 - |v|) on [-1, 1] convolved with the Gaussian, at m. The triangle is the second difference of the ramp
 * max(v, 0), and the ramp convolved with the Gaussian is R(x) = x Phi(x / s) + s phi(x / s) (Phi and phi the standard
 * normal's distribution and density): the weight is R(m + 1) - 2 R(m) + R(m - 1). */
static double ramp(double x, double s) {
    const double z = x / s;
    return x * 0.5 * erfc(-z / sqrt(2.0)) + s * exp(-0.5 * z * z) / sqrt(2.0 * PI);
}

static double weight(int m, double s) { return ramp(m + 1.0, s) - 2.0 * ramp((double)m, s) + ramp(m - 1.0, s); }

void mx_composite_filter_design(mx_composite_filter *f, double bandwidth_hz) {
    memset(f, 0, sizeof *f);
    /* s: the Gaussian's standard deviation in dots, from |H(B)| = 1/sqrt(2). */
    const double s = bandwidth_hz > 0.0 ? sqrt(log(2.0)) / (2.0 * PI * bandwidth_hz) * MX_CGA_DOT_HZ : 0.0;
    int32_t w[MX_COMPOSITE_RADIUS_MAX + 1] = {0};
    unsigned radius = 0;
    int32_t outer = 0;
    if (s > 1e-3) {
        /* The weights that round to at least one unit (2^-16) and no more than the maximum reach; the centre takes the
         * rest, so they add up to 65536 exactly (a flat field stays flat to the level). */
        for (int m = 1; m <= MX_COMPOSITE_RADIUS_MAX; ++m) {
            const int32_t t = (int32_t)(weight(m, s) * 65536.0 + 0.5);
            if (t < 1) break;
            w[m] = t;
            outer += t;
            radius = (unsigned)m;
        }
    }
    w[0] = 65536 - 2 * outer;
    f->radius = radius;
    for (int m = -(int)radius; m <= (int)radius; ++m) f->tap[(int)radius + m] = w[m < 0 ? -m : m];
    for (unsigned i = 0; i < 16; ++i) f->level[i] = (uint8_t)mx_cga_composite_level(i);
}

const mx_composite_filter *mx_composite_filter_default(void) {
    static mx_composite_filter filter;
    static int ready;
    if (!ready) {
        mx_composite_filter_design(&filter, MX_COMPOSITE_BANDWIDTH_HZ);
        ready = 1;
    }
    return &filter;
}

/* A line is done in blocks, each in plain loops over the block (the levels of its dots and the radius beyond each side,
 * then the weighted sums, then the pixels) that a compiler can vectorise; nothing is held between lines or blocks. */
#define BLOCK 1024

static void row_block(const mx_composite_filter *f, const uint8_t *indices, int n, int x0, int len, uint32_t *argb) {
    const int radius = (int)f->radius, taps = 2 * radius + 1;
    int16_t lv[BLOCK + 2 * MX_COMPOSITE_RADIUS_MAX];
    /* lv[j] is the dot x0 + j - radius, the end dot's level beyond the ends of the line. */
    const int first = radius > x0 ? radius - x0 : 0, last = x0 + len + radius <= n ? len + 2 * radius : n - x0 + radius;
    for (int j = 0; j < first && j < len + 2 * radius; ++j) lv[j] = f->level[indices[0] & 15u];
    for (int j = first; j < last; ++j) lv[j] = f->level[indices[x0 + j - radius] & 15u];
    for (int j = last > first ? last : first; j < len + 2 * radius; ++j) lv[j] = f->level[indices[n - 1] & 15u];
    if (radius == 1) {
        /* The default: the taps are symmetric and add up to 65536, so side * (left + right) + (65536 - 2 side) * dot is
         * the dot plus side times how far its neighbours' mean is from it, in 16-bit differences. */
        const int16_t side = (int16_t)f->tap[0];
        int16_t diff[BLOCK];
        for (int x = 0; x < len; ++x) diff[x] = (int16_t)(lv[x] + lv[x + 2] - 2 * lv[x + 1]);
        for (int x = 0; x < len; ++x) argb[x0 + x] = 0xFF000000u | (uint32_t)(((lv[x + 1] << 16) + side * diff[x] + 32768) >> 16) * 0x010101u;
        return;
    }
    int32_t acc[BLOCK];
    for (int x = 0; x < len; ++x) acc[x] = 32768;                   /* rounding */
    for (int k = 0; k < taps; ++k) {
        const int32_t t = f->tap[k];
        const int16_t *p = lv + k;
        for (int x = 0; x < len; ++x) acc[x] += t * p[x];
    }
    for (int x = 0; x < len; ++x) argb[x0 + x] = 0xFF000000u | (uint32_t)(acc[x] >> 16) * 0x010101u;
}

const mx_composite_filter *mx_composite_filter_none(void) {
    static mx_composite_filter filter;
    static int ready;
    if (!ready) {
        mx_composite_filter_design(&filter, 0.0);
        ready = 1;
    }
    return &filter;
}

/* A line of one index (a blank line of a text screen, say) is that level at once; the others go through the blocks. */
void mx_cga_composite_row(const mx_composite_filter *f, const uint8_t *indices, unsigned count, uint32_t *argb) {
    const int n = (int)count;
    if (n <= 0) return;
    if (!memcmp(indices, indices + 1, (size_t)n - 1)) {
        const uint32_t grey = f->level[indices[0] & 15u], pixel = 0xFF000000u | grey << 16 | grey << 8 | grey;
        for (int x = 0; x < n; ++x) argb[x] = pixel;
        return;
    }
    for (int x0 = 0; x0 < n; x0 += BLOCK) row_block(f, indices, n, x0, n - x0 < BLOCK ? n - x0 : BLOCK, argb);
}
