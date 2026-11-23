/* MezinXT - the serial adapter (sio_card.h). Portable C11. MezinXT's own
 * code, from the Junior's manual (3.4) and `junior_serial.sv`. */
#include "sio_card.h"

#include <string.h>

/* The 8253's clock: 1.8432 MHz as a ratio of OSC periods per edge. */
#define BAUD_NUM 21875u
#define BAUD_DEN 2816u

/* --- what the chips see of the board ---------------------------------------------------------- */

static uint64_t sh_now(void *ctx) {
    const mx_sio_card *c = ctx;
    return c->live ? mx_dev_now(&c->dev) : 0;
}

static void sh_schedule(void *ctx, void *dev, uint64_t when) {
    mx_sio_card *c = ctx;
    (void)dev;
    mx_dev_schedule(&c->dev, when);
}

static void sh_line(void *ctx, void *dev, int pin, int state) {
    mx_sio_card *c = ctx;
    (void)dev;
    const uint8_t v = state != 0;
    if (pin == MX_Z80SIO_OUT_INT) {
        mx_dev_line(&c->dev, MX_SIO_CARD_IRQ, v);
        return;
    }
    const unsigned ch = (unsigned)(pin - 1) & 1u;
    if (pin == (int)MX_Z80SIO_OUT_RTS(ch)) c->rts[ch] = v;
    else c->dtr[ch] = v;
    mx_dev_line(&c->dev, pin, v);
    if (c->end[ch].modem) c->end[ch].modem(c->end[ch].ctx, ch, c->rts[ch], c->dtr[ch]);
}

/* The 8253 never reports or schedules: its counters are read on demand and
 * the card derives the SIO's clocks from their programming. */
static void ph_schedule(void *ctx, void *dev, uint64_t when) { (void)ctx; (void)dev; (void)when; }
static void ph_line(void *ctx, void *dev, int pin, int state) { (void)ctx; (void)dev; (void)pin; (void)state; }

/* --- baud clocks ------------------------------------------------------------------------------- */

static uint32_t counter_value(const mx_i8253_counter *k) {
    uint32_t v = k->cr;
    if (k->bcd) {
        v = (v >> 12 & 15u) * 1000u + (v >> 8 & 15u) * 100u + (v >> 4 & 15u) * 10u + (v & 15u);
        return v ? v : 10000u;
    }
    return v ? v : 65536u;
}

/* After anything that may have changed counters 0 and 1: a counter in mode 3
 * or 2 with a loaded count of N >= 2 clocks its channel with one edge per N
 * 8253 clocks. A first load starts the cycle at the first 8253 edge after now
 * (the chip's load edge), the first SIO edge being one cycle on (OUT rises at
 * the end of the first cycle); a new count of a running counter takes over at
 * the end of the cycle running. */
static void retune(mx_sio_card *c) {
    const mx_z80sio_clock grid = {BAUD_NUM, BAUD_DEN, 0};
    const uint64_t now = mx_dev_now(&c->dev);
    for (unsigned n = 0; n < 2; n++) {
        const mx_i8253_counter *k = &c->pit.c[n];
        uint32_t count = 0;
        int run = 0;
        if ((k->mode == 2 || k->mode == 3) && (k->loaded || k->pending)) {
            count = counter_value(k);
            run = count >= 2;
        }
        if (!run) {
            if (c->pit_run[n]) {
                c->pit_run[n] = 0;
                mx_z80sio_set_clock(&c->sio, n, 3, NULL);
            }
            continue;
        }
        if (c->pit_run[n] && c->pit_n[n] == count) continue;
        uint64_t k0, first;
        const uint64_t next = mx_z80sio_edge_after(&grid, now, 1);
        if (c->pit_run[n] && next > c->pit_k0[n]) {
            const uint64_t m = (next - c->pit_k0[n] + c->pit_n[n] - 1u) / c->pit_n[n];
            k0 = c->pit_k0[n] + m * c->pit_n[n];
            first = k0;
        } else {
            k0 = next;
            first = k0 + count;
        }
        c->pit_run[n] = 1;
        c->pit_n[n] = count;
        c->pit_k0[n] = k0;
        const mx_z80sio_clock clk = {(uint64_t)BAUD_NUM * count, BAUD_DEN, first * BAUD_NUM};
        mx_z80sio_set_clock(&c->sio, n, 3, &clk);
    }
}

/* --- byte ends --------------------------------------------------------------------------------- */

/* Collects bytes from the end of a channel while the receiver can take them. */
static void pump(mx_sio_card *c, unsigned ch) {
    if (c->pumping[ch] || c->loop[ch] || !c->end[ch].rx) return;
    c->pumping[ch] = 1;
    while (mx_z80sio_rx_ready(&c->sio, ch) && (!c->flow || mx_z80sio_rx_count(&c->sio, ch) < 3)) {
        uint8_t b = 0;
        if (!c->end[ch].rx(c->end[ch].ctx, ch, &b)) break;
        mx_z80sio_frame f;
        mx_z80sio_make_frame(&c->sio, ch, b, mx_dev_now(&c->dev), &f);
        if (!mx_z80sio_rx_frame(&c->sio, ch, &f)) break;
    }
    c->pumping[ch] = 0;
}

static void lk_tx_frame(void *ctx, unsigned ch, const mx_z80sio_frame *f) {
    mx_sio_card *c = ctx;
    if (c->loop[ch]) {
        (void)mx_z80sio_rx_frame(&c->sio, ch, f);       /* a busy or disabled receiver loses it, as on a wire */
        return;
    }
    if (c->end[ch].tx) c->end[ch].tx(c->end[ch].ctx, ch, f->data, f->nbits);
}

static void lk_tx_break(void *ctx, unsigned ch, int on) {
    mx_sio_card *c = ctx;
    if (c->loop[ch]) mx_z80sio_rx_break(&c->sio, ch, on);
    else if (c->end[ch].tx_break) c->end[ch].tx_break(c->end[ch].ctx, ch, on);
}

static void lk_rx_ready(void *ctx, unsigned ch) {
    pump(ctx, ch);
}

/* --- the register window ----------------------------------------------------------------------- */

static uint32_t card_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    mx_sio_card *c = dev;
    (void)size;
    offset &= 15u;
    switch (offset >> 2) {
    case 0: {
        const uint32_t v = c->sio.dev.ops->read(&c->sio, space, offset & 3u, 1);
        if (!(offset & 1u)) pump(c, offset >> 1);       /* a character left the FIFO */
        return v;
    }
    case 1:
        return c->pit.dev.ops->read(&c->pit, space, offset & 3u, 1);
    case 2:
        return mx_z80sio_inta(&c->sio);
    default:
        return c->dsr[0] ? 1u : 0u;
    }
}

static void card_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_sio_card *c = dev;
    (void)size;
    offset &= 15u;
    switch (offset >> 2) {
    case 0:
        c->sio.dev.ops->write(&c->sio, space, offset & 3u, 1, value);
        break;
    case 1:
        c->pit.dev.ops->write(&c->pit, space, offset & 3u, 1, value);
        retune(c);
        break;
    default:                                            /* the acknowledge and status ports take no writes */
        break;
    }
}

static void card_event(void *dev, uint64_t now) {
    mx_sio_card *c = dev;
    c->sio.dev.ops->event(&c->sio, now);
}

static void card_reset(void *dev) {
    mx_sio_card *c = dev;
    c->live = 1;
    c->pit.dev.ops->reset(&c->pit);
    c->pit_run[0] = c->pit_run[1] = 0;
    mx_z80sio_set_clock(&c->sio, 0, 3, NULL);
    mx_z80sio_set_clock(&c->sio, 1, 3, NULL);
    c->sio.dev.ops->reset(&c->sio);
}

static void card_input(void *dev, int pin, int state) {
    mx_sio_card *c = dev;
    if (pin < 0 || pin > 5) return;
    if (pin >= MX_SIO_CARD_DSR(0)) {
        c->dsr[pin - MX_SIO_CARD_DSR(0)] = state != 0;
        return;
    }
    c->sio.dev.ops->input(&c->sio, pin, state);         /* the numbering is the SIO's CTS 0-1, DCD 2-3 */
    pump(c, (unsigned)pin & 1u);                        /* DCD may enable the receiver */
}

static uint32_t card_wait(void *dev, unsigned space, uint32_t offset, int write, uint64_t now) {
    (void)dev; (void)space; (void)offset; (void)write; (void)now;
    return MX_SIO_CARD_WAIT_OSC;
}

static const mx_dev_ops CARD_OPS = {card_read, card_write, card_event, card_reset, card_input, card_wait, NULL};

void mx_sio_card_init(mx_sio_card *c, const mx_hw_host *host, const char *name) {
    memset(c, 0, sizeof *c);
    c->dev.ops = &CARD_OPS;
    c->dev.host = host;
    c->dev.name = name;
    c->flow = 1;
    c->sio_host.ctx = c;
    c->sio_host.now = sh_now;
    c->sio_host.schedule = sh_schedule;
    c->sio_host.line = sh_line;
    c->sio_host.dma = NULL;
    c->pit_host = c->sio_host;
    c->pit_host.schedule = ph_schedule;
    c->pit_host.line = ph_line;
    mx_z80sio_init(&c->sio, &c->sio_host, "z80sio");
    c->sio.overrun_keeps = 1;                           /* the RTL: an overrun loses the new character */
    const mx_z80sio_link link = {c, lk_tx_frame, lk_tx_break, lk_rx_ready};
    mx_z80sio_set_link(&c->sio, &link);
    mx_i8253_init(&c->pit, &c->pit_host, BAUD_NUM, BAUD_DEN);
    mx_i8253_watch(&c->pit, 0);
}

void mx_sio_card_set_end(mx_sio_card *c, unsigned channel, const mx_sio_end *end) {
    if (channel > 1) return;
    if (end) c->end[channel] = *end;
    else memset(&c->end[channel], 0, sizeof c->end[channel]);
    pump(c, channel);
}

void mx_sio_card_set_loopback(mx_sio_card *c, unsigned channel, int on) {
    if (channel > 1) return;
    c->loop[channel] = on != 0;
    if (!on) pump(c, channel);
}

void mx_sio_card_set_flow_control(mx_sio_card *c, int on) {
    c->flow = on != 0;
}

void mx_sio_card_rx_notify(mx_sio_card *c, unsigned channel) {
    if (channel < 2) pump(c, channel);
}

void mx_sio_card_rx_break(mx_sio_card *c, unsigned channel, int on) {
    if (channel < 2) mx_z80sio_rx_break(&c->sio, channel, on);
}

void mx_sio_card_set_modem(mx_sio_card *c, unsigned channel, int cts, int dcd, int dsr) {
    if (channel > 1) return;
    card_input(c, MX_SIO_CARD_CTS(channel), cts);
    card_input(c, MX_SIO_CARD_DCD(channel), dcd);
    card_input(c, MX_SIO_CARD_DSR(channel), dsr);
}
