/* MezinXT - Motorola MC6845 CRT controller. See mc6845.h for the register
 * file, the cursor and the timing model. MezinXT's own code, written from
 * the MC6845 data sheet. */
#include "mc6845.h"

#include <string.h>

#define R(c, n) ((unsigned)(c)->reg[n])

/* --- the frame's geometry ------------------------------------------------------------- */

static unsigned rows_h(const mx_mc6845 *c) { return R(c, 9) + 1u; }                  /* lines per row */
static uint32_t visible_lines(const mx_mc6845 *c) { return (R(c, 4) + 1u) * rows_h(c); }
static uint32_t frame_lines(const mx_mc6845 *c) { return visible_lines(c) + R(c, 5); }
static uint16_t start_address(const mx_mc6845 *c) { return (uint16_t)((R(c, 12) << 8 | R(c, 13)) & MX_MC6845_MA_MASK); }
static uint16_t cursor_address(const mx_mc6845 *c) { return (uint16_t)((R(c, 14) << 8 | R(c, 15)) & MX_MC6845_MA_MASK); }

static uint32_t line_index(const mx_mc6845 *c, const mx_mc6845_beam *b) {
    return b->adj ? visible_lines(c) + b->ra : (uint32_t)b->row * rows_h(c) + b->ra;
}

/* VSYNC covers 16 lines from the first line of row R7; it runs on over the
 * end of the frame if it starts that close to it. */
static int vsync_at(const mx_mc6845 *c, uint32_t index) {
    const uint32_t f = frame_lines(c), vs = R(c, 7) * rows_h(c);
    if (vs >= f) return 0;
    return (index % f + f - vs) % f < MX_MC6845_VSYNC_LINES;
}

/* Vertical display enable: set at the start of the frame, cleared when the
 * row counter meets R6. The adjust lines are row R4+1 (not wrapped). */
static int vde_at(const mx_mc6845 *c, const mx_mc6845_beam *b) {
    return (b->adj ? R(c, 4) + 1u : b->row) < R(c, 6);
}

static int cursor_scan(const mx_mc6845 *c, unsigned ra) {
    const unsigned s = R(c, 10) & 31u, e = R(c, 11) & 31u;
    return ra >= s && (s > e || ra <= e);
}

static int cursor_phase(const mx_mc6845 *c, uint64_t frame) {
    switch ((R(c, 10) >> 5) & 3u) {
    case MX_MC6845_CURSOR_STEADY: return 1;
    case MX_MC6845_CURSOR_OFF: return 0;
    case MX_MC6845_CURSOR_BLINK16: return !(frame & 8u);
    default: return !(frame & 16u);
    }
}

/* --- the counters --------------------------------------------------------------------- */

static void new_frame(const mx_mc6845 *c, mx_mc6845_beam *b) {
    b->row = b->ra = b->adj = 0;
    b->start = b->row_start = start_address(c);
    b->frame++;
}

/* The horizontal counter has met R0 (or wrapped into it): the next line. */
static void end_line(const mx_mc6845 *c, mx_mc6845_beam *b) {
    b->h = 0;
    if (b->adj) {
        if (b->ra + 1u >= R(c, 5)) new_frame(c, b);
        else b->ra++;
        return;
    }
    if (b->ra == R(c, 9)) {
        b->ra = 0;
        b->row_start = (uint16_t)((b->row_start + R(c, 1)) & MX_MC6845_MA_MASK);
        if (b->row == R(c, 4)) {
            if (R(c, 5)) {
                b->adj = 1;
                b->row = (uint8_t)((b->row + 1u) & 0x7Fu);
            } else {
                new_frame(c, b);
            }
        } else {
            b->row = (uint8_t)((b->row + 1u) & 0x7Fu);
        }
    } else {
        b->ra = (uint8_t)((b->ra + 1u) & 0x1Fu);
    }
}

/* Counters inside their limits: the closed form below applies. */
static int consistent(const mx_mc6845 *c, const mx_mc6845_beam *b) {
    if (b->adj) return b->ra < R(c, 5) && b->row == ((R(c, 4) + 1u) & 0x7Fu);
    return b->row <= R(c, 4) && b->ra <= R(c, 9);
}

/* `n` whole lines on from a line boundary. */
static void advance_lines(const mx_mc6845 *c, mx_mc6845_beam *b, uint64_t n) {
    while (n) {
        if (!consistent(c, b)) {            /* stepping until the counters wrap into range */
            end_line(c, b);
            --n;
            continue;
        }
        const uint64_t rh = rows_h(c), v = visible_lines(c), f = frame_lines(c);
        const uint64_t li = b->adj ? v + b->ra : (uint64_t)b->row * rh + b->ra;
        const uint64_t total = li + n, frames = total / f, lin = total % f;
        const uint64_t rows_old = b->adj ? R(c, 4) + 1u : b->row;
        const uint64_t rows_new = lin < v ? lin / rh : R(c, 4) + 1u;
        if (lin < v) {
            b->adj = 0;
            b->row = (uint8_t)(lin / rh);
            b->ra = (uint8_t)(lin % rh);
        } else {
            b->adj = 1;
            b->row = (uint8_t)((R(c, 4) + 1u) & 0x7Fu);
            b->ra = (uint8_t)(lin - v);
        }
        if (frames) {
            b->start = start_address(c);
            b->row_start = (uint16_t)((b->start + rows_new * R(c, 1)) & MX_MC6845_MA_MASK);
            b->frame += frames;
        } else {
            b->row_start = (uint16_t)((b->row_start + (rows_new - rows_old) * R(c, 1)) & MX_MC6845_MA_MASK);
        }
        return;
    }
}

/* `chars` character clocks on (the beam's time is the caller's). */
static void beam_advance(const mx_mc6845 *c, mx_mc6845_beam *b, uint64_t chars) {
    const uint32_t left = ((uint32_t)(R(c, 0) - b->h) & 0xFFu) + 1u;     /* to the end of this line */
    if (chars < left) {
        b->h = (uint8_t)(b->h + chars);
        return;
    }
    chars -= left;
    end_line(c, b);
    const uint64_t ll = R(c, 0) + 1u;
    advance_lines(c, b, chars / ll);
    b->h = (uint8_t)(chars % ll);
}

/* The counters at `now`. */
static void sync(mx_mc6845 *c, uint64_t now) {
    mx_mc6845_beam *b = &c->beam;
    if (now <= b->t) return;
    const uint64_t chars = (now - b->t) / c->period;
    if (!chars) return;
    beam_advance(c, b, chars);
    b->t += chars * c->period;
}

static uint16_t beam_ma(const mx_mc6845_beam *b) { return (uint16_t)((b->row_start + b->h) & MX_MC6845_MA_MASK); }

/* --- VSYNC pin ------------------------------------------------------------------------ */

/* Drives the pin to the present level and arms the next edge. */
static void vsync_resched(mx_mc6845 *c) {
    if (!c->vsync_enabled) return;
    const int level = vsync_at(c, line_index(c, &c->beam));
    if (level != c->vsync_level) {
        c->vsync_level = (uint8_t)level;
        mx_dev_line(&c->dev, MX_MC6845_PIN_VSYNC, level);
    }
    /* The edges fall on line starts: walk the lines after this one. */
    mx_mc6845_beam b = c->beam;
    uint64_t t = b.t + (uint64_t)(((uint32_t)(R(c, 0) - b.h) & 0xFFu) + 1u) * c->period;
    const uint64_t step = (R(c, 0) + 1u) * c->period;
    end_line(c, &b);
    const uint32_t limit = 2u * frame_lines(c) + 2u * MX_MC6845_VSYNC_LINES + 2u;
    for (uint32_t i = 0; i < limit; ++i) {
        if (vsync_at(c, line_index(c, &b)) != level) {
            mx_dev_schedule(&c->dev, t);
            return;
        }
        t += step;
        end_line(c, &b);
    }
    mx_dev_schedule(&c->dev, MX_NEVER);
}

/* --- registers ------------------------------------------------------------------------ */

static const uint8_t WIDTH[MX_MC6845_REGS] = {
    0xFF, 0xFF, 0xFF, 0x0F, 0x7F, 0x1F, 0x7F, 0x7F, 0x03, 0x1F, 0x7F, 0x1F, 0x3F, 0xFF, 0x3F, 0xFF, 0x3F, 0xFF};

static uint8_t read_data(const mx_mc6845 *c) {
    return c->addr == 14 || c->addr == 15 || c->addr == 16 || c->addr == 17 ? c->reg[c->addr] : 0;
}

static void write_data(mx_mc6845 *c, uint8_t value) {
    const unsigned r = c->addr;
    if (r > 15) return;                         /* R16-R17 are read only, R18-R31 do not exist */
    sync(c, mx_dev_now(&c->dev));
    c->reg[r] = value & WIDTH[r];
    if (r <= 9) vsync_resched(c);               /* timing registers move the VSYNC edges */
}

static uint32_t crtc_read1(const mx_mc6845 *c, unsigned rs) { return rs ? read_data(c) : 0xFFu; }
static void crtc_write1(mx_mc6845 *c, unsigned rs, uint8_t v) {
    if (rs) write_data(c, v);
    else c->addr = v & 31u;
}

static uint32_t crtc_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    const mx_mc6845 *c = dev;
    if (space != MX_SPACE_IO) return size == 2 ? 0xFFFFu : 0xFFu;
    uint32_t v = crtc_read1(c, offset & 1u);
    if (size == 2) v |= crtc_read1(c, (offset + 1u) & 1u) << 8;
    return v;
}

static void crtc_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_mc6845 *c = dev;
    if (space != MX_SPACE_IO) return;
    crtc_write1(c, offset & 1u, (uint8_t)value);
    if (size == 2) crtc_write1(c, (offset + 1u) & 1u, (uint8_t)(value >> 8));
}

static void crtc_event(void *dev, uint64_t now) {
    mx_mc6845 *c = dev;
    sync(c, now);
    vsync_resched(c);
}

static void crtc_reset(void *dev) {
    mx_mc6845 *c = dev;
    memset(&c->beam, 0, sizeof c->beam);
    c->beam.t = mx_dev_now(&c->dev);
    c->beam.start = c->beam.row_start = start_address(c);
    c->lpstb = 0;
    if (c->vsync_enabled) {
        c->vsync_level = 0;
        mx_dev_line(&c->dev, MX_MC6845_PIN_VSYNC, 0);
        vsync_resched(c);
    }
}

static const mx_dev_ops CRTC_OPS = {crtc_read, crtc_write, crtc_event, crtc_reset, NULL, NULL, NULL};

/* --- public --------------------------------------------------------------------------- */

void mx_mc6845_init(mx_mc6845 *c, const mx_hw_host *host, const char *name) {
    memset(c, 0, sizeof *c);
    c->dev.ops = &CRTC_OPS;
    c->dev.host = host;
    c->dev.name = name ? name : "mc6845";
    c->period = 8;
    c->beam.t = host->now(host->ctx);
}

void mx_mc6845_set_period(mx_mc6845 *c, uint64_t now, uint64_t period) {
    if (period < 1) period = 1;
    if (period == c->period) return;
    sync(c, now);
    c->period = period;
    vsync_resched(c);
}

uint8_t mx_mc6845_get_reg(const mx_mc6845 *c, unsigned reg) { return reg < MX_MC6845_REGS ? c->reg[reg] : 0; }

void mx_mc6845_state_at(mx_mc6845 *c, uint64_t now, mx_mc6845_state *st) {
    sync(c, now);
    const mx_mc6845_beam *b = &c->beam;
    st->h = b->h;
    st->ra = b->ra;
    st->row = b->row;
    st->adj = b->adj;
    st->ma = beam_ma(b);
    st->line = line_index(c, b);
    st->frame = b->frame;
    st->hde = b->h < R(c, 1);
    st->vde = vde_at(c, b);
    st->de = st->hde && st->vde;
    st->hsync = b->h >= R(c, 2) && b->h < R(c, 2) + (R(c, 3) & 15u);
    st->vsync = vsync_at(c, st->line);
    st->cursor = st->de && st->ma == cursor_address(c) && cursor_scan(c, b->ra) && cursor_phase(c, b->frame);
}

void mx_mc6845_lpstb(mx_mc6845 *c, uint64_t now, int level) {
    level = level != 0;
    if (level && !c->lpstb) {
        sync(c, now);
        const uint16_t ma = beam_ma(&c->beam);
        c->reg[16] = (uint8_t)((ma >> 8) & 0x3Fu);
        c->reg[17] = (uint8_t)ma;
    }
    c->lpstb = (uint8_t)level;
}

void mx_mc6845_vsync_line(mx_mc6845 *c, int enable) {
    c->vsync_enabled = enable != 0;
    if (!enable) {
        if (c->vsync_level) mx_dev_line(&c->dev, MX_MC6845_PIN_VSYNC, 0);
        c->vsync_level = 0;
        mx_dev_schedule(&c->dev, MX_NEVER);
        return;
    }
    sync(c, mx_dev_now(&c->dev));
    vsync_resched(c);
}

uint64_t mx_mc6845_line_period(const mx_mc6845 *c) { return (R(c, 0) + 1u) * c->period; }
uint64_t mx_mc6845_frame_period(const mx_mc6845 *c) { return mx_mc6845_line_period(c) * frame_lines(c); }

/* --- line stepping -------------------------------------------------------------------- */

void mx_mc6845_line_refresh(const mx_mc6845 *c, mx_mc6845_line *ln) {
    const mx_mc6845_beam *b = &ln->beam;
    ln->start = b->t;
    ln->length = mx_mc6845_line_period(c);
    ln->index = line_index(c, b);
    ln->ma = b->row_start;
    ln->row = b->row;
    ln->ra = b->ra;
    ln->display = vde_at(c, b);
    ln->hdisp = ln->display ? (R(c, 1) < R(c, 0) + 1u ? R(c, 1) : R(c, 0) + 1u) : 0u;
    ln->vsync = vsync_at(c, ln->index);
    ln->cursor = ln->display && cursor_scan(c, b->ra) && cursor_phase(c, b->frame);
    ln->cursor_ma = cursor_address(c);
}

void mx_mc6845_line_locate(mx_mc6845 *c, uint64_t now, mx_mc6845_line *ln) {
    sync(c, now);
    ln->beam = c->beam;
    const uint64_t back = (uint64_t)ln->beam.h * c->period;
    ln->beam.t = ln->beam.t >= back ? ln->beam.t - back : 0;
    ln->beam.h = 0;
    mx_mc6845_line_refresh(c, ln);
}

void mx_mc6845_line_next(const mx_mc6845 *c, mx_mc6845_line *ln) {
    ln->beam.t += ln->length;
    end_line(c, &ln->beam);
    mx_mc6845_line_refresh(c, ln);
}

void mx_mc6845_line_frame_start(const mx_mc6845 *c, mx_mc6845_line *ln) {
    const uint64_t back = (uint64_t)ln->index * ln->length;
    ln->beam.t = ln->beam.t >= back ? ln->beam.t - back : 0;
    ln->beam.row = ln->beam.ra = ln->beam.adj = 0;
    ln->beam.row_start = ln->beam.start;
    mx_mc6845_line_refresh(c, ln);
}
