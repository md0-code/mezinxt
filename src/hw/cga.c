/* MezinXT - the Junior's CGA adapter. See cga.h for the board interface,
 * the registers, the store's addressing and the picture. MezinXT's own
 * code, written from the IBM Color/Graphics Monitor Adapter's documented
 * behaviour (modes, colour select, status, light pen ports) and the
 * Junior's schematic (a 32 KiB
 * store); the colour logic is checked against the adapter's mux-level
 * description in the MiSTer core's reference RTL (behaviour only). */
#include "cga.h"

#include <string.h>

#define W MX_CGA_FRAME_W
#define H MX_CGA_FRAME_H

/* Mode register bits. */
#define MODE_80      0x01u
#define MODE_GRAPHICS 0x02u
#define MODE_BW      0x04u
#define MODE_VIDEO   0x08u
#define MODE_HIRES   0x10u
#define MODE_BLINK   0x20u

static uint8_t *back_buffer(mx_cga *c) { return c->frames[c->front ^ 1u]; }

/* The overscan colour: the colour-select nibble, black in 640x200. */
static uint8_t border_colour(const mx_cga *c) {
    return (c->mode & (MODE_GRAPHICS | MODE_HIRES)) == (MODE_GRAPHICS | MODE_HIRES) ? 0u : (uint8_t)(c->colour & 15u);
}

/* --- drawing -------------------------------------------------------------------------- */

static void fill(uint8_t *p, uint32_t count, uint8_t value) { if (count) memset(p, value, count); }

/* A text cell from the character and attribute the latches hold. */
static void draw_cell(const mx_cga *c, const mx_mc6845_line *ln, uint64_t frame, unsigned ma, unsigned ch, unsigned at, uint8_t *d, uint32_t cw,
                      uint32_t clip) {
    const int blink_en = (c->mode & MODE_BLINK) != 0;
    const unsigned fg = at & 15u, bg = blink_en ? (at >> 4) & 7u : at >> 4;
    const int cursor = ln->cursor && ma == ln->cursor_ma;
    unsigned bits = c->glyph[ch * 8u + (ln->ra & 7u)];
    if (blink_en && (at & 0x80u) && !cursor && (frame & 16u)) bits = 0;     /* the hidden half of the blink */
    if (cursor && !(frame & 8u)) bits = 0xFFu;                              /* the cursor's visible half */
    for (uint32_t x = 0; x < clip; ++x) d[x] = (bits >> (7u - x * 8u / cw)) & 1u ? (uint8_t)fg : (uint8_t)bg;
}

static void draw_text(const mx_cga *c, const mx_mc6845_line *ln, uint64_t frame, unsigned ch_index, uint8_t *d, uint32_t cw, uint32_t clip) {
    const unsigned ma = (ln->ma + ch_index) & MX_MC6845_MA_MASK;
    const unsigned a = (ma << 1) & (MX_CGA_MEM_SIZE - 1u);
    draw_cell(c, ln, frame, ma, c->vram[a], c->vram[a | 1u], d, cw, clip);
}

static void draw_gfx4(const mx_cga *c, const mx_mc6845_line *ln, unsigned ch_index, uint8_t *d, uint32_t cw, uint32_t clip, const uint8_t *pal) {
    const unsigned ma = (ln->ma + ch_index) & MX_MC6845_MA_MASK;
    const unsigned a = ((ma & 0x0FFFu) << 1) | ((ln->ra & 1u) << 13) | (((ma >> 13) & 1u) << 14);
    const unsigned b[2] = {c->vram[a], c->vram[a | 1u]};
    for (uint32_t x = 0; x < clip; ++x) {
        const unsigned k = x * 8u / cw;                                     /* pixel 0-7 of the pair of bytes */
        d[x] = pal[(b[k >> 2] >> (6u - 2u * (k & 3u))) & 3u];
    }
}

static void draw_gfx2(const mx_cga *c, const mx_mc6845_line *ln, unsigned ch_index, uint8_t *d, uint32_t cw, uint32_t clip, uint8_t fg) {
    const unsigned ma = (ln->ma + ch_index) & MX_MC6845_MA_MASK;
    const unsigned a = ((ma & 0x0FFFu) << 1) | ((ln->ra & 1u) << 13) | (((ma >> 13) & 1u) << 14);
    const unsigned b[2] = {c->vram[a], c->vram[a | 1u]};
    for (uint32_t x = 0; x < clip; ++x) {
        const unsigned k = x * 16u / cw;                                    /* pixel 0-15 */
        d[x] = (b[k >> 3] >> (7u - (k & 7u))) & 1u ? fg : 0u;
    }
}

/* The line's displayed part from the store and the registers into `row`, the
 * rest of its row of the frame the border. */
static void render_row(const mx_cga *c, const mx_mc6845_line *ln, uint64_t frame, uint8_t *row) {
    const uint8_t border = border_colour(c);
    uint32_t x = 0;
    if (ln->display && (c->mode & MODE_VIDEO)) {
        const uint32_t cw = (uint32_t)c->crtc.period;
        uint8_t pal[4];
        pal[0] = (uint8_t)(c->colour & 15u);
        for (unsigned v = 1; v < 4; ++v) {
            static const uint8_t green[3] = {2, 4, 6}, pastel[3] = {3, 5, 7}, bw[3] = {3, 4, 7};
            const uint8_t base = (c->mode & MODE_BW) ? bw[v - 1] : (c->colour & 0x20u) ? pastel[v - 1] : green[v - 1];
            pal[v] = (uint8_t)(base | ((c->colour & 0x10u) ? 8u : 0u));
        }
        for (unsigned i = 0; i < ln->hdisp && x < W; ++i, x += cw) {
            const uint32_t clip = x + cw <= W ? cw : W - x;
            if (!(c->mode & MODE_GRAPHICS)) draw_text(c, ln, frame, i, row + x, cw, clip);
            else if (c->mode & MODE_HIRES) draw_gfx2(c, ln, i, row + x, cw, clip, (uint8_t)(c->colour & 15u));
            else draw_gfx4(c, ln, i, row + x, cw, clip, pal);
        }
    }
    if (x < W) fill(row + x, W - x, border);
}

/* 80-column text with the video on: both RAM cycles fetch for the picture; and the snow is not turned off. */
static int snow_mode(const mx_cga *c) { return c->snow_on && (c->mode & (MODE_80 | MODE_GRAPHICS | MODE_VIDEO)) == (MODE_80 | MODE_VIDEO); }

/* The cells of the line whose fetch the CPU took show what the latches took instead. */
static void draw_snow(mx_cga *c, const mx_mc6845_line *ln, uint64_t frame, uint8_t *row) {
    if (!ln->display || !snow_mode(c)) return;
    const uint32_t cw = (uint32_t)c->crtc.period;
    for (unsigned i = 0; i < c->snow_n; ++i) {
        const mx_cga_snow *s = &c->snow[i];
        const uint32_t x = (uint32_t)s->h * cw;
        if (s->h >= ln->hdisp || x >= W) continue;
        draw_cell(c, ln, frame, (ln->ma + s->h) & MX_MC6845_MA_MASK, s->ch, s->at, row + x, cw, x + cw <= W ? cw : W - x);
        c->snow_cells++;
    }
}

static void render_line(mx_cga *c, const mx_mc6845_line *ln, uint64_t frame) {
    /* Snow is on the line being scanned when it was taken: none is left once this line is drawn. */
    const int snow = c->snow_n && c->snow_line == ln->start;
    if (c->snow_n && c->snow_line < ln->start) c->snow_n = 0;
    if (ln->index >= H) { if (snow) c->snow_n = 0; return; }
    uint8_t *row = back_buffer(c) + (size_t)ln->index * W;
    render_row(c, ln, frame, row);
    if (snow) {
        draw_snow(c, ln, frame, row);
        c->snow_n = 0;
    }
    if (ln->index + 1u > c->rows_done) c->rows_done = ln->index + 1u;
}

/* The frame being drawn is complete: rows it never reached are border,
 * and it becomes the one shown if it differs from the one shown. */
static void finish_frame(mx_cga *c) {
    uint8_t *back = back_buffer(c);
    if (c->rows_done < H) fill(back + (size_t)c->rows_done * W, (H - c->rows_done) * W, border_colour(c));
    c->rows_done = 0;
    if (memcmp(back, c->frames[c->front], sizeof c->frames[0]) != 0) {
        c->front ^= 1u;
        c->serial++;
    }
}

/* Draws every scanline whose displayed part has been scanned by `now`. */
static void catch_up(mx_cga *c, uint64_t now) {
    mx_mc6845_line *ln = &c->line;
    /* The first time, or far behind: start at the top of the frame before
     * the present one, as the store and the registers say now (nothing
     * visited since they last changed). It completes at once, so the frame
     * returned is current however seldom the shell asks. */
    const uint64_t period = mx_mc6845_frame_period(&c->crtc);
    if (c->line_valid && now > ln->start + 2u * period) c->line_valid = c->draw_valid = 0;
    if (!c->line_valid) {
        mx_mc6845_line_locate(&c->crtc, now, ln);
        mx_mc6845_line_frame_start(&c->crtc, ln);
        if (ln->beam.frame > 0 && ln->start >= period) {
            ln->beam.t -= period;
            ln->beam.frame--;
            mx_mc6845_line_refresh(&c->crtc, ln);
        }
        c->line_valid = 1;
    }
    for (;;) {
        mx_mc6845_line_refresh(&c->crtc, ln);
        if (now < ln->start + (uint64_t)ln->hdisp * c->crtc.period) break;
        if (!c->draw_valid || ln->beam.frame != c->draw_frame) {
            c->draw_valid = 1;
            c->draw_frame = ln->beam.frame;
            c->rows_done = 0;
        }
        render_line(c, ln, ln->beam.frame);
        c->last_frame = ln->beam.frame;
        c->last_index = ln->index;
        mx_mc6845_line_next(&c->crtc, ln);
        if (ln->beam.frame != c->last_frame) finish_frame(c);
    }
}

/* The CRTC's timing changed at `now` (a register R0-R9, the character
 * clock): carry on from the scanline `now` is on; one already drawn is not
 * drawn twice. */
static void resync(mx_cga *c, uint64_t now) {
    mx_mc6845_line *ln = &c->line;
    if (!c->line_valid) return;                 /* nothing drawn yet: the first catch-up starts a frame */
    mx_mc6845_line_locate(&c->crtc, now, ln);
    if (ln->beam.frame == c->last_frame && ln->index == c->last_index) mx_mc6845_line_next(&c->crtc, ln);
}

/* --- light pen ------------------------------------------------------------------------ */

static void pen_set(mx_cga *c, uint64_t now) {
    if (!c->lp_latch) {
        c->lp_latch = 1;
        mx_mc6845_lpstb(&c->crtc, now, 1);
    }
}

static void pen_clear(mx_cga *c, uint64_t now) {
    c->lp_latch = 0;
    mx_mc6845_lpstb(&c->crtc, now, 0);
}

/* The next time after `now` that the beam is on the pen's pixel: the line's
 * start in the frame (the lines are all as long as R0 + 1 characters) and the
 * pixel's place on it (a pixel is one OSC period wide in every mode). */
static uint64_t pen_time(mx_cga *c, uint64_t now) {
    mx_mc6845_state st;
    mx_mc6845_state_at(&c->crtc, now, &st);
    const uint64_t line = mx_mc6845_line_period(&c->crtc), frame = mx_mc6845_frame_period(&c->crtc);
    const uint64_t spent = (uint64_t)st.line * line + (uint64_t)st.h * c->crtc.period;     /* into the frame, at the last character */
    const uint64_t start = c->crtc.beam.t >= spent ? c->crtc.beam.t - spent : 0;
    uint64_t t = start + (uint64_t)c->pen_y * line + c->pen_x;
    while (t <= now) t += frame;
    return t;
}

static void pen_arm(mx_cga *c, uint64_t now) {
    if (!c->pen_inside) { mx_dev_schedule(&c->dev, MX_NEVER); return; }
    mx_dev_schedule(&c->dev, pen_time(c, now));
}

void mx_cga_pen_place(mx_cga *c, int x, int y, int inside) {
    inside = inside && x >= 0 && y >= 0 && x < (int)W && y < (int)H;
    c->pen_inside = (uint8_t)inside;
    if (inside) { c->pen_x = (uint16_t)x; c->pen_y = (uint16_t)y; }
    pen_arm(c, mx_dev_now(&c->dev));
}

/* --- bus ------------------------------------------------------------------------------ */

static uint32_t io_read1(mx_cga *c, uint32_t offset, uint64_t now) {
    offset &= 15u;
    if (offset < 8u) return c->crtc.dev.ops->read(&c->crtc, MX_SPACE_IO, offset & 1u, 1);
    if (offset == 10u) {
        mx_mc6845_state st;
        mx_mc6845_state_at(&c->crtc, now, &st);
        return 0xF0u | (st.vsync ? 8u : 0u) | (c->pen_switch ? 0u : 4u) | (c->lp_latch ? 2u : 0u) | (st.de ? 0u : 1u);
    }
    return 0xFFu;
}

static void io_write1(mx_cga *c, uint32_t offset, uint8_t value, uint64_t now) {
    offset &= 15u;
    if (offset < 8u) {
        catch_up(c, now);                       /* what is due is drawn with the old registers */
        c->crtc.dev.ops->write(&c->crtc, MX_SPACE_IO, offset & 1u, 1, value);
        if ((offset & 1u) && c->crtc.addr <= 9u) { resync(c, now); pen_arm(c, now); }       /* the line timing moved */
        return;
    }
    switch (offset) {
    case 8:
        catch_up(c, now);
        c->mode = value & 0x3Fu;
        mx_mc6845_set_period(&c->crtc, now, (value & MODE_80) ? 8u : 16u);
        resync(c, now);
        pen_arm(c, now);
        break;
    case 9:
        catch_up(c, now);
        c->colour = value & 0x3Fu;
        break;
    case 11: pen_clear(c, now); break;
    case 12: pen_set(c, now); break;
    default: break;
    }
}

/* The store cycle running now took its slot as asked of ops->wait: the
 * time of its CAS (the bus cycle ends 3 to 18 periods after it), once. */
static int cycle_slot(mx_cga *c, int write, uint64_t now, uint64_t *slot) {
    const int in_slot = c->slot_valid && c->slot_write == write && c->slot <= now && now - c->slot < MX_CGA_CHAIN;
    c->slot_valid = 0;
    *slot = c->slot;
    return in_slot;
}

/* Snow: the CPU's cycle whose CAS is at `slot` took the fetch of the cell
 * the CRTC addressed when its address latch last closed (cga.h); the
 * latches take `ch` and `at` for it. Only the line being scanned can be
 * affected, and it is not drawn yet: the cell is noted for it. */
static void take_fetch(mx_cga *c, uint64_t slot, unsigned ch, unsigned at) {
    if (!snow_mode(c) || slot < MX_CGA_WRITE_OSC + MX_CGA_MA_LATCH_OSC) return;
    const uint64_t latch = slot - MX_CGA_WRITE_OSC - MX_CGA_MA_LATCH_OSC;
    catch_up(c, latch);
    const mx_mc6845_line *ln = &c->line;
    if (!c->line_valid || latch < ln->start || ln->index >= H) return;      /* in the blanking (or a line already drawn) */
    const uint64_t h = (latch - ln->start) / c->crtc.period;
    if (h >= ln->hdisp) return;                                             /* the border */
    if (c->snow_n && c->snow_line != ln->start) c->snow_n = 0;
    c->snow_line = ln->start;
    if (c->snow_n < MX_CGA_SNOW_MAX) c->snow[c->snow_n++] = (mx_cga_snow){(uint8_t)h, (uint8_t)ch, (uint8_t)at};
}

/* What the card's data bus carries in a memory cycle at `a` (a word, or a byte on the half its address says: A0 is
 * D8-15) and in an I/O cycle (D0-7): the half no gate opens keeps what it carried last (cga.h). Only the snow reads
 * it. */
static void hold_mem(mx_cga *c, uint32_t a, unsigned size, uint32_t value) {
    if (!c->snow_on) return;
    if (size == 2 && !(a & 1u)) {
        c->dbus[0] = (uint8_t)value;
        c->dbus[1] = (uint8_t)(value >> 8);
    } else {
        c->dbus[a & 1u] = (uint8_t)value;
    }
}

static void hold_io(mx_cga *c, uint32_t value) {
    if (c->snow_on) c->dbus[0] = (uint8_t)value;
}

static uint32_t cga_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    mx_cga *c = dev;
    if (space == MX_SPACE_MEM) {
        const uint32_t a = offset & (MX_CGA_MEM_SIZE - 1u);
        uint64_t slot;
        /* Both banks drive the RAM bus with the word at the CPU's address. */
        if (c->slot_valid && cycle_slot(c, 0, mx_dev_now(&c->dev), &slot) && c->snow_on) take_fetch(c, slot, c->vram[a & ~1u], c->vram[a | 1u]);
        const uint32_t v = size == 2 ? (uint32_t)(c->vram[a] | c->vram[(a + 1u) & (MX_CGA_MEM_SIZE - 1u)] << 8) : c->vram[a];
        hold_mem(c, a, size, v);                /* the read latches U2/U6 put the data on the bus's half or halves */
        return v;
    }
    const uint64_t now = mx_dev_now(&c->dev);
    uint32_t v = io_read1(c, offset, now);
    if (size == 2) v |= io_read1(c, offset + 1u, now) << 8;
    hold_io(c, size == 2 ? v >> 8 : v);                     /* the registers are on D0-7: the last byte cycle's */
    return v;
}

static void cga_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_cga *c = dev;
    const uint64_t now = mx_dev_now(&c->dev);
    if (space == MX_SPACE_MEM) {
        /* The CPU's write reached the store in its slot, before the bus
         * cycle ended (a write that did not wait, as now). */
        uint64_t slot;
        const int in_slot = cycle_slot(c, 1, now, &slot);
        const uint32_t a = offset & (MX_CGA_MEM_SIZE - 1u);
        if (in_slot && c->snow_on) {
            /* The RAM bus carries the CPU's data in the half or halves written (LS244 U3/U7 from the card's data
             * bus); the other half of the card's bus is driven by nothing (DGL or DGH off) and keeps the byte it
             * last carried. */
            const unsigned lo = (unsigned)value & 0xFFu, hi = ((unsigned)value >> 8) & 0xFFu;
            if (size == 2 && !(a & 1u)) take_fetch(c, slot, lo, hi);
            else if (a & 1u) take_fetch(c, slot, c->dbus[0], lo);
            else take_fetch(c, slot, lo, c->dbus[1]);
        }
        hold_mem(c, a, size, value);
        catch_up(c, in_slot ? slot : now);
        c->vram[a] = (uint8_t)value;
        if (size == 2) c->vram[(a + 1u) & (MX_CGA_MEM_SIZE - 1u)] = (uint8_t)(value >> 8);
        return;
    }
    io_write1(c, offset, (uint8_t)value, now);
    if (size == 2) io_write1(c, offset + 1u, (uint8_t)(value >> 8), now);
    hold_io(c, size == 2 ? value >> 8 : value);
}

/* The pen's pass: the beam is on its pixel now. It strobes when the picture is lit there (the pixel
 * as the store and the registers give it as the beam passes: the line is drawn into a row of its own),
 * and looks for its next pass a frame on. */
static void cga_event(void *dev, uint64_t now) {
    mx_cga *c = dev;
    if (!c->pen_inside) return;
    mx_mc6845_line ln;
    uint8_t row[W];
    mx_mc6845_line_locate(&c->crtc, now, &ln);
    render_row(c, &ln, ln.beam.frame, row);
    if (row[c->pen_x] & 15u) pen_set(c, now);
    pen_arm(c, now);
}

/* The timing chain's state at OSC time t: cleared while RESET is high, it
 * leaves state 0 on the second OSC edge after RESET falls and counts 16. */
static unsigned chain_state(const mx_cga *c, uint64_t t) {
    return t > c->chain_reset ? (unsigned)((t - c->chain_reset - 1u) % MX_CGA_CHAIN) : 0u;
}

/* A memory cycle whose command goes active at `now`: the first entry to
 * the sampling state that sees the select, then the release (cga.h). */
static uint32_t cga_wait(void *dev, unsigned space, uint32_t offset, int write, uint64_t now) {
    mx_cga *c = dev;
    (void)offset;
    if (space != MX_SPACE_MEM) return 0;
    const uint64_t t = now + MX_CGA_SELECT_OSC;
    const uint64_t sample = t + ((MX_CGA_SAMPLE_STATE + MX_CGA_CHAIN - chain_state(c, t)) % MX_CGA_CHAIN);
    c->slot = sample + MX_CGA_WRITE_OSC;
    c->slot_valid = 1;
    c->slot_write = write != 0;
    return (uint32_t)(sample + MX_CGA_RELEASE_OSC - now);
}

static void cga_reset(void *dev) {
    mx_cga *c = dev;
    c->chain_reset = mx_dev_now(&c->dev);
    c->slot_valid = 0;
    c->snow_n = 0;
    c->crtc.dev.ops->reset(&c->crtc);
    c->lp_latch = 0;
    c->line_valid = c->draw_valid = 0;
    c->rows_done = 0;
    pen_arm(c, mx_dev_now(&c->dev));
}

static void cga_input(void *dev, int pin, int state) {
    mx_cga *c = dev;
    if (pin == MX_CGA_PIN_PEN) {
        if (state) pen_set(c, mx_dev_now(&c->dev));
    } else if (pin == MX_CGA_PIN_PEN_SWITCH) {
        c->pen_switch = state != 0;
    }
}

static const mx_dev_ops CGA_OPS = {cga_read, cga_write, cga_event, cga_reset, cga_input, cga_wait, NULL};

/* --- public --------------------------------------------------------------------------- */

void mx_cga_init(mx_cga *c, const mx_hw_host *host, const char *name, const uint8_t *glyphs) {
    memset(c, 0, sizeof *c);
    c->snow_on = 1;
    c->dbus[0] = c->dbus[1] = 0xFFu;            /* no cycle yet: an open input reads high */
    c->dev.ops = &CGA_OPS;
    c->dev.host = host;
    c->dev.name = name ? name : "cga";
    if (glyphs) memcpy(c->glyph, glyphs, sizeof c->glyph);
    mx_mc6845_init(&c->crtc, host, "mc6845");
    mx_mc6845_set_period(&c->crtc, host->now(host->ctx), 16);           /* mode 0: not 80 columns */
}

void mx_cga_set_snow(mx_cga *c, int on) {
    c->snow_on = on != 0;
    c->snow_n = 0;
}

const uint8_t *mx_cga_frame(mx_cga *c, uint64_t *serial) {
    catch_up(c, mx_dev_now(&c->dev));
    if (serial) *serial = c->serial;
    return c->frames[c->front];
}

uint64_t mx_cga_frame_period(const mx_cga *c) { return mx_mc6845_frame_period(&c->crtc); }
