/* MezinXT - the sound of the disk drives (drivesnd.h). Portable C11. MezinXT's own code: damped
 * resonances, noise bursts and slow oscillators summed, nothing recorded, no C library math. */
#include "drivesnd.h"

#include <string.h>

#define RATE ((double)MX_DRIVESND_RATE)
#define PI 3.14159265358979323846
#define INC (4294967296.0 / RATE)       /* a 32-bit phase's increment for 1 Hz */

/* What a queued event does. */
enum { K_MOTOR, K_STEP, K_LOAD, K_UNLOAD, K_SEEK };

/* --- the levels (sample units at volume 100) ------------------------------------------------ */

#define STEP_UNIT 2100.0                /* the diskette stepper's click: its parts sum to a peak of about 4500 */
#define TOCK_UNIT 1200.0                /* the head load's tock */
#define MOTOR_UNIT 1450.0               /* the diskette motor: an RMS of about 650 */
/* Free running: the stream of clicks and seeks that stands for the steps and the seeks that happened. */
#define SUM_STEP_GAP 288u               /* 6 ms, the BIOS's step rate */
#define SUM_STEP_BACKLOG 40u
#define SUM_SEEK_GAP 288u               /* 6 ms between the Winchester's seeks */
#define SUM_SEEK_PACE 2.0               /* a Winchester seek in the summary runs at twice its pace ... */
#define SUM_SEEK_LEVEL 0.85             /* ... and a little softer */
#define SUM_TOCK_SPACING 5760u          /* at most one tock in 120 ms */
#define SKIP_LIMIT 4800u                /* a gap in the frames is skipped by running only this many (100 ms) */

/* --- own math ---------------------------------------------------------------------------------- */

/* sin(pi * x), summed as a Taylor series after reducing x to [-1/2, 1/2] (error below 1e-11). */
static double sinpi_(double x) {
    double n = x + 1.0;
    n -= 2.0 * (double)(long long)(n / 2.0);
    if (n < 0) n += 2.0;
    x = n - 1.0;
    if (x > 0.5) x = 1.0 - x;
    else if (x < -0.5) x = -1.0 - x;
    const double z = PI * x, z2 = z * z;
    double term = z, sum = z;
    for (int i = 1; i < 12; i++) {
        term *= -z2 / ((2.0 * i) * (2.0 * i + 1.0));
        sum += term;
    }
    return sum;
}

/* e^-x for x >= 0: halve x down to 1/2, sum the series, square back. */
static double exp_neg(double x) {
    int k = 0;
    while (x > 0.5) { x *= 0.5; k++; }
    double term = 1.0, sum = 1.0;
    for (int i = 1; i < 14; i++) {
        term *= -x / i;
        sum += term;
    }
    while (k--) sum *= sum;
    return sum;
}

/* The pseudo-random sequence: xorshift32 from a fixed seed, so the same events give the same sound. */
static double rnd(mx_drivesnd *s) {                 /* -1 to 1 */
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return (double)(int32_t)x / 2147483648.0;
}

static double rnd_hd(mx_drivesnd *s) {              /* the Winchester's own: -1 to 1 */
    uint32_t x = s->rng_hd;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng_hd = x;
    return (double)(int32_t)x / 2147483648.0;
}

static uint64_t frame_of(uint64_t t) { return (t / MX_SPEAKER_DEN) * MX_SPEAKER_NUM + (t % MX_SPEAKER_DEN) * MX_SPEAKER_NUM / MX_SPEAKER_DEN; }

/* The sine table read at a 32-bit phase, interpolated. */
static double osc(const mx_drivesnd *s, uint32_t phase) {
    const unsigned i = phase >> 22;
    const double frac = (double)(phase & 0x3FFFFFu) * (1.0 / 4194304.0);
    return s->table[i] + (s->table[i + 1u] - s->table[i]) * frac;
}

/* --- where the drives sit ------------------------------------------------------------------- */

static const double PAN[MX_DRIVE_COUNT] = {-0.2, 0.2, -0.1, 0.1, 0.0, 0.0};
static double left_of(unsigned d) { return 1.0 - (PAN[d] > 0.0 ? PAN[d] : 0.0); }
static double right_of(unsigned d) { return 1.0 + (PAN[d] < 0.0 ? PAN[d] : 0.0); }

/* --- the voices --------------------------------------------------------------------------------- */

/* A damped resonance of `f` Hz that falls to 1/e in `tau` seconds, starting at amplitude `amp` (`soft`: it starts
 * at zero and swells over a quarter of its period, so that it has no hard edge to click with). */
static void add_res_x(mx_drivesnd *s, double f, double tau, double amp, unsigned d, int soft) {
    mx_drivesnd_res *v = NULL;
    for (unsigned i = 0; i < MX_DRIVESND_RES; i++) {
        const unsigned k = (s->res_next + i) % MX_DRIVESND_RES;
        if (!s->res[k].live) { v = &s->res[k]; s->res_next = k + 1u; break; }
    }
    if (!v) {                                       /* all busy: the oldest gives way */
        v = &s->res[s->res_next % MX_DRIVESND_RES];
        s->res_next++;
        s->nres--;
    }
    const double w = 2.0 * f / RATE;                /* the angle per sample in units of pi */
    v->c = sinpi_(w + 0.5);
    v->s = sinpi_(w);
    v->r = exp_neg(1.0 / (tau * RATE));
    v->x = soft ? 0.0 : amp;
    v->y = soft ? -amp : 0.0;
    v->left = left_of(d);
    v->right = right_of(d);
    v->live = 1;
    s->nres++;
}

static void add_res(mx_drivesnd *s, double f, double tau, double amp, unsigned d) { add_res_x(s, f, tau, amp, d, 0); }

/* A burst of noise low-passed by a one-pole filter (coefficient `k`), falling to 1/e in `tau` seconds. */
static void add_burst(mx_drivesnd *s, double k, double tau, double amp, unsigned d) {
    mx_drivesnd_burst *v = NULL;
    for (unsigned i = 0; i < MX_DRIVESND_BURSTS; i++) {
        const unsigned n = (s->burst_next + i) % MX_DRIVESND_BURSTS;
        if (!s->burst[n].live) { v = &s->burst[n]; s->burst_next = n + 1u; break; }
    }
    if (!v) {
        v = &s->burst[s->burst_next % MX_DRIVESND_BURSTS];
        s->burst_next++;
        s->nburst--;
    }
    v->env = amp;
    v->decay = exp_neg(1.0 / (tau * RATE));
    v->lp = 0.0;
    v->k = k;
    v->left = left_of(d);
    v->right = right_of(d);
    v->hard = d >= MX_DRIVE_HARD0;
    v->live = 1;
    s->nburst++;
}

/* A step of a diskette drive's stepper: a sharp tick, the carriage's thud, a high ring and a crack of noise.
 * Each step differs a little; one outwards is a little lower, one against the end stop heavier and longer. */
static void trigger_step(mx_drivesnd *s, unsigned d, int in, int stop) {
    const double level = 1.0 + 0.12 * rnd(s), dir = in ? 1.0 : 0.94;
    add_res(s, 2250.0 * (1.0 + 0.07 * rnd(s)) * dir, stop ? 0.0020 : 0.0011, STEP_UNIT * level * (stop ? 1.3 : 1.0), d);
    add_res(s, 560.0 * (1.0 + 0.10 * rnd(s)) * dir * (stop ? 0.7 : 1.0), stop ? 0.0070 : 0.0040, STEP_UNIT * level * (stop ? 1.6 : 0.85), d);
    add_res(s, 4300.0 * (1.0 + 0.06 * rnd(s)), 0.00045, STEP_UNIT * level * 0.4, d);
    add_burst(s, 0.6, 0.0003, STEP_UNIT * level * 0.55, d);
}

/* The head load's tock (the unload is lower and softer): a low thud, a mid knock and a rush of dull noise. */
static void trigger_tock(mx_drivesnd *s, unsigned d, int unload) {
    const double level = (unload ? 0.55 : 1.0) * (1.0 + 0.10 * rnd(s)), pitch = unload ? 0.8 : 1.0;
    add_res(s, 330.0 * pitch * (1.0 + 0.05 * rnd(s)), 0.0070, TOCK_UNIT * level * 0.7, d);
    add_res(s, 1150.0 * pitch * (1.0 + 0.06 * rnd(s)), 0.0022, TOCK_UNIT * level * 0.45, d);
    add_burst(s, 0.12, 0.0015, TOCK_UNIT * level * 0.55, d);
}

/* --- runs of clicks ----------------------------------------------------------------------------- */

static void seq_add(mx_drivesnd *s, unsigned d, uint32_t n, uint32_t gap, uint32_t cap, int in) {
    mx_drivesnd_seq *q = &s->seq[d];
    if (!q->left) q->next = s->clock;
    q->left += n;
    if (q->left > cap) q->left = cap;
    q->gap = gap;
    q->in = (uint8_t)(in != 0);
    s->seq_mask = (uint8_t)(s->seq_mask | 1u << d);
}

static void run_seqs(mx_drivesnd *s) {
    for (unsigned d = 0; d < MX_DRIVE_HARD0; d++) {
        if (!(s->seq_mask & 1u << d)) continue;
        mx_drivesnd_seq *q = &s->seq[d];
        if (s->clock < q->next) continue;
        trigger_step(s, d, q->in, 0);
        q->next = s->clock + q->gap;
        if (--q->left == 0) s->seq_mask = (uint8_t)(s->seq_mask & ~(1u << d));
    }
}

/* --- the Winchester (drivesnd.h): a 1987 Miniscribe 8425, measured ------------------------------------------------------ */

/* Every number in this section comes from the analysis of recordings of a 1987 Miniscribe 8425:
 * amplitudes are in the recordings' units, where the drive running at speed has an RMS of 642, and HD_GAIN takes them to the
 * source's level (the running spindle at an RMS of HD_RMS at volume 100). No sample of the recordings is used. */
#define HD_REF_RMS 642.0
#define HD_RMS 170.0
#define HD_GAIN (HD_RMS / HD_REF_RMS)
#define HD_FROT 60.0115                 /* the rotation, Hz: the harmonics' spacing in the running recording (3600.7 rpm) */
#define HD_LOOP_TURNS 10u               /* the tones' loop: so many turns */

/* The spindle's 28 strongest tones at full speed (65% of the running recording's power): Hz, amplitude, the rotation's harmonic (0:
 * not one) and the tone's phase as a 32-bit phase of the sine (for a harmonic, against the rotation's: they make its 60 Hz flutter).
 * The four strongest of the wandering tones near 3 kHz are set to the level of their line in the recording's spectrum (their power,
 * spread by the wandering, is the larger figure: it is left to the noise). */
static const struct { double hz, amp; unsigned n; uint32_t phase; } HD_TONE[MX_DRIVESND_HD_TONES] = {
    {319.48, 83.3, 0, 0x97F624D1u},   {772.89, 240.9, 0, 0x14913778u},  {1380.26, 72.7, 23, 0xCD5E2169u}, {1440.28, 107.2, 24, 0x6332CDC9u},
    {1522.03, 63.1, 0, 0x5A02C0E7u},  {1560.30, 122.1, 26, 0x9FAC9D4Cu}, {1569.24, 68.2, 0, 0xE84B914Bu},  {1620.31, 167.1, 27, 0xDADA95F3u},
    {1680.32, 344.0, 28, 0x8F5D5580u}, {1740.33, 111.9, 29, 0x47E3523Du}, {1800.35, 64.6, 30, 0x5BC87999u}, {2040.39, 127.1, 34, 0x2A241A1Du},
    {2296.87, 69.9, 0, 0x32C54193u},  {2320.39, 121.0, 0, 0x9F008361u},  {2599.63, 67.9, 0, 0xB714DDE6u},  {2820.54, 71.9, 47, 0x2EC60939u},
    {2836.02, 199.2, 0, 0xBF8F6DB4u}, {2857.25, 127.2, 0, 0x33296345u},  {2880.55, 99.7, 48, 0x84AD721Cu}, {3067.68, 136.2, 0, 0x83800200u},
    {3093.85, 63.0, 0, 0x674A038Bu},  {3117.32, 164.9, 0, 0x7E3B5C21u},  {3146.03, 74.5, 0, 0xAC6D52A7u},  {3325.34, 133.0, 0, 0x89065DFBu},
    {3372.01, 106.0, 0, 0x9C060D73u}, {3401.02, 113.8, 0, 0xF174510Du},  {3437.46, 88.3, 0, 0xFDC56B84u},  {3582.92, 119.7, 0, 0x4E556837u},
};

/* The noise under them: white noise through five resonant bands (centre Hz, bandwidth Hz, gain at the centre), fitted to the running
 * recording's spectrum with those tones taken out (within 2 dB in the thirds of an octave from 150 Hz to 16 kHz). The last band is the
 * motor driver's: three times as loud while it accelerates the spindle, a quarter with the motor switched off. */
static const struct { double hz, bw, gain; } HD_BAND[MX_DRIVESND_HD_BANDS] = {
    {680.0, 338.0, 596.5}, {2230.0, 1058.0, 194.3}, {3319.0, 932.0, 1152.9}, {6244.0, 1770.0, 353.7}, {13038.0, 1500.0, 168.5},
};
#define HD_DRIVER (MX_DRIVESND_HD_BANDS - 1u)
#define HD_DRIVER_UP 3.0
#define HD_DRIVER_OFF 0.25

/* Spin-up, every 0.25 s from the motor's switch-on: the tones' pitch against the running one (tracked on the tones that rise into the
 * running ones: 0.76 after a second, 0.87 after 1.7 s, 0.94 after 2.1 s, at speed after 2.5 s; in the first half second the recording's
 * tones are already at half their pitch or more, coming up out of the noise) and the level (the RMS against the running one, the room's
 * noise taken out). */
#define HD_UP_DT 0.25
static const double HD_UP_SPEED[] = {0.45, 0.55, 0.63, 0.70, 0.76, 0.80, 0.835, 0.875, 0.93, 0.985, 1.0};
static const double HD_UP_LEVEL[] = {0.0, 0.29, 0.44, 0.56, 0.67, 0.78, 0.88, 0.92, 1.0, 1.0, 1.0};
#define HD_UP_N (sizeof HD_UP_SPEED / sizeof HD_UP_SPEED[0])
/* Spin-down, every 0.1 s from the motor's switch-off: a fast fall at first (the motor brakes), then a slow coast; the level falls
 * faster than the speed, and the sound is gone after 2 s. */
#define HD_DOWN_DT 0.1
static const double HD_DOWN_SPEED[] = {1.0, 0.985, 0.96, 0.89, 0.81, 0.75, 0.71, 0.68, 0.655, 0.62, 0.58,
                                       0.55, 0.525, 0.505, 0.49, 0.475, 0.455, 0.44, 0.425, 0.41, 0.40};
static const double HD_DOWN_LEVEL[] = {1.0, 0.88, 0.97, 0.86, 0.75, 0.67, 0.56, 0.45, 0.375, 0.325, 0.265,
                                       0.23, 0.205, 0.165, 0.13, 0.12, 0.09, 0.06, 0.035, 0.015, 0.0};
#define HD_DOWN_N (sizeof HD_DOWN_SPEED / sizeof HD_DOWN_SPEED[0])

/* Once at speed the drive energises its stepper: a 1120 Hz tone (with 560 Hz, 2240 Hz and 3360 Hz at -16, -14 and -10 dB) whose
 * envelope is this (seconds from the motor's switch-on, amplitude), with a 22 Hz flutter of 10%; then it seeks track 0 itself. */
#define HD_HOLD_HZ 560.12
static const struct { double t, amp; } HD_HOLD[] = {
    {2.100, 0.0}, {2.105, 920.0}, {2.175, 920.0}, {2.185, 2300.0}, {2.240, 2300.0}, {2.250, 0.0},
    {2.285, 0.0}, {2.290, 460.0}, {2.305, 460.0}, {2.310, 0.0},   {2.325, 0.0},    {2.330, 460.0}, {2.345, 460.0}, {2.350, 0.0},
    {2.375, 0.0}, {2.380, 460.0}, {2.395, 460.0}, {2.400, 0.0},   {2.475, 0.0},    {2.480, 920.0}, {3.195, 550.0}, {3.200, 0.0},
};
#define HD_HOLD_N (sizeof HD_HOLD / sizeof HD_HOLD[0])
#define HD_PON_T 3.415                  /* the drive's own seek ... */
#define HD_PON_STEPS 200u               /* ... a long run to track 0 */

/* A seek. The step's click: damped resonances (Hz, decay to 1/e in s, amplitude, phase of the cosine). */
typedef struct hd_res { double hz, tau, amp, phase; } hd_res;
static const hd_res HD_CLICK[] = {{1983.3, 0.00132, 6000.0, -0.523}, {2700.0, 0.00091, 4130.0, 2.538}};
/* Then the stepper drive's tone, which starts 1 ms after the last step, peaks at 1.5 at 4 ms, holds at 1 to 29 ms, falls to a rattle
 * of 0.28 (50 Hz flutter) by 36 ms and is gone at 60 ms; a 1469 Hz partial rides on it at half its level. */
#define HD_TONE_HZ 2495.4
#define HD_TONE_AMP 1850.0
#define HD_RATTLE_HZ 1468.8
#define HD_RATTLE 0.49
/* And 61 ms after the last step the heads settle with the loudest sound the drive makes: these resonances and a burst of noise for
 * what they leave of the click's energy. */
#define HD_SETTLE_T 0.061
static const hd_res HD_SETTLE[] = {
    {1018.5, 0.00820, 2122.0, -1.556}, {1446.5, 0.00394, 2205.0, 2.056}, {1955.4, 0.00371, 4529.0, -0.960}, {2305.9, 0.00463, 1419.0, -2.389},
    {2907.1, 0.00336, 3316.0, 0.672},  {3285.9, 0.00497, 1309.0, -2.522}, {4701.0, 0.00194, 1947.0, 1.670},
};
#define HD_SETTLE_GAIN 0.88            /* the fit's amplitudes carry the running drive under the click: this brings the click's peak to
                                         * 16.9 times the running RMS, as in the recording */
#define HD_SETTLE_NOISE 9000.0
#define HD_SETTLE_NOISE_TAU 0.003
#define HD_SETTLE_NOISE_K 0.15           /* its low-pass (per sample: about 1.2 kHz) */
/* The steps between the first and the last: the stepper ramps from HD_V0 to HD_VMAX steps a second at HD_ACCEL and down again; each
 * step rings the body's resonances (the settling click's strongest, at their levels in it) with HD_STEP_AMP at HD_V0, less at a higher
 * rate (as the fourth root of the rate) so that the buzz keeps near the measured level (1.8 times the running RMS). */
#define HD_V0 500.0
#define HD_VMAX 5000.0
#define HD_ACCEL 150000.0
#define HD_STEP_AMP 1000.0
static const hd_res HD_BODY[MX_DRIVESND_HD_BODY] = {
    {1018.5, 0.00820, 0.469, 0.0}, {1446.5, 0.00394, 0.487, 0.0}, {1955.4, 0.00371, 1.0, 0.0}, {2907.1, 0.00336, 0.732, 0.0}, {4701.0, 0.00194, 0.430, 0.0},
};

enum { HD_OFF, HD_UP, HD_RUN, HD_DOWN };

static double sqrt_(double x) {                     /* Newton, from above */
    if (x <= 0.0) return 0.0;
    double g = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 60; i++) {
        const double n = 0.5 * (g + x / g);
        if (n >= g) break;
        g = n;
    }
    return g;
}

/* A damped resonance of the Winchester at its measured phase. */
static void hd_res_add(mx_drivesnd *s, const hd_res *p, double amp, unsigned d) {
    add_res(s, p->hz * (1.0 + 0.02 * rnd_hd(s)), p->tau, amp, d);
    mx_drivesnd_res *v = &s->res[(s->res_next + MX_DRIVESND_RES - 1u) % MX_DRIVESND_RES];
    v->x = amp * sinpi_(p->phase / PI + 0.5);
    v->y = amp * sinpi_(p->phase / PI);
}

/* The step's click (the first step of a seek). */
static void hd_click(mx_drivesnd *s, unsigned d, double level) {
    for (unsigned i = 0; i < sizeof HD_CLICK / sizeof HD_CLICK[0]; i++) hd_res_add(s, &HD_CLICK[i], HD_GAIN * HD_CLICK[i].amp * level, d);
}

/* The heads settle. */
static void hd_settle(mx_drivesnd *s, unsigned d, mx_drivesnd_buzz *b, double scale) {
    const double lv = HD_GAIN * HD_SETTLE_GAIN * b->job.level * scale * (1.0 + 0.08 * rnd_hd(s));
    for (unsigned i = 0; i < sizeof HD_SETTLE / sizeof HD_SETTLE[0]; i++) hd_res_add(s, &HD_SETTLE[i], HD_SETTLE[i].amp * lv, d);
    add_burst(s, HD_SETTLE_NOISE_K, HD_SETTLE_NOISE_TAU, HD_SETTLE_NOISE * lv, d);
    b->settled = 1;
}

/* The stepper's rate at step i of n (steps a second), and the seek's time at that ramp (seconds, n steps). */
static double hd_rate(uint32_t i, uint32_t n) {
    const uint32_t k = i < n - i ? i : n - i;
    const double v = sqrt_(HD_V0 * HD_V0 + 2.0 * HD_ACCEL * (double)k);
    return v < HD_VMAX ? v : HD_VMAX;
}
static double hd_profile(uint32_t n) {
    const double half = (double)(n - 1u) / 2.0, kacc = (HD_VMAX * HD_VMAX - HD_V0 * HD_V0) / (2.0 * HD_ACCEL);
    if (half <= kacc) return 2.0 * (sqrt_(HD_V0 * HD_V0 + 2.0 * HD_ACCEL * half) - HD_V0) / HD_ACCEL;
    return 2.0 * (HD_VMAX - HD_V0) / HD_ACCEL + ((double)(n - 1u) - 2.0 * kacc) / HD_VMAX;
}

/* The frames to the step after step i (the steps are not quite even). */
static double hd_interval(mx_drivesnd *s, const mx_drivesnd_buzz *b) {
    return RATE / hd_rate(b->step, b->job.steps) * b->job.stretch * (1.0 + 0.05 * rnd_hd(s));
}

/* The sound of a seek of `distance` cylinders that the coupler says takes `us` microseconds: as many steps; a time longer than the
 * drive's own ramp stretches it (to twice); every seek a little different in level. `summary`: at twice the pace. */
static mx_drivesnd_job make_job(mx_drivesnd *s, uint32_t distance, uint32_t us, int summary) {
    mx_drivesnd_job j;
    j.steps = distance > 1024u ? 1024u : distance;
    j.level = (1.0 + 0.10 * rnd_hd(s)) * (summary ? SUM_SEEK_LEVEL : 1.0);
    j.stretch = 1.0;
    if (j.steps > 1u) {
        const double own = hd_profile(j.steps), want = us / 1e6;
        if (want > own) j.stretch = want / own < 2.0 ? want / own : 2.0;
    }
    j.pace = summary ? SUM_SEEK_PACE : 1.0;
    if (summary) j.stretch /= SUM_SEEK_PACE;
    return j;
}

static void buzz_start(mx_drivesnd *s, unsigned d, mx_drivesnd_buzz *b, const mx_drivesnd_job *j) {
    b->job = *j;
    s->buzz_mask = (uint8_t)(s->buzz_mask | 1u << d);
    hd_click(s, d, j->level);
    b->step = 1;
    b->t = 0;
    b->settled = 0;
    b->tone = 0;
    if (j->steps > 1u) {
        b->phase = 1;
        b->next = hd_interval(s, b);
    } else b->phase = 2;
}

/* One step of the run: the body rings. */
static void hd_step(mx_drivesnd *s, mx_drivesnd_buzz *b) {
    const double g = HD_GAIN * HD_STEP_AMP * b->job.level * sqrt_(sqrt_(HD_V0 / hd_rate(b->step, b->job.steps))) * (1.0 + 0.10 * rnd_hd(s));
    for (unsigned k = 0; k < MX_DRIVESND_HD_BODY; k++) b->bx[k] += g * HD_BODY[k].amp;
    b->body = 1;
}

/* The stepper drive's tone at `ts` seconds after the last step. */
static double hd_tone_env(const mx_drivesnd *s, double ts) {
    if (ts < 0.001) return 0.0;
    if (ts < 0.004) return 1.5 * (ts - 0.001) / 0.003;
    if (ts < 0.007) return 1.5 - 0.5 * (ts - 0.004) / 0.003;
    if (ts < 0.029) return 1.0;
    if (ts < 0.036) return 0.28 + 0.72 * exp_neg((ts - 0.029) / 0.0025);
    const double rattle = 0.28 * (1.0 + 0.25 * osc(s, (uint32_t)(uint64_t)((ts - 0.036) * 50.0 * 4294967296.0)));
    if (ts < 0.056) return rattle;
    if (ts < 0.060) return rattle * (0.060 - ts) / 0.004;
    return 0.0;
}

/* A Winchester's seek sound, one frame: the run of steps, the tone, the settling click, the body's ring; the waiting jobs of the summary. */
static double run_buzz(mx_drivesnd *s, unsigned d, mx_drivesnd_buzz *b) {
    double y = 0.0;
    if (b->phase == 1) {
        b->next -= 1.0;
        while (b->phase == 1 && b->next <= 0.0) {
            hd_step(s, b);
            if (++b->step >= b->job.steps) {
                b->phase = 2;
                b->t = 0;
            } else b->next += hd_interval(s, b);
        }
    } else if (b->phase == 2) {
        const double ts = (double)b->t++ * b->job.pace / RATE;
        const double env = hd_tone_env(s, ts) * HD_GAIN * HD_TONE_AMP * b->job.level;
        b->tone += (uint32_t)(HD_RATTLE_HZ * INC);
        if (env != 0.0) y += env * (osc(s, (uint32_t)(uint64_t)((double)b->t * (HD_TONE_HZ * INC))) + HD_RATTLE * osc(s, b->tone));
        if (ts >= HD_SETTLE_T) {
            if (!b->settled) hd_settle(s, d, b, 1.0);
            b->phase = 0;
            if (b->qn) b->wait = SUM_SEEK_GAP;
        }
    } else if (b->wait > 1u) b->wait--;
    else if (b->qn) {
        b->wait = 0;
        const mx_drivesnd_job j = b->q[0];
        memmove(b->q, b->q + 1, (--b->qn) * sizeof b->q[0]);
        buzz_start(s, d, b, &j);
    } else b->wait = 0;
    if (b->body) {
        double e = 0.0;
        for (unsigned k = 0; k < MX_DRIVESND_HD_BODY; k++) {
            const double x = b->bx[k], v = b->by[k], r = s->br[k];
            y += x;
            b->bx[k] = r * (x * s->bc[k] - v * s->bs[k]);
            b->by[k] = r * (x * s->bs[k] + v * s->bc[k]);
            e += b->bx[k] * b->bx[k] + b->by[k] * b->by[k];
        }
        if (e < 0.01 && b->phase != 1) {
            b->body = 0;
            memset(b->bx, 0, sizeof b->bx);
            memset(b->by, 0, sizeof b->by);
        }
    }
    if (!b->phase && !b->wait && !b->qn && !b->body) s->buzz_mask = (uint8_t)(s->buzz_mask & ~(1u << d));
    return y;
}

/* A seek event of the coupler: in guest time it takes over from the seek that may still sound (the heads settle for it at once if they had
 * not); in the summary it waits its turn (a seek that does not fit in the backlog is lost). */
static void seek_start(mx_drivesnd *s, unsigned d, const mx_drivesnd_job *j, int summary) {
    mx_drivesnd_buzz *b = &s->buzz[d - MX_DRIVE_HARD0];
    if (summary) {
        if (b->phase || b->wait || b->qn) {
            if (b->qn < MX_DRIVESND_JOBS) b->q[b->qn++] = *j;
            return;
        }
    } else {
        if (b->phase == 2 && !b->settled) hd_settle(s, d, b, 0.6);
        b->qn = b->wait = 0;
    }
    buzz_start(s, d, b, j);
}

static void seek_event(mx_drivesnd *s, unsigned d, uint32_t a, uint32_t us, int summary) {
    if (d < MX_DRIVE_HARD0) return;
    const uint32_t distance = a & ~MX_DRIVE_SEEK_HOME;
    if (!distance) return;
    const mx_drivesnd_job j = make_job(s, distance, us, summary);
    seek_start(s, d, &j, summary);
}

/* --- the Winchester's spindle ------------------------------------------------------------------------------------------------ */

static double table_at(const double *v, size_t n, double dt, double t) {
    const double x = t / dt;
    if (x >= (double)(n - 1u)) return v[n - 1u];
    const size_t i = (size_t)x;
    return v[i] + (v[i + 1u] - v[i]) * (x - (double)i);
}

/* The tones' loop at the spindle's speed: ten turns of the disk. */
static void hd_rates(mx_drivesnd_hd *h) { h->loop_inc = (uint32_t)(HD_FROT / HD_LOOP_TURNS * INC * h->speed); }

/* The time in a table of speeds at which it passes `speed` (rising or falling). */
static uint32_t hd_time_at(const double *v, size_t n, double dt, double speed, int rising) {
    for (size_t i = 0; i + 1u < n; i++) {
        const double a = v[i], b = v[i + 1u];
        if (rising ? (b >= speed) : (b <= speed)) {
            const double f = b != a ? (speed - a) / (b - a) : 0.0;
            return (uint32_t)(((double)i + (f < 0.0 ? 0.0 : f > 1.0 ? 1.0 : f)) * dt * RATE);
        }
    }
    return (uint32_t)((double)(n - 1u) * dt * RATE);
}

/* The spindle is switched on: from rest the drive powers up (and energises its stepper and seeks once at speed); running down, the motor
 * takes it up again from its speed. */
static void hd_start(mx_drivesnd *s, unsigned d) {
    mx_drivesnd_hd *h = &s->hd[d - MX_DRIVE_HARD0];
    if (h->state == HD_UP || h->state == HD_RUN) return;
    if (h->state == HD_OFF) {
        memset(h, 0, sizeof *h);
        h->pon = 1;
    } else {
        h->t = hd_time_at(HD_UP_SPEED, HD_UP_N, HD_UP_DT, h->speed, 1);
        h->cur = HD_HOLD_N;
        h->pon = 0;
    }
    h->state = HD_UP;
    s->spin_mask = (uint8_t)(s->spin_mask | 1u << d);
}

static void hd_stop(mx_drivesnd *s, unsigned d) {
    mx_drivesnd_hd *h = &s->hd[d - MX_DRIVE_HARD0];
    if (h->state != HD_UP && h->state != HD_RUN) return;
    h->t = hd_time_at(HD_DOWN_SPEED, HD_DOWN_N, HD_DOWN_DT, h->speed, 0);
    h->state = HD_DOWN;
    h->cur = HD_HOLD_N;
    h->pon = 0;
}

/* A Winchester spindle, one frame. */
static double hd_frame(mx_drivesnd *s, unsigned d) {
    mx_drivesnd_hd *h = &s->hd[d - MX_DRIVE_HARD0];
    const double ts = (double)h->t++ * (1.0 / RATE);
    double hold = 0.0;
    switch (h->state) {
    case HD_UP:
        h->speed = table_at(HD_UP_SPEED, HD_UP_N, HD_UP_DT, ts);
        h->level = table_at(HD_UP_LEVEL, HD_UP_N, HD_UP_DT, ts);
        h->hf = h->speed < 1.0 ? HD_DRIVER_UP : 1.0;
        if (h->cur < HD_HOLD_N && ts >= HD_HOLD[0].t) {
            while (h->cur + 1u < HD_HOLD_N && ts >= HD_HOLD[h->cur + 1u].t) h->cur++;
            if (h->cur + 1u >= HD_HOLD_N) h->cur = HD_HOLD_N;
            else {
                const double a = HD_HOLD[h->cur].amp, b = HD_HOLD[h->cur + 1u].amp;
                const double amp = a + (b - a) * (ts - HD_HOLD[h->cur].t) / (HD_HOLD[h->cur + 1u].t - HD_HOLD[h->cur].t);
                h->hold += (uint32_t)(HD_HOLD_HZ * INC);
                const uint32_t p = h->hold, f = (uint32_t)(uint64_t)((double)h->t * (22.0 * INC));
                hold = amp * (1.0 + 0.1 * osc(s, f)) * (0.153 * osc(s, p) + osc(s, 2u * p) + 0.204 * osc(s, 4u * p) + 0.331 * osc(s, 6u * p));
            }
        }
        if (h->pon && ts >= HD_PON_T) {
            h->pon = 0;
            const mx_drivesnd_job j = {HD_PON_STEPS, 1.0, 1.0, 1.0};
            seek_start(s, d, &j, 0);
        }
        if (h->speed >= 1.0 && !h->pon && h->cur >= HD_HOLD_N) {
            h->state = HD_RUN;
            h->hf = 1.0;
            hd_rates(h);
        }
        break;
    case HD_DOWN:
        if (ts >= (double)(HD_DOWN_N - 1u) * HD_DOWN_DT) {
            h->state = HD_OFF;
            h->speed = h->level = 0.0;
            s->spin_mask = (uint8_t)(s->spin_mask & ~(1u << d));
            return 0.0;
        }
        h->speed = table_at(HD_DOWN_SPEED, HD_DOWN_N, HD_DOWN_DT, ts);
        h->level = table_at(HD_DOWN_LEVEL, HD_DOWN_N, HD_DOWN_DT, ts);
        h->hf = HD_DRIVER_OFF;
        break;
    default: break;
    }
    if (h->state != HD_RUN && (!(h->t & 31u) || h->t == 1u)) hd_rates(h);
    double tones = 0.0, noise = 0.0;
    {
        const uint32_t i = (h->loop += h->loop_inc) >> 18;
        const float frac = (float)(h->loop & 0x3FFFFu) * (1.0f / 262144.0f);
        tones = s->hd_loop[i] + (s->hd_loop[i + 1u] - s->hd_loop[i]) * frac;
    }
    const double w = rnd_hd(s) * 1.7320508075688772;   /* unit variance */
    for (unsigned k = 0; k < MX_DRIVESND_HD_BANDS; k++) {
        const double y = s->nb[k][0] * w + s->nb[k][1] * h->n1[k] - s->nb[k][2] * h->n2[k];
        h->n2[k] = h->n1[k];
        h->n1[k] = y;
        noise += k == HD_DRIVER ? h->hf * y : y;
    }
    return HD_GAIN * (h->level * (tones + noise) + hold);
}

/* The noise bands', the body's coefficients and the tones' loop: ten turns of the disk, in which every tone makes a whole number of
 * cycles (the rotation's harmonics exactly, the others within 3 Hz of their frequency), each summed as a rotating phasor from its phase. */
static void hd_init(mx_drivesnd *s) {
    memset(s->hd_loop, 0, sizeof s->hd_loop);
    for (unsigned t = 0; t < MX_DRIVESND_HD_TONES; t++) {
        const double cycles = HD_TONE[t].n ? (double)(HD_TONE[t].n * HD_LOOP_TURNS) : (double)(long)(HD_TONE[t].hz * HD_LOOP_TURNS / HD_FROT + 0.5);
        const double w = 2.0 * cycles / MX_DRIVESND_HD_LOOP, p = (double)HD_TONE[t].phase / 2147483648.0;     /* in units of pi */
        const double c = sinpi_(w + 0.5), sn = sinpi_(w);
        double x = sinpi_(p), y = sinpi_(p + 0.5);         /* sin and cos of the phase */
        for (unsigned i = 0; i <= MX_DRIVESND_HD_LOOP; i++) {
            s->hd_loop[i] += (float)(HD_TONE[t].amp * x);
            const double nx = x * c + y * sn, ny = y * c - x * sn;
            x = nx;
            y = ny;
        }
    }
    for (unsigned k = 0; k < MX_DRIVESND_HD_BANDS; k++) {
        const double r = exp_neg(PI * HD_BAND[k].bw / RATE), w = 2.0 * HD_BAND[k].hz / RATE;     /* w: the centre in units of pi */
        const double a1 = 2.0 * r * sinpi_(w + 0.5), a2 = r * r;
        const double re = 1.0 - a1 * sinpi_(w + 0.5) + a2 * sinpi_(2.0 * w + 0.5), im = a1 * sinpi_(w) - a2 * sinpi_(2.0 * w);
        s->nb[k][0] = HD_BAND[k].gain * sqrt_(re * re + im * im);     /* the gain at the centre: the denominator's magnitude there */
        s->nb[k][1] = a1;
        s->nb[k][2] = a2;
    }
    for (unsigned k = 0; k < MX_DRIVESND_HD_BODY; k++) {
        const double w = 2.0 * HD_BODY[k].hz / RATE;
        s->bc[k] = sinpi_(w + 0.5);
        s->bs[k] = sinpi_(w);
        s->br[k] = exp_neg(1.0 / (HD_BODY[k].tau * RATE));
    }
}

/* --- the diskette motors ---------------------------------------------------------------------------------- */

/* A diskette motor at speed fraction v: a 120 Hz hum with its harmonics, a band of low noise, a little hiss and the
 * flutter of the disk's 5 Hz turn; its pitch follows its speed and its loudness the square of it. */
static double floppy_motor(mx_drivesnd *s, mx_drivesnd_spin *p, unsigned d, double v) {
    p->phase[0] += (uint32_t)((112.0 + 6.0 * (double)d) * v * INC);
    p->phase[1] += (uint32_t)(5.0 * v * INC);
    const uint32_t ph = p->phase[0];
    const double tone = 0.50 * osc(s, ph) + 0.28 * osc(s, ph * 2u) + 0.16 * osc(s, ph * 3u);
    const double flutter = 1.0 + 0.2 * osc(s, p->phase[1]);
    const double w = rnd(s);
    p->lp1 += 0.075 * (w - p->lp1);
    p->lp2 += 0.075 * (p->lp1 - p->lp2);
    p->lp3 += 0.10 * (w - p->lp3);
    return MOTOR_UNIT * v * v * flutter * (tone + 2.6 * p->lp2 + 0.09 * (w - p->lp3));
}

static void spin_frame(mx_drivesnd *s, unsigned d, double *l, double *r) {
    if (d >= MX_DRIVE_HARD0) {                      /* the fixed disks sit in the middle */
        const double y = hd_frame(s, d);
        *l += y;
        *r += y;
        return;
    }
    mx_drivesnd_spin *p = &s->spin[d];
    p->level += (p->target - p->level) * (p->target > p->level ? s->atk : s->rel);
    if (p->target == 0.0 && p->level < 0.003) {
        p->level = 0.0;
        s->spin_mask = (uint8_t)(s->spin_mask & ~(1u << d));
        return;
    }
    const double y = floppy_motor(s, p, d, p->level);
    *l += y * left_of(d);
    *r += y * right_of(d);
}

/* --- one frame -------------------------------------------------------------------------------------- */

static void synth_frame(mx_drivesnd *s, double *outl, double *outr) {
    double l = 0.0, r = 0.0;
    s->clock++;
    if (s->seq_mask) run_seqs(s);
    if (s->spin_mask)
        for (unsigned d = 0; d < MX_DRIVE_COUNT; d++)
            if (s->spin_mask & 1u << d) spin_frame(s, d, &l, &r);
    if (s->buzz_mask)
        for (unsigned d = MX_DRIVE_HARD0; d < MX_DRIVE_COUNT; d++)
            if (s->buzz_mask & 1u << d) {
                const double y = run_buzz(s, d, &s->buzz[d - MX_DRIVE_HARD0]);
                l += y;
                r += y;
            }
    if (s->nres) {
        for (unsigned i = 0; i < MX_DRIVESND_RES; i++) {
            mx_drivesnd_res *v = &s->res[i];
            if (!v->live) continue;
            l += v->x * v->left;
            r += v->x * v->right;
            const double nx = v->r * (v->x * v->c - v->y * v->s), ny = v->r * (v->x * v->s + v->y * v->c);
            v->x = nx;
            v->y = ny;
            if (nx * nx + ny * ny < 0.04) { v->live = 0; s->nres--; }
        }
    }
    if (s->nburst) {
        for (unsigned i = 0; i < MX_DRIVESND_BURSTS; i++) {
            mx_drivesnd_burst *v = &s->burst[i];
            if (!v->live) continue;
            v->lp += v->k * ((v->hard ? rnd_hd(s) : rnd(s)) * v->env - v->lp);
            l += v->lp * v->left;
            r += v->lp * v->right;
            v->env *= v->decay;
            if (v->env < 0.1) { v->live = 0; s->nburst--; }
        }
    }
    *outl = l * s->gain;
    *outr = r * s->gain;
}

static int to_s16(double v) {
    v = v >= 0.0 ? v + 0.5 : v - 0.5;
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return (int)(long)v;
}

static int clip(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

/* --- events ------------------------------------------------------------------------------------------ */

/* An event takes effect. `summary`: free running, so the clicks are queued as a stream at the clock's pace instead of
 * sounding at their time. */
static void apply(mx_drivesnd *s, const mx_drivesnd_item *e, int summary) {
    const unsigned d = e->drive;
    switch (e->kind) {
    case K_MOTOR:
        if (d >= MX_DRIVE_HARD0) {
            if (e->a) hd_start(s, d);
            else hd_stop(s, d);
            break;
        }
        s->spin[d].target = e->a ? 1.0 : 0.0;
        if (e->a) s->spin_mask = (uint8_t)(s->spin_mask | 1u << d);
        break;
    case K_STEP:
        if (summary) seq_add(s, d, 1, SUM_STEP_GAP, SUM_STEP_BACKLOG, (int)e->a);
        else trigger_step(s, d, (int)e->a, (int)e->b);
        break;
    case K_LOAD:
    case K_UNLOAD:
        if (e->kind == K_UNLOAD) s->loaded = (uint8_t)(s->loaded & ~(1u << d));
        if (summary) {
            if (s->clock < s->tock_next[d]) break;
            s->tock_next[d] = s->clock + SUM_TOCK_SPACING;
        }
        trigger_tock(s, d, e->kind == K_UNLOAD);
        break;
    case K_SEEK:
        seek_event(s, d, e->a, e->b, summary);
        break;
    default: break;
    }
}

static void pop_apply(mx_drivesnd *s, int summary) {
    const mx_drivesnd_item e = s->q[s->qh++];
    if (s->qh == s->qn) {
        s->qh = s->qn = 0;
        s->next_frame = UINT64_MAX;
    } else s->next_frame = frame_of(s->q[s->qh].t);
    if (!e.dead) apply(s, &e, summary);
}

/* The event into the queue, in time order (events arrive in order but for a head unload, which the source
 * places itself after the hold time). A full queue plays its oldest event as a summary. */
static void push(mx_drivesnd *s, const mx_drivesnd_item *e) {
    if (s->qn - s->qh >= MX_DRIVESND_QUEUE) pop_apply(s, 1);
    if (s->qn == MX_DRIVESND_QUEUE) {
        memmove(s->q, s->q + s->qh, (s->qn - s->qh) * sizeof s->q[0]);
        s->qn -= s->qh;
        s->qh = 0;
    }
    unsigned i = s->qn++;
    while (i > s->qh && s->q[i - 1u].t > e->t) {
        s->q[i] = s->q[i - 1u];
        i--;
    }
    s->q[i] = *e;
    s->next_frame = frame_of(s->q[s->qh].t);
}

/* The head unload that is pending for drive `d` is cancelled when something uses the head at time `t`, before it; one
 * that is already due stays. */
static void cancel_hold(mx_drivesnd *s, unsigned d, uint64_t t) {
    if (s->hold_t[d] == MX_NEVER) return;
    if (s->hold_t[d] > t)
        for (unsigned i = s->qh; i < s->qn; i++)
            if (s->q[i].kind == K_UNLOAD && s->q[i].drive == d && s->q[i].t == s->hold_t[d]) s->q[i].dead = 1;
    s->hold_t[d] = MX_NEVER;
}

void mx_drivesnd_event(void *ctx, unsigned drive, unsigned event, uint64_t t, uint32_t a, uint32_t b) {
    mx_drivesnd *s = ctx;
    if (!s->enabled || drive >= MX_DRIVE_COUNT) return;
    mx_drivesnd_item e;
    memset(&e, 0, sizeof e);
    e.t = t;
    e.a = a;
    e.b = b;
    e.drive = (uint8_t)drive;
    switch (event) {
    case MX_DRIVE_MOTOR:
    case MX_DRIVE_SPINDLE: e.kind = K_MOTOR; e.a = a != 0; break;
    case MX_DRIVE_STEP: e.kind = K_STEP; break;
    case MX_DRIVE_HEAD_LOAD:
        cancel_hold(s, drive, t);
        s->loaded = (uint8_t)(s->loaded | 1u << drive);
        e.kind = K_LOAD;
        break;
    case MX_DRIVE_HEAD_HOLD:
        if (!(s->loaded & 1u << drive)) return;
        cancel_hold(s, drive, t);
        e.t = t + a;
        s->hold_t[drive] = e.t;
        e.kind = K_UNLOAD;
        break;
    case MX_DRIVE_SEEK: e.kind = K_SEEK; break;
    default: return;
    }
    push(s, &e);
}

/* --- the source ---------------------------------------------------------------------------------------- */

static void clear(mx_drivesnd *s) {
    memset(s->q, 0, sizeof s->q);
    memset(s->spin, 0, sizeof s->spin);
    memset(s->seq, 0, sizeof s->seq);
    memset(s->hd, 0, sizeof s->hd);
    memset(s->buzz, 0, sizeof s->buzz);
    memset(s->res, 0, sizeof s->res);
    memset(s->burst, 0, sizeof s->burst);
    memset(s->tock_next, 0, sizeof s->tock_next);
    for (unsigned d = 0; d < MX_DRIVE_COUNT; d++) s->hold_t[d] = MX_NEVER;
    s->qh = s->qn = 0;
    s->next_frame = UINT64_MAX;
    s->loaded = 0;
    s->spin_mask = s->seq_mask = s->buzz_mask = 0;
    s->res_next = s->burst_next = s->nres = s->nburst = 0;
}

void mx_drivesnd_init(mx_drivesnd *s) {
    memset(s, 0, sizeof *s);
    s->rng = 0x2545F491u;
    s->rng_hd = 0x9E3779B9u;
    for (unsigned i = 0; i <= MX_DRIVESND_TABLE; i++) s->table[i] = sinpi_(2.0 * (double)i / (double)MX_DRIVESND_TABLE);
    s->atk = 1.0 - exp_neg(1.0 / (0.14 * RATE));
    s->rel = 1.0 - exp_neg(1.0 / (0.50 * RATE));
    hd_init(s);
    clear(s);
}

void mx_drivesnd_set(mx_drivesnd *s, int enabled, unsigned volume) {
    const int on = enabled && volume > 0;
    s->gain = (volume > 100u ? 100u : volume) / 100.0;
    if (on != s->enabled) {
        s->enabled = on;
        clear(s);
    }
}

int mx_drivesnd_busy(const mx_drivesnd *s) { return s->nres || s->nburst || s->spin_mask || s->seq_mask || s->buzz_mask || s->qh < s->qn; }

/* Frames that nobody takes: the source goes on without output, so its events and its decays stay in step. */
static void skip(mx_drivesnd *s, uint64_t frames) {
    if (frames > SKIP_LIMIT) {
        s->done += frames - SKIP_LIMIT;
        s->clock += frames - SKIP_LIMIT;
        frames = SKIP_LIMIT;
    }
    while (frames--) {
        while (s->next_frame <= s->done) pop_apply(s, 0);
        double l, r;
        synth_frame(s, &l, &r);
        s->done++;
    }
}

void mx_drivesnd_mix(mx_drivesnd *s, uint64_t first, int16_t *stereo, size_t frames) {
    if (!s->enabled || !frames) return;
    if (first > s->done) skip(s, first - s->done);
    const uint64_t end = s->done + frames;
    if (!(s->nres || s->nburst || s->spin_mask || s->seq_mask || s->buzz_mask) && s->next_frame >= end) {       /* nothing to make */
        s->done = end;
        s->clock += frames;
        return;
    }
    for (size_t i = 0; i < frames; i++) {
        while (s->next_frame <= s->done) pop_apply(s, 0);
        double l, r;
        synth_frame(s, &l, &r);
        s->done++;
        stereo[2u * i] = (int16_t)clip(stereo[2u * i] + to_s16(l));
        stereo[2u * i + 1u] = (int16_t)clip(stereo[2u * i + 1u] + to_s16(r));
    }
}

size_t mx_drivesnd_free(mx_drivesnd *s, uint64_t until, int16_t *stereo, size_t frames) {
    if (!s->enabled) {
        memset(stereo, 0, frames * 4u);
        return frames;
    }
    while (s->qh < s->qn && s->q[s->qh].t <= until) pop_apply(s, 1);
    const uint64_t f = frame_of(until);
    if (f > s->done) s->done = f;
    for (size_t i = 0; i < frames; i++) {
        double l, r;
        synth_frame(s, &l, &r);
        stereo[2u * i] = (int16_t)to_s16(l);
        stereo[2u * i + 1u] = (int16_t)to_s16(r);
    }
    return frames;
}
