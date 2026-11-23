/* MezinXT - Zilog Z80-SIO (z80sio.h). Portable C11. MezinXT's own code,
 * written from the Z80-SIO data sheet and matched to the Junior's adapter
 * (junior_serial.sv). */
#include "z80sio.h"

#include <string.h>

enum { ERR_PARITY = 1, ERR_FRAMING = 2 };
enum { RX_IDLE, RX_FRAME, RX_RECOVER };

/* --- small helpers ------------------------------------------------------------------------------ */

static unsigned divisor(const mx_z80sio_chan *c) {
    static const uint8_t d[4] = {1, 16, 32, 64};
    return d[c->wr[4] >> 6];
}

/* 5, 7, 6 or 8 bits for the register encodings 0-3. */
static unsigned char_bits(unsigned v) {
    static const uint8_t b[4] = {5, 7, 6, 8};
    return b[v & 3u];
}

/* Receiver enable (WR3 bit 0) and, with auto enables (WR3 bit 5), DCD. */
static int rx_enabled(const mx_z80sio_chan *c) {
    return (c->wr[3] & 1u) && (!(c->wr[3] & 0x20u) || c->in_dcd);
}

/* Transmitter enable (WR5 bit 3) and, with auto enables, CTS. */
static int tx_enabled(const mx_z80sio_chan *c) {
    return (c->wr[5] & 8u) && (!(c->wr[3] & 0x20u) || c->in_cts);
}

static uint8_t ext_inputs(const mx_z80sio_chan *c) {
    return (uint8_t)((c->in_cts ? MX_Z80SIO_RR0_CTS : 0) | (c->in_dcd ? MX_Z80SIO_RR0_DCD : 0) |
                     (c->in_sync ? MX_Z80SIO_RR0_SYNC : 0) | (c->rx_in_break ? MX_Z80SIO_RR0_BREAK : 0));
}

/* An external/status source changed: with the interrupt enabled the new
 * state is latched (frozen in RR0) and the interrupt pending until the
 * reset external/status command; a second change while latched is not seen. */
static void ext_refresh(mx_z80sio_chan *c) {
    const uint8_t now = ext_inputs(c);
    if (now == c->ext_live) return;
    c->ext_live = now;
    if ((c->wr[1] & 1u) && !c->ext_latched) {
        c->ext_latched = 1;
        c->ext_snap = now;
        c->ext_ip = 1;
    }
}

static void chan_reset(mx_z80sio_chan *c) {
    const uint8_t cts = c->in_cts, dcd = c->in_dcd, sync = c->in_sync, hold = c->rx_hold, rts = c->rts_out, dtr = c->dtr_out;
    const mx_z80sio_clock rxc = c->rx_clk, txc = c->tx_clk;
    memset(c, 0, sizeof *c);
    c->rts_out = rts;                           /* the pins as last reported: update_outputs brings them down */
    c->dtr_out = dtr;
    c->in_cts = cts;
    c->in_dcd = dcd;
    c->in_sync = sync;
    c->rx_hold = hold;
    c->rx_clk = rxc;
    c->tx_clk = txc;
    c->rx_armed = 1;
    c->rx_done_at = c->rx_free_at = c->tx_start_at = c->tx_end_at = MX_NEVER;
    c->ext_live = ext_inputs(c);
}

/* --- interrupt logic ------------------------------------------------------------------------------ */

/* The character at the head of the FIFO carries a special receive condition:
 * overrun, framing error, and parity error only in the "parity affects
 * vector" mode (the data sheet: not in first-character mode after the first). */
static int rx_special(const mx_z80sio_chan *c) {
    if (!c->used) return 0;
    if (c->overrun) return 1;
    const uint8_t e = c->fifo_err[c->head];
    if (e & ERR_FRAMING) return 1;
    return (e & ERR_PARITY) && ((c->wr[1] >> 3) & 3u) == 2u;
}

static int rx_pending(const mx_z80sio_chan *c) {
    const unsigned mode = (c->wr[1] >> 3) & 3u;
    if (mode == 0 || !c->used) return 0;
    return mode >= 2 || c->rx_armed || rx_special(c);
}

/* Interrupt sources in priority order: bit 0 receive A, 1 transmit A,
 * 2 external A, 3 receive B, 4 transmit B, 5 external B. */
static unsigned pending_mask(const mx_z80sio *s) {
    unsigned m = 0;
    for (unsigned i = 0; i < 2; i++) {
        const mx_z80sio_chan *c = &s->ch[i];
        if (rx_pending(c)) m |= 1u << (3 * i);
        if ((c->wr[1] & 2u) && c->tx_ip) m |= 1u << (3 * i + 1);
        if ((c->wr[1] & 1u) && c->ext_ip) m |= 1u << (3 * i + 2);
    }
    return m;
}

/* The level an acknowledge would serve: the highest source that is pending
 * and not blocked by an equal or higher one under service; -1 for none. */
static int winner(const mx_z80sio *s) {
    const unsigned p = pending_mask(s);
    for (unsigned j = 0; j < 6; j++) {
        if (s->in_service >> j & 1u) return -1;
        if (p >> j & 1u) return (int)j;
    }
    return -1;
}

static uint8_t vector_for(const mx_z80sio *s, int w) {
    const uint8_t v = s->ch[1].wr[2];
    if (!(s->ch[1].wr[1] & 4u)) return v;
    unsigned code;
    switch (w) {
    case 0: code = rx_special(&s->ch[0]) ? 7u : 6u; break;
    case 1: code = 4; break;
    case 2: code = 5; break;
    case 3: code = rx_special(&s->ch[1]) ? 3u : 2u; break;
    case 4: code = 0; break;
    case 5: code = 1; break;
    default: code = 3; break;                   /* nothing pending: the special receive code of B */
    }
    return (uint8_t)((v & 0xF1u) | code << 1);
}

/* --- receiver -------------------------------------------------------------------------------------- */

static int rx_ready_now(const mx_z80sio_chan *c) {
    return c->rx_state == RX_IDLE && rx_enabled(c) && !c->rx_in_break && !c->rx_hold && c->rx_clk.num != 0;
}

static void rx_abort(mx_z80sio_chan *c) {
    c->rx_state = RX_IDLE;
    c->rx_done_at = c->rx_free_at = MX_NEVER;
}

/* The line level a frame has at OSC time t (idle after its last bit). */
static unsigned frame_bit(const mx_z80sio_frame *f, uint64_t t) {
    if (t < f->start) return 1;
    const uint64_t i = (t - f->start) * f->bit_den / f->bit_num;
    if (i >= f->length) return 1;
    return (f->bits >> i) & 1u;
}

/* Starts receiving `f`: the first clock edge at or after its start finds the
 * low start bit; the middle of the start bit and of every later bit is
 * divisor/2 + n * divisor edges on. */
static void rx_accept(mx_z80sio *s, unsigned chn, const mx_z80sio_frame *f) {
    mx_z80sio_chan *c = &s->ch[chn];
    const uint64_t now = mx_dev_now(&s->dev);
    const uint64_t t = f->start > now ? f->start : now;
    const unsigned d = divisor(c), h = d > 1 ? d >> 1 : 0;
    c->rx_nb = (uint8_t)char_bits(c->wr[3] >> 6);
    c->rx_par = c->wr[4] & 1u;
    c->rx_d = (uint8_t)d;
    c->rx_frame = *f;
    c->rx_snap = c->rx_clk;
    c->rx_k0 = mx_z80sio_edge_after(&c->rx_clk, t, 0);
    c->rx_state = RX_FRAME;
    c->rx_done_at = mx_z80sio_edge_time(&c->rx_snap, c->rx_k0 + h + (uint64_t)(1u + c->rx_nb + c->rx_par) * d);
}

static void rx_push(const mx_z80sio *s, mx_z80sio_chan *c, uint8_t data, uint8_t err) {
    if (c->used < 3) {
        const unsigned i = (c->head + c->used) % 3u;
        c->fifo[i] = data;
        c->fifo_err[i] = err;
        c->used++;
        return;
    }
    c->overrun = 1;
    if (!s->overrun_keeps) {                    /* the data sheet: the new character replaces the last one */
        c->fifo[(c->head + 2u) % 3u] = data;
        c->fifo_err[(c->head + 2u) % 3u] = err;
    }
}

/* The stop bit of the frame has been sampled. */
static void rx_complete(mx_z80sio *s, unsigned chn) {
    mx_z80sio_chan *c = &s->ch[chn];
    const mx_z80sio_frame *f = &c->rx_frame;
    const uint64_t k0 = c->rx_k0;
    const unsigned d = c->rx_d, h = d > 1 ? d >> 1 : 0, nb = c->rx_nb, par = c->rx_par;
    const uint64_t t_done = c->rx_done_at;
    c->rx_done_at = MX_NEVER;
    c->rx_state = RX_IDLE;
    if (!rx_enabled(c)) return;
    if (f->brk && !c->rx_hold) return;
    /* A transient low is not a start bit. */
    if (frame_bit(f, mx_z80sio_edge_time(&c->rx_snap, k0 + h))) return;
    unsigned data = 0, px = 0;
    for (unsigned i = 0; i < nb; i++) {
        const unsigned bit = frame_bit(f, mx_z80sio_edge_time(&c->rx_snap, k0 + h + (uint64_t)(1u + i) * d));
        data |= bit << i;
        px ^= bit;
    }
    unsigned pe = 0;
    if (par) {
        const unsigned pb = frame_bit(f, mx_z80sio_edge_time(&c->rx_snap, k0 + h + (uint64_t)(1u + nb) * d));
        pe = pb != (px ^ ((c->wr[4] >> 1 & 1u) ^ 1u));     /* even parity (WR4 bit 1) expects the XOR of the data */
    }
    const unsigned sb = frame_bit(f, mx_z80sio_edge_time(&c->rx_snap, k0 + h + (uint64_t)(1u + nb + par) * d));
    const unsigned fe = !sb;
    rx_push(s, c, (uint8_t)data, (uint8_t)((pe ? ERR_PARITY : 0) | (fe ? ERR_FRAMING : 0)));
    if (pe) c->parity_latch = 1;
    if (f->brk && fe) {
        c->rx_in_break = 1;
        ext_refresh(c);
    }
    if (fe && !f->brk) {                        /* a framing error delays the search for the next start bit by half a bit */
        c->rx_state = RX_RECOVER;
        c->rx_free_at = t_done + f->bit_num / (2u * f->bit_den);
    }
}

/* A held break on RxD becomes one all-zero frame once the receiver is free. */
static void rx_break_poll(mx_z80sio *s, unsigned chn) {
    mx_z80sio_chan *c = &s->ch[chn];
    if (!c->rx_hold || c->rx_in_break || c->rx_state != RX_IDLE || !rx_enabled(c) || c->rx_clk.num == 0) return;
    mx_z80sio_frame f;
    memset(&f, 0, sizeof f);
    f.length = (uint8_t)(1u + char_bits(c->wr[3] >> 6) + (c->wr[4] & 1u) + 1u);
    f.brk = 1;
    f.start = mx_dev_now(&s->dev);
    f.bit_num = (uint64_t)divisor(c) * c->rx_clk.num;
    f.bit_den = c->rx_clk.den;
    rx_accept(s, chn, &f);
}

/* --- transmitter ----------------------------------------------------------------------------------- */

/* A character waiting in the buffer starts on the first clock edge after now. */
static void tx_arm(mx_z80sio *s, unsigned chn) {
    mx_z80sio_chan *c = &s->ch[chn];
    if (c->tx_active || !c->tx_full || !tx_enabled(c) || c->tx_clk.num == 0) {
        c->tx_start_at = MX_NEVER;
        return;
    }
    if (c->tx_start_at != MX_NEVER) return;
    c->tx_start_at = mx_z80sio_edge_time(&c->tx_clk, mx_z80sio_edge_after(&c->tx_clk, mx_dev_now(&s->dev), 1));
}

/* The character in the buffer goes to the shift register at the clock edge at
 * time `t`: the buffer is empty again (Tx interrupt), the start bit is on the
 * line and the frame is handed to the link. */
static void tx_begin(mx_z80sio *s, unsigned chn, uint64_t t) {
    mx_z80sio_chan *c = &s->ch[chn];
    c->tx_start_at = MX_NEVER;
    if (c->tx_active || !c->tx_full || !tx_enabled(c) || c->tx_clk.num == 0) return;
    const unsigned d = divisor(c), nb = char_bits(c->wr[5] >> 5), par = c->wr[4] & 1u, even = c->wr[4] >> 1 & 1u;
    const unsigned data = c->tx_buf & ((1u << nb) - 1u);
    mx_z80sio_frame f;
    memset(&f, 0, sizeof f);
    unsigned px = 0, pos = 1;
    for (unsigned i = 0; i < nb; i++) {
        const unsigned bit = data >> i & 1u;
        f.bits |= (uint16_t)(bit << pos);
        px ^= bit;
        pos++;
    }
    if (par) {
        f.bits |= (uint16_t)((even ? px : px ^ 1u) << pos);
        pos++;
    }
    f.bits |= (uint16_t)(1u << pos);            /* the stop bit */
    pos++;
    f.length = (uint8_t)pos;
    f.data = (uint8_t)data;
    f.nbits = (uint8_t)nb;
    f.start = t;
    f.bit_num = (uint64_t)d * c->tx_clk.num;
    f.bit_den = c->tx_clk.den;
    const unsigned sm = c->wr[4] >> 2 & 3u;
    const unsigned stop = sm == 2 ? d + (d >> 1) : sm == 3 ? 2u * d : d;
    const uint64_t k = mx_z80sio_edge_after(&c->tx_clk, t, 0);
    c->tx_end_at = mx_z80sio_edge_time(&c->tx_clk, k + (uint64_t)(pos - 1u) * d + stop);
    c->tx_active = 1;
    c->tx_full = 0;
    c->tx_ip = 1;
    if (s->link.tx_frame) s->link.tx_frame(s->link.ctx, chn, &f);
}

/* --- outputs, deadline, notification ---------------------------------------------------------------- */

static void update_outputs(mx_z80sio *s) {
    const uint8_t irq = winner(s) >= 0;
    if (irq != s->int_out) {
        s->int_out = irq;
        mx_dev_line(&s->dev, MX_Z80SIO_OUT_INT, irq);
    }
    for (unsigned i = 0; i < 2; i++) {
        mx_z80sio_chan *c = &s->ch[i];
        /* RTS follows its bit; when the bit is reset the output stays low until the transmitter is empty. */
        const uint8_t rts = (uint8_t)((c->wr[5] & 2u) || (c->rts_out && (c->tx_full || c->tx_active)));
        const uint8_t dtr = (c->wr[5] & 0x80u) != 0;
        if (rts != c->rts_out) {
            c->rts_out = rts;
            mx_dev_line(&s->dev, MX_Z80SIO_OUT_RTS(i), rts);
        }
        if (dtr != c->dtr_out) {
            c->dtr_out = dtr;
            mx_dev_line(&s->dev, MX_Z80SIO_OUT_DTR(i), dtr);
        }
    }
}

static void rearm(mx_z80sio *s) {
    uint64_t t = MX_NEVER;
    for (unsigned i = 0; i < 2; i++) {
        const mx_z80sio_chan *c = &s->ch[i];
        if (c->tx_start_at < t) t = c->tx_start_at;
        if (c->tx_end_at < t) t = c->tx_end_at;
        if (c->rx_done_at < t) t = c->rx_done_at;
        if (c->rx_free_at < t) t = c->rx_free_at;
    }
    mx_dev_schedule(&s->dev, t);
}

static void settle(mx_z80sio *s) {
    for (unsigned i = 0; i < 2; i++) rx_break_poll(s, i);
    update_outputs(s);
    rearm(s);
}

/* Tells the link when a receiver has become able to take a frame. */
static void notify_ready(mx_z80sio *s) {
    for (unsigned i = 0; i < 2; i++) {
        mx_z80sio_chan *c = &s->ch[i];
        if (!rx_ready_now(c)) {
            c->rx_ready_reported = 0;
        } else if (!c->rx_ready_reported) {
            c->rx_ready_reported = 1;
            if (s->link.rx_ready) s->link.rx_ready(s->link.ctx, i);
        }
    }
}

static void finish(mx_z80sio *s) {
    settle(s);
    notify_ready(s);
}

/* --- registers ---------------------------------------------------------------------------------------- */

static void channel_reset(mx_z80sio *s, unsigned chn) {
    mx_z80sio_chan *c = &s->ch[chn];
    const int was_break = c->tx_break_on;
    s->in_service &= (uint8_t)~(7u << (3 * chn));
    chan_reset(c);
    if (was_break && s->link.tx_break) s->link.tx_break(s->link.ctx, chn, 0);
}

static void return_from_interrupt(mx_z80sio *s) {
    for (unsigned j = 0; j < 6; j++) {
        if (s->in_service >> j & 1u) {
            s->in_service &= (uint8_t)~(1u << j);
            return;
        }
    }
}

static void write_register(mx_z80sio *s, unsigned chn, unsigned reg, uint8_t v) {
    mx_z80sio_chan *c = &s->ch[chn];
    const uint8_t old = c->wr[reg];
    c->wr[reg] = v;
    if (reg == 3 || reg == 5) {
        if (!rx_enabled(c) && c->rx_state != RX_IDLE) rx_abort(c);
        if (!tx_enabled(c)) c->tx_start_at = MX_NEVER;
        tx_arm(s, chn);
    }
    if (reg == 5 && ((old ^ v) & 0x10u)) {
        c->tx_break_on = (v >> 4) & 1u;
        if (s->link.tx_break) s->link.tx_break(s->link.ctx, chn, c->tx_break_on);
    }
}

static void write_control(mx_z80sio *s, unsigned chn, uint8_t v) {
    mx_z80sio_chan *c = &s->ch[chn];
    if (c->ptr != 0) {
        const unsigned reg = c->ptr;
        c->ptr = 0;
        write_register(s, chn, reg, v);
        return;
    }
    c->wr[0] = v;
    c->ptr = v & 7u;
    switch ((v >> 3) & 7u) {
    case 2:                                     /* reset external/status interrupts */
        c->ext_latched = 0;
        c->ext_ip = 0;
        break;
    case 3:                                     /* channel reset */
        channel_reset(s, chn);
        break;
    case 4:                                     /* enable interrupt on next receive character */
        c->rx_armed = 1;
        break;
    case 5:                                     /* reset Tx interrupt pending */
        c->tx_ip = 0;
        break;
    case 6:                                     /* error reset */
        c->overrun = 0;
        c->parity_latch = 0;
        if (c->used) c->fifo_err[c->head] = 0;
        break;
    case 7:                                     /* return from interrupt */
        return_from_interrupt(s);
        break;
    default:                                    /* null, send abort (synchronous only) */
        break;
    }
}

static uint8_t read_control(mx_z80sio *s, unsigned chn) {
    mx_z80sio_chan *c = &s->ch[chn];
    const unsigned reg = c->ptr;
    c->ptr = 0;
    switch (reg) {
    case 0: {
        const uint8_t ext = (uint8_t)((c->ext_latched ? c->ext_snap : c->ext_live) & 0xB8u);
        return (uint8_t)(ext | (c->tx_full ? 0 : MX_Z80SIO_RR0_TXEMPTY) | (c->used ? MX_Z80SIO_RR0_RXAVAIL : 0) |
                         (chn == 0 && pending_mask(s) ? MX_Z80SIO_RR0_INTPEND : 0));
    }
    case 1: {
        uint8_t v = (!c->tx_full && !c->tx_active) ? MX_Z80SIO_RR1_ALLSENT : 0;
        const uint8_t e = c->used ? c->fifo_err[c->head] : 0;
        if (c->parity_latch || (e & ERR_PARITY)) v |= MX_Z80SIO_RR1_PARITY;
        if (c->overrun) v |= MX_Z80SIO_RR1_OVERRUN;
        if (e & ERR_FRAMING) v |= MX_Z80SIO_RR1_FRAMING;
        return v;
    }
    case 2:
        return vector_for(s, winner(s));
    default:
        return 0;
    }
}

static uint8_t read_data(mx_z80sio_chan *c) {
    if (!c->used) return 0;
    const uint8_t v = c->fifo[c->head];
    c->head = (uint8_t)((c->head + 1u) % 3u);
    c->used--;
    c->rx_armed = 0;
    return v;
}

/* --- bus and events ------------------------------------------------------------------------------------ */

static uint32_t z_read(void *d, unsigned space, uint32_t offset, unsigned size) {
    mx_z80sio *s = d;
    (void)space; (void)size;
    const unsigned chn = offset >> 1 & 1u;
    const uint32_t v = (offset & 1u) ? read_control(s, chn) : read_data(&s->ch[chn]);
    finish(s);
    return v;
}

static void z_write(void *d, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_z80sio *s = d;
    (void)space; (void)size;
    const unsigned chn = offset >> 1 & 1u;
    mx_z80sio_chan *c = &s->ch[chn];
    if (offset & 1u) {
        write_control(s, chn, (uint8_t)value);
    } else {
        c->tx_buf = (uint8_t)value;
        c->tx_full = 1;
        c->tx_ip = 0;
        tx_arm(s, chn);
    }
    finish(s);
}

static void z_event(void *d, uint64_t now) {
    mx_z80sio *s = d;
    for (;;) {
        uint64_t best = MX_NEVER;
        unsigned bc = 0, kind = 0;
        for (unsigned i = 0; i < 2; i++) {
            const mx_z80sio_chan *c = &s->ch[i];
            if (c->tx_start_at < best) { best = c->tx_start_at; bc = i; kind = 0; }
            if (c->tx_end_at < best) { best = c->tx_end_at; bc = i; kind = 1; }
            if (c->rx_done_at < best) { best = c->rx_done_at; bc = i; kind = 2; }
            if (c->rx_free_at < best) { best = c->rx_free_at; bc = i; kind = 3; }
        }
        if (best > now) break;
        mx_z80sio_chan *c = &s->ch[bc];
        switch (kind) {
        case 0:
            tx_begin(s, bc, best);
            break;
        case 1:
            c->tx_end_at = MX_NEVER;
            c->tx_active = 0;
            tx_begin(s, bc, best);
            break;
        case 2:
            rx_complete(s, bc);
            break;
        default:
            c->rx_free_at = MX_NEVER;
            c->rx_state = RX_IDLE;
            break;
        }
    }
    finish(s);
}

static void z_reset(void *d) {
    mx_z80sio *s = d;
    for (unsigned i = 0; i < 2; i++) channel_reset(s, i);
    s->in_service = 0;
    finish(s);
}

static void z_input(void *d, int pin, int state) {
    mx_z80sio *s = d;
    if (pin < 0 || pin > 5) return;
    const unsigned chn = (unsigned)pin & 1u;
    mx_z80sio_chan *c = &s->ch[chn];
    const uint8_t v = state != 0;
    switch (pin >> 1) {
    case 0: c->in_cts = v; break;
    case 1: c->in_dcd = v; break;
    default: c->in_sync = v; break;
    }
    ext_refresh(c);
    if (!rx_enabled(c) && c->rx_state != RX_IDLE) rx_abort(c);
    if (!tx_enabled(c)) c->tx_start_at = MX_NEVER;
    tx_arm(s, chn);
    finish(s);
}

static const mx_dev_ops Z80SIO_OPS = {z_read, z_write, z_event, z_reset, z_input, NULL, NULL};

/* --- public ------------------------------------------------------------------------------------------------ */

void mx_z80sio_init(mx_z80sio *s, const mx_hw_host *host, const char *name) {
    memset(s, 0, sizeof *s);
    s->dev.ops = &Z80SIO_OPS;
    s->dev.host = host;
    s->dev.name = name;
    for (unsigned i = 0; i < 2; i++) {
        s->ch[i].rx_clk.den = s->ch[i].tx_clk.den = 1;
        chan_reset(&s->ch[i]);
    }
}

void mx_z80sio_set_link(mx_z80sio *s, const mx_z80sio_link *link) {
    if (link) s->link = *link;
    else memset(&s->link, 0, sizeof s->link);
}

void mx_z80sio_set_clock(mx_z80sio *s, unsigned channel, unsigned which, const mx_z80sio_clock *clock) {
    if (channel > 1) return;
    mx_z80sio_chan *c = &s->ch[channel];
    mx_z80sio_clock n = {0, 1, 0};
    if (clock && clock->num && clock->den) n = *clock;
    if (which & 1u) c->rx_clk = n;
    if (which & 2u) {
        c->tx_clk = n;
        c->tx_start_at = MX_NEVER;
        tx_arm(s, channel);
    }
    finish(s);
}

int mx_z80sio_rx_ready(const mx_z80sio *s, unsigned channel) {
    return channel < 2 && rx_ready_now(&s->ch[channel]);
}

int mx_z80sio_rx_frame(mx_z80sio *s, unsigned channel, const mx_z80sio_frame *frame) {
    if (channel > 1 || !frame->bit_num || !frame->bit_den || !rx_ready_now(&s->ch[channel])) return 0;
    rx_accept(s, channel, frame);
    finish(s);
    return 1;
}

void mx_z80sio_make_frame(const mx_z80sio *s, unsigned channel, uint8_t value, uint64_t start, mx_z80sio_frame *frame) {
    const mx_z80sio_chan *c = &s->ch[channel & 1u];
    const unsigned nb = char_bits(c->wr[3] >> 6), par = c->wr[4] & 1u, even = c->wr[4] >> 1 & 1u;
    const unsigned data = value & ((1u << nb) - 1u);
    unsigned px = 0, pos = 1;
    memset(frame, 0, sizeof *frame);
    for (unsigned i = 0; i < nb; i++) {
        const unsigned bit = data >> i & 1u;
        frame->bits |= (uint16_t)(bit << pos);
        px ^= bit;
        pos++;
    }
    if (par) {
        frame->bits |= (uint16_t)((even ? px : px ^ 1u) << pos);
        pos++;
    }
    frame->bits |= (uint16_t)(1u << pos);
    pos++;
    frame->length = (uint8_t)pos;
    frame->data = (uint8_t)data;
    frame->nbits = (uint8_t)nb;
    frame->start = start;
    frame->bit_num = (uint64_t)divisor(c) * c->rx_clk.num;
    frame->bit_den = c->rx_clk.den;
}

void mx_z80sio_rx_break(mx_z80sio *s, unsigned channel, int on) {
    if (channel > 1) return;
    mx_z80sio_chan *c = &s->ch[channel];
    c->rx_hold = on != 0;
    if (!on) {
        if (c->rx_state == RX_FRAME && c->rx_frame.brk) rx_abort(c);
        c->rx_in_break = 0;
        ext_refresh(c);
    }
    finish(s);
}

uint8_t mx_z80sio_vector(const mx_z80sio *s) { return vector_for(s, winner(s)); }

uint8_t mx_z80sio_inta(mx_z80sio *s) {
    const int w = winner(s);
    const uint8_t v = vector_for(s, w);
    if (w >= 0) {
        s->in_service |= (uint8_t)(1u << w);
        finish(s);
    }
    return v;
}

unsigned mx_z80sio_rx_count(const mx_z80sio *s, unsigned channel) {
    return channel < 2 ? s->ch[channel].used : 0;
}
