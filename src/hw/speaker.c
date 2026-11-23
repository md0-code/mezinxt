/* MezinXT - the PC speaker: an AND gate and a band-limited step per edge
 * (see speaker.h). Written for MezinXT from the textbook method of summing
 * band-limited step responses at an edge's exact position (the "BLEP"
 * idea): the output is the lowpass-filtered square wave sampled at frame
 * instants, computed edge by edge. MezinXT's own code. */
#include "speaker.h"

#include <string.h>

#define LAT  MX_SPEAKER_LATENCY
#define RES  MX_SPEAKER_STEP_RES
#define RING MX_SPEAKER_RING
#define MASK (RING - 1u)
#define CUTOFF 0.36                 /* lowpass corner, cycles per frame (17.3 kHz) */
#define HP_KEEP 0.996               /* the speaker ignores DC: a one-pole high-pass at about 30 Hz */
#define PI 3.14159265358979323846

/* sin(pi * x) without the C library's math: x is reduced to [-1/2, 1/2]
 * and summed as a Taylor series (error below 1e-11). */
static double sinpi(double x) {
    double n = x + 1.0;
    n -= 2.0 * (double)(long long)(n / 2.0);        /* x + 1 modulo 2 ... */
    if (n < 0) n += 2.0;
    x = n - 1.0;                                    /* ... so x is in [-1, 1) */
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

/* The step response of a Blackman-windowed sinc lowpass over [-LAT, LAT]
 * frames, RES entries per frame, 0 at the first and 1 at the last. */
static void build_table(double *tab) {
    const unsigned n = 2u * LAT * RES;
    for (unsigned j = 0; j <= n; j++) {
        const double t = ((double)j - (double)(LAT * RES)) / (double)RES;        /* frames from the edge */
        const double x = 2.0 * CUTOFF * t;
        const double sinc = x == 0.0 ? 1.0 : sinpi(x) / (PI * x);
        const double y = t / (double)LAT;
        const double window = 0.42 + 0.5 * sinpi(y + 0.5) + 0.08 * sinpi(2.0 * y + 0.5);
        tab[j] = sinc * window;
    }
    double prev = tab[0];
    tab[0] = 0.0;
    for (unsigned j = 1; j <= n; j++) {         /* the running integral of the kernel */
        const double cur = tab[j];
        tab[j] = tab[j - 1] + 0.5 * (prev + cur);
        prev = cur;
    }
    const double total = tab[n];
    for (unsigned j = 0; j <= n; j++) tab[j] /= total;
    tab[n] = 1.0;
}

/* The frame that `t` OSC periods falls in (floor), and the fraction of the
 * frame the time is past it, in 1/MX_SPEAKER_DEN. */
static uint64_t frame_of(uint64_t t, uint32_t *rem) {
    const uint64_t scaled = t * MX_SPEAKER_NUM;
    if (rem) *rem = (uint32_t)(scaled % MX_SPEAKER_DEN);
    return scaled / MX_SPEAKER_DEN;
}

/* The next frame's value; the filter state moves on. */
static double next_value(mx_speaker *s) {
    const unsigned i = (unsigned)(s->done & MASK);
    s->naive += s->step[i];
    s->step[i] = 0;
    const double x = ((double)s->naive + s->acc[i]) * (double)MX_SPEAKER_AMPLITUDE;
    s->acc[i] = 0.0;
    const double y = x - s->hp_x + HP_KEEP * s->hp_y;
    s->hp_x = x;
    s->hp_y = y;
    s->done++;
    return y;
}

/* Frames that are skipped (the shell is not taking them). */
static void drop(mx_speaker *s, uint64_t frames) {
    while (frames--) (void)next_value(s);
}

static int16_t to_sample(double v) {
    v = v >= 0.0 ? v + 0.5 : v - 0.5;
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return (int16_t)(long)v;
}

/* The drive changed at OSC time t: a step of `up` (1 or -1) levels. */
static void edge(mx_speaker *s, uint64_t t, int up) {
    uint32_t rem;
    uint64_t ip = frame_of(t, &rem);
    if (ip < s->done + LAT) { ip = s->done + LAT; rem = 0; }        /* an edge older than what was rendered: now */
    if (ip + LAT >= s->done + RING) drop(s, ip + LAT + 1u - RING - s->done);
    const double frac = (double)rem / (double)MX_SPEAKER_DEN;
    /* The unfiltered step begins at the first frame at or after the edge... */
    s->step[(unsigned)((ip + (rem != 0)) & MASK)] += (int16_t)up;
    /* ...and the filter's difference from it is summed over the frames the kernel reaches. */
    const double fr = frac * (double)RES;
    for (int k = -(int)LAT + 1; k <= (int)LAT; k++) {
        const double p = (double)((k + (int)LAT) * (int)RES) - fr;
        const unsigned i0 = (unsigned)p;
        double v;
        if (i0 >= MX_SPEAKER_TABLE - 1u) v = s->table[MX_SPEAKER_TABLE - 1u];
        else v = s->table[i0] + (s->table[i0 + 1u] - s->table[i0]) * (p - (double)i0);
        const int after = k > 0 || (k == 0 && rem == 0);        /* the unfiltered step is already in force */
        s->acc[(unsigned)((ip + (uint64_t)(int64_t)k) & MASK)] += (double)up * (v - (after ? 1.0 : 0.0));
    }
}

size_t mx_speaker_render(mx_speaker *s, uint64_t until, int16_t *stereo, size_t frames) {
    const uint64_t due = frame_of(until, NULL);
    const uint64_t ready = due > LAT ? due - LAT : 0;               /* frames before this are final */
    if (ready <= s->done) return 0;
    uint64_t avail = ready - s->done;
    if (avail > MX_SPEAKER_RATE) {                              /* the shell fell behind: keep the last second */
        drop(s, avail - MX_SPEAKER_RATE);
        avail = MX_SPEAKER_RATE;
    }
    const size_t n = avail < frames ? (size_t)avail : frames;
    for (size_t i = 0; i < n; i++) stereo[2u * i] = stereo[2u * i + 1u] = to_sample(next_value(s));
    return n;
}

static void spk_input(void *d, int pin, int state) {
    mx_speaker *s = d;
    if (pin == MX_SPEAKER_TIMER) s->timer = state != 0;
    else if (pin == MX_SPEAKER_DATA) s->data = state != 0;
    else return;
    const uint8_t level = s->timer & s->data;
    if (level == s->level) return;
    s->level = level;
    edge(s, mx_dev_now(&s->dev), level ? 1 : -1);
}

static uint32_t spk_read(void *d, unsigned space, uint32_t offset, unsigned size) {
    (void)d; (void)space; (void)offset; (void)size;
    return 0xFFu;
}
static void spk_write(void *d, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    (void)d; (void)space; (void)offset; (void)size; (void)value;
}
static void spk_event(void *d, uint64_t now) { (void)d; (void)now; }

/* Power-on / reset: silence from now on (the input pins keep their levels). */
static void spk_reset(void *d) {
    mx_speaker *s = d;
    memset(s->step, 0, sizeof s->step);
    memset(s->acc, 0, sizeof s->acc);
    s->level = s->timer & s->data;
    s->done = frame_of(mx_dev_now(&s->dev), NULL);
    s->naive = s->level;
    s->hp_x = (double)s->level * (double)MX_SPEAKER_AMPLITUDE;
    s->hp_y = 0.0;
}

static const mx_dev_ops SPEAKER_OPS = {spk_read, spk_write, spk_event, spk_reset, spk_input, NULL, NULL};

void mx_speaker_init(mx_speaker *s, const mx_hw_host *host) {
    memset(s, 0, sizeof *s);
    s->dev.ops = &SPEAKER_OPS;
    s->dev.host = host;
    s->dev.name = "speaker";
    build_table(s->table);
    spk_reset(s);
}
