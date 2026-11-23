/* MezinXT - the monochrome display coupler. See mda.h for the board interface,
 * the registers, the timing and the picture. MezinXT's own code, written from
 * the IBM Monochrome Display and Printer Adapter's documented behaviour (the
 * 6845's 9-dot cells, the attribute decoding, the 4 KiB store, the status bits
 * the POST tests) and the MC6845 data sheet; the card's rendering follows the
 * CGA's (cga.c) line by line. */
#include "mda.h"

#include <string.h>

#define W MX_MDA_FRAME_W
#define H MX_MDA_FRAME_H
#define CW MX_MDA_CELL_W
#define CH MX_MDA_CELL_H

/* Mode control bits. */
#define MODE_HIRES 0x01u
#define MODE_VIDEO 0x08u
#define MODE_BLINK 0x20u

/* The scan line of the underline. */
#define UNDERLINE_LINE 12u

/* --- time: the card's own crystal ------------------------------------------------------- */

uint64_t mx_mda_osc_to_dots(uint64_t osc) {
    return osc / MX_MDA_DOT_DEN * MX_MDA_DOT_NUM + osc % MX_MDA_DOT_DEN * MX_MDA_DOT_NUM / MX_MDA_DOT_DEN;
}

uint64_t mx_mda_dots_to_osc(uint64_t dots) {
    return dots / MX_MDA_DOT_NUM * MX_MDA_DOT_DEN + (dots % MX_MDA_DOT_NUM * MX_MDA_DOT_DEN + MX_MDA_DOT_NUM - 1u) / MX_MDA_DOT_NUM;
}

/* The CRTC's host: its clock is the dot clock, derived from the board's now(). It has no pins and no deadlines. */
static uint64_t dots_now(void *ctx) {
    const mx_mda *m = ctx;
    return mx_mda_osc_to_dots(m->dev.host->now(m->dev.host->ctx));
}
static void dots_schedule(void *ctx, void *dev, uint64_t when) { (void)ctx; (void)dev; (void)when; }
static void dots_line(void *ctx, void *dev, int pin, int state) { (void)ctx; (void)dev; (void)pin; (void)state; }

/* --- the font ------------------------------------------------------------------------------ */

int mx_mda_font_row(unsigned ch, unsigned line) {
    static const signed char text[CH] = {-1, 0, 0, 1, 2, 2, 3, 4, 4, 5, 6, 6, 7, 7};
    if (line >= CH) return -1;
    if (ch >= 0xB0u && ch <= 0xDFu) return (int)((2u * line + 1u) * 2u / 7u);
    return text[line];
}

void mx_mda_font_build(uint8_t *out, const uint8_t *glyphs8) {
    for (unsigned c = 0; c < 256u; ++c)
        for (unsigned line = 0; line < CH; ++line) {
            const int row = mx_mda_font_row(c, line);
            out[c * CH + line] = row < 0 || !glyphs8 ? 0u : glyphs8[c * 8u + (unsigned)row];
        }
}

/* --- drawing ------------------------------------------------------------------------------- */

static uint8_t *back_buffer(mx_mda *m) { return m->frames[m->front ^ 1u]; }

/* The nine dots of the cell for character `ch` and attribute `at` on scan line `ra`, as levels 0 to 2. */
static void cell_dots(const mx_mda *m, unsigned ch, unsigned at, unsigned ra, int cursor, uint64_t frame, uint8_t out[9]) {
    const int reverse = (at & 0x70u) == 0x70u;
    const int shown = (at & 7u) != 0;
    const int underline = (at & 7u) == 1u && ra == UNDERLINE_LINE;
    const int hidden = (m->mode & MODE_BLINK) && (at & 0x80u) && (frame & 16u);
    const unsigned bits = ra < CH ? m->glyph[ch * CH + ra] : 0u;
    const unsigned level = (at & 8u) ? MX_MDA_INTENSE : MX_MDA_NORMAL;
    for (unsigned k = 0; k < 9u; ++k) {
        /* The ninth dot is blank, but for the line-drawing characters, which repeat the eighth. */
        const unsigned dot = k < 8u ? (bits >> (7u - k)) & 1u : (ch >= 0xC0u && ch <= 0xDFu ? bits & 1u : 0u);
        const unsigned ink = !hidden && (dot || underline);
        const unsigned lit = reverse ? !ink : (shown && ink);
        out[k] = (uint8_t)(cursor || lit ? level : MX_MDA_DARK);
    }
}

static int video_on(const mx_mda *m) { return (m->mode & (MODE_HIRES | MODE_VIDEO)) == (MODE_HIRES | MODE_VIDEO); }

/* The line's displayed part from the store and the registers into `row`, the rest of the row dark. */
static void render_row(const mx_mda *m, const mx_mc6845_line *ln, uint64_t frame, uint8_t *row) {
    uint32_t x = 0;
    if (ln->display && video_on(m)) {
        for (unsigned i = 0; i < ln->hdisp && x < W; ++i, x += CW) {
            const unsigned ma = (ln->ma + i) & MX_MC6845_MA_MASK;
            const unsigned a = (ma << 1) & (MX_MDA_VRAM_SIZE - 1u);
            const int cursor = ln->cursor && ma == ln->cursor_ma && !(frame & 8u);
            uint8_t dots[9];
            cell_dots(m, m->vram[a], m->vram[a | 1u], ln->ra, cursor, frame, dots);
            const uint32_t n = x + CW <= W ? CW : W - x;
            memcpy(row + x, dots, n);
        }
    }
    if (x < W) memset(row + x, 0, W - x);
}

static void render_line(mx_mda *m, const mx_mc6845_line *ln, uint64_t frame) {
    if (ln->index >= H) return;
    render_row(m, ln, frame, back_buffer(m) + (size_t)ln->index * W);
    if (ln->index + 1u > m->rows_done) m->rows_done = ln->index + 1u;
}

/* The frame being drawn is complete: rows it never reached are dark, and it becomes the one shown if it differs
 * from the one shown. */
static void finish_frame(mx_mda *m) {
    uint8_t *back = back_buffer(m);
    if (m->rows_done < H) memset(back + (size_t)m->rows_done * W, 0, (H - m->rows_done) * W);
    m->rows_done = 0;
    if (memcmp(back, m->frames[m->front], sizeof m->frames[0]) != 0) {
        m->front ^= 1u;
        m->serial++;
    }
}

/* Draws every scanline whose displayed part has been scanned by `now` (dot-clock time). */
static void catch_up(mx_mda *m, uint64_t now) {
    mx_mc6845_line *ln = &m->line;
    const uint64_t period = mx_mc6845_frame_period(&m->crtc);
    /* The first time, or far behind: start at the top of the frame before the present one, as the store and the
     * registers say now. It completes at once, so the frame returned is current however seldom the shell asks. */
    if (m->line_valid && now > ln->start + 2u * period) m->line_valid = m->draw_valid = 0;
    if (!m->line_valid) {
        mx_mc6845_line_locate(&m->crtc, now, ln);
        mx_mc6845_line_frame_start(&m->crtc, ln);
        if (ln->beam.frame > 0 && ln->start >= period) {
            ln->beam.t -= period;
            ln->beam.frame--;
            mx_mc6845_line_refresh(&m->crtc, ln);
        }
        m->line_valid = 1;
    }
    for (;;) {
        mx_mc6845_line_refresh(&m->crtc, ln);
        if (now < ln->start + (uint64_t)ln->hdisp * m->crtc.period) break;
        if (!m->draw_valid || ln->beam.frame != m->draw_frame) {
            m->draw_valid = 1;
            m->draw_frame = ln->beam.frame;
            m->rows_done = 0;
        }
        render_line(m, ln, ln->beam.frame);
        m->last_frame = ln->beam.frame;
        m->last_index = ln->index;
        mx_mc6845_line_next(&m->crtc, ln);
        if (ln->beam.frame != m->last_frame) finish_frame(m);
    }
}

/* The CRTC's timing changed at `now`: carry on from the scanline `now` is on; one already drawn is not drawn twice. */
static void resync(mx_mda *m, uint64_t now) {
    mx_mc6845_line *ln = &m->line;
    if (!m->line_valid) return;
    mx_mc6845_line_locate(&m->crtc, now, ln);
    if (ln->beam.frame == m->last_frame && ln->index == m->last_index) mx_mc6845_line_next(&m->crtc, ln);
}

/* --- bus ----------------------------------------------------------------------------------- */

/* Status: bit 0 the CRTC's HSYNC, bit 3 the video signal at the dot the beam is on; bits 7-4 read 1, the rest 0. */
static uint32_t status(mx_mda *m, uint64_t now) {
    mx_mc6845_state st;
    mx_mc6845_state_at(&m->crtc, now, &st);
    uint32_t v = 0xF0u | (st.hsync ? 1u : 0u);
    if (st.de && video_on(m) && now >= m->crtc.beam.t) {
        const uint64_t dot = now - m->crtc.beam.t;                      /* the beam is at the start of this character */
        const unsigned a = (st.ma << 1) & (MX_MDA_VRAM_SIZE - 1u);
        uint8_t dots[9];
        cell_dots(m, m->vram[a], m->vram[a | 1u], st.ra, st.cursor && !(st.frame & 8u), st.frame, dots);
        if (dot < 9u && dots[dot]) v |= 8u;
    }
    return v;
}

static uint32_t io_read1(mx_mda *m, uint32_t offset, uint64_t now) {
    if (offset < 8u) return m->crtc.dev.ops->read(&m->crtc, MX_SPACE_IO, offset & 1u, 1);
    if (offset == 10u) return status(m, now);
    return 0xFFu;
}

static void io_write1(mx_mda *m, uint32_t offset, uint8_t value, uint64_t now) {
    if (offset < 8u) {
        catch_up(m, now);                       /* what is due is drawn with the old registers */
        m->crtc.dev.ops->write(&m->crtc, MX_SPACE_IO, offset & 1u, 1, value);
        if ((offset & 1u) && m->crtc.addr <= 9u) resync(m, now);        /* the line timing moved */
    } else if (offset == 8u) {
        catch_up(m, now);
        m->mode = value & 0x29u;
    }
}

static uint32_t mda_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    mx_mda *m = dev;
    if (space == MX_SPACE_MEM) {
        const uint32_t a = offset & (MX_MDA_VRAM_SIZE - 1u);
        if (size == 2) return (uint32_t)(m->vram[a] | m->vram[(a + 1u) & (MX_MDA_VRAM_SIZE - 1u)] << 8);
        return m->vram[a];
    }
    const uint64_t now = dots_now(m);
    uint32_t v = io_read1(m, offset, now);
    if (size == 2) v |= io_read1(m, offset + 1u, now) << 8;
    return v;
}

static void mda_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_mda *m = dev;
    const uint64_t now = dots_now(m);
    if (space == MX_SPACE_MEM) {
        catch_up(m, now);
        const uint32_t a = offset & (MX_MDA_VRAM_SIZE - 1u);
        m->vram[a] = (uint8_t)value;
        if (size == 2) m->vram[(a + 1u) & (MX_MDA_VRAM_SIZE - 1u)] = (uint8_t)(value >> 8);
        return;
    }
    io_write1(m, offset, (uint8_t)value, now);
    if (size == 2) io_write1(m, offset + 1u, (uint8_t)(value >> 8), now);
}

static void mda_event(void *dev, uint64_t now) { (void)dev; (void)now; }

static void mda_reset(void *dev) {
    mx_mda *m = dev;
    m->crtc.dev.ops->reset(&m->crtc);
    m->line_valid = m->draw_valid = 0;
    m->rows_done = 0;
}

static const mx_dev_ops MDA_OPS = {mda_read, mda_write, mda_event, mda_reset, NULL, NULL, NULL};

/* --- public -------------------------------------------------------------------------------- */

void mx_mda_init(mx_mda *m, const mx_hw_host *host, const char *name, const uint8_t *glyphs) {
    memset(m, 0, sizeof *m);
    m->dev.ops = &MDA_OPS;
    m->dev.host = host;
    m->dev.name = name ? name : "mda";
    m->dots.ctx = m;
    m->dots.now = dots_now;
    m->dots.schedule = dots_schedule;
    m->dots.line = dots_line;
    if (glyphs) memcpy(m->glyph, glyphs, sizeof m->glyph);
    mx_mc6845_init(&m->crtc, &m->dots, "mc6845");
    mx_mc6845_set_period(&m->crtc, dots_now(m), CW);
}

const uint8_t *mx_mda_frame(mx_mda *m, uint64_t *serial) {
    catch_up(m, dots_now(m));
    if (serial) *serial = m->serial;
    return m->frames[m->front];
}

uint64_t mx_mda_frame_period(const mx_mda *m) { return mx_mda_dots_to_osc(mx_mc6845_frame_period(&m->crtc)); }
