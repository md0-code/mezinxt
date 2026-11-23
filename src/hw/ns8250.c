/* MezinXT - the INS8250 (ns8250.h). Portable C11. MezinXT's own code, from the
 * INS8250 data sheet. */
#include "ns8250.h"

#include <string.h>

enum { LCR_STB = 0x04, LCR_PEN = 0x08, LCR_EPS = 0x10, LCR_STICK = 0x20, LCR_BRK = 0x40, LCR_DLAB = 0x80 };
enum { IER_RDA = 0x01, IER_THRE = 0x02, IER_LS = 0x04, IER_MS = 0x08 };
enum { MCR_DTR = 0x01, MCR_RTS = 0x02, MCR_OUT1 = 0x04, MCR_OUT2 = 0x08, MCR_LOOP = 0x10 };
enum { MSR_CTS = 1, MSR_DSR = 2, MSR_RI = 4, MSR_DCD = 8 };       /* the pins' bits, in `pins` and `msr_state` */

/* --- the line's timing ------------------------------------------------------------------------ */

/* A time in clock edges of the 1.8432 MHz crystal as OSC periods (rounded down: a deadline is the start
 * plus its whole span, so the error does not add up over a run of characters). */
static uint64_t osc(uint64_t clocks) { return clocks * MX_NS8250_CLOCK_NUM / MX_NS8250_CLOCK_DEN; }

static unsigned data_bits(const mx_ns8250 *u) { return 5u + (u->lcr & 3u); }
static unsigned parity_bits(const mx_ns8250 *u) { return (u->lcr & LCR_PEN) ? 1u : 0u; }
/* The stop bits in half bit times: 1, 1.5 (two selected with 5 data bits) or 2. */
static unsigned stop_halves(const mx_ns8250 *u) {
    if (!(u->lcr & LCR_STB)) return 2u;
    return data_bits(u) == 5u ? 3u : 4u;
}
static unsigned frame_halves(const mx_ns8250 *u) { return 2u * (1u + data_bits(u) + parity_bits(u)) + stop_halves(u); }
/* start + halves half-bit times; never when the baud generator is stopped. */
static uint64_t deadline(const mx_ns8250 *u, uint64_t start, unsigned halves) {
    return u->divisor ? start + osc((uint64_t)halves * 8u * u->divisor) : MX_NEVER;
}

/* A divisor or format change moves the deadlines of a character on the line. */
static void retime(mx_ns8250 *u) {
    if (u->tx_busy) u->tx_end = deadline(u, u->tx_start, frame_halves(u));
    if (u->rx_busy) {
        u->rx_ready = deadline(u, u->rx_start, frame_halves(u) - stop_halves(u) + 1u);
        u->rx_end = deadline(u, u->rx_start, frame_halves(u));
    }
    if (u->brk_in && !u->brk_seen) u->brk_at = deadline(u, u->brk_start, frame_halves(u));
}

static void arm(mx_ns8250 *u) {
    uint64_t t = MX_NEVER;
    if (u->rx_busy && !u->rx_landed && u->rx_ready < t) t = u->rx_ready;
    if (u->rx_busy && u->rx_end < t) t = u->rx_end;
    if (u->brk_in && !u->brk_seen && u->brk_at < t) t = u->brk_at;
    if (u->tx_busy && u->tx_end < t) t = u->tx_end;
    mx_dev_schedule(&u->dev, t);
}

/* --- registers ---------------------------------------------------------------------------------- */

static uint8_t lsr_value(const mx_ns8250 *u) {
    return (uint8_t)(u->dr | u->oe << 1 | u->pe << 2 | u->fe << 3 | u->bi << 4 | (u->thr_full ? 0 : 0x20) |
                     (!u->thr_full && !u->tx_busy ? 0x40 : 0));
}

static uint8_t msr_value(const mx_ns8250 *u) {
    return (uint8_t)(u->msr_delta | u->msr_state << 4);
}

/* The interrupt identification: 6, 4, 2 or 0 for what is pending, in order of priority; 1 for none. */
static unsigned pending(const mx_ns8250 *u) {
    if ((u->ier & IER_LS) && (u->oe || u->pe || u->fe || u->bi)) return 6u;
    if ((u->ier & IER_RDA) && u->dr) return 4u;
    if ((u->ier & IER_THRE) && u->thre_int) return 2u;
    if ((u->ier & IER_MS) && u->msr_delta) return 0u;
    return 1u;
}

int mx_ns8250_intrpt(const mx_ns8250 *u) { return pending(u) != 1u; }

/* The bus line: INTRPT through the adapter's OUT2 gate (the OUT2 pin is inactive in loopback mode). */
static void update_irq(mx_ns8250 *u) {
    const uint8_t level = (uint8_t)(pending(u) != 1u && (u->mcr & MCR_OUT2) && !(u->mcr & MCR_LOOP));
    if (level != u->o_irq) {
        u->o_irq = level;
        mx_dev_line(&u->dev, MX_NS8250_IRQ, level);
    }
}

/* The four modem outputs on their pins: the MCR's bits, all inactive in loopback mode. */
static void sync_pins(mx_ns8250 *u) {
    const int loop = (u->mcr & MCR_LOOP) != 0;
    const uint8_t rts = (uint8_t)(!loop && (u->mcr & MCR_RTS)), dtr = (uint8_t)(!loop && (u->mcr & MCR_DTR));
    const uint8_t out1 = (uint8_t)(!loop && (u->mcr & MCR_OUT1)), out2 = (uint8_t)(!loop && (u->mcr & MCR_OUT2));
    const int modem = rts != u->o_rts || dtr != u->o_dtr;
    const int r = rts != u->o_rts, d = dtr != u->o_dtr, o1 = out1 != u->o_out1, o2 = out2 != u->o_out2;
    u->o_rts = rts;
    u->o_dtr = dtr;
    u->o_out1 = out1;
    u->o_out2 = out2;
    if (r) mx_dev_line(&u->dev, MX_NS8250_RTS, rts);
    if (d) mx_dev_line(&u->dev, MX_NS8250_DTR, dtr);
    if (o1) mx_dev_line(&u->dev, MX_NS8250_OUT1, out1);
    if (o2) mx_dev_line(&u->dev, MX_NS8250_OUT2, out2);
    if (modem && u->end.modem) u->end.modem(u->end.ctx, u->unit, rts, dtr);
}

/* The MSR's state bits from the pins, or from the MCR in loopback mode; a change sets its delta bit (the
 * trailing edge of RI only when RI falls). */
static void msr_update(mx_ns8250 *u) {
    uint8_t now;
    if (u->mcr & MCR_LOOP)
        now = (uint8_t)((u->mcr & MCR_RTS ? MSR_CTS : 0) | (u->mcr & MCR_DTR ? MSR_DSR : 0) | (u->mcr & MCR_OUT1 ? MSR_RI : 0) |
                        (u->mcr & MCR_OUT2 ? MSR_DCD : 0));
    else now = u->pins;
    const uint8_t changed = (uint8_t)(now ^ u->msr_state);
    if (changed & MSR_CTS) u->msr_delta |= 0x01;
    if (changed & MSR_DSR) u->msr_delta |= 0x02;
    if ((changed & MSR_RI) && !(now & MSR_RI)) u->msr_delta |= 0x04;
    if (changed & MSR_DCD) u->msr_delta |= 0x08;
    u->msr_state = now;
}

/* --- the receiver ---------------------------------------------------------------------------------- */

/* The parity bit a character carries (and the receiver expects) with this LCR. */
static int parity_bit(const mx_ns8250 *u, uint8_t data) {
    if (u->lcr & LCR_STICK) return (u->lcr & LCR_EPS) ? 0 : 1;
    unsigned ones = 0;
    for (unsigned b = 0; b < 8; ++b) ones += (data >> b) & 1u;
    return (u->lcr & LCR_EPS) ? (int)(ones & 1u) : !(ones & 1u);
}

static uint8_t data_mask(const mx_ns8250 *u) { return (uint8_t)(0xFFu >> (8u - data_bits(u))); }

/* A character reaches the RBR: a character still in it is lost (OE); the errors stay until the LSR is read. */
static void land(mx_ns8250 *u, uint8_t data, int bad_parity, int bad_stop, int brk) {
    if (u->dr) u->oe = 1;
    u->rbr = data;
    u->dr = 1;
    if (bad_parity) u->pe = 1;
    if (bad_stop) u->fe = 1;
    if (brk) u->bi = 1;
}

static void start_rx(mx_ns8250 *u, uint8_t byte, int bad_parity, int bad_stop, uint64_t t) {
    u->rx_busy = 1;
    u->rx_landed = 0;
    u->rx_data = (uint8_t)(byte & data_mask(u));
    u->rx_bad_parity = (uint8_t)bad_parity;
    u->rx_bad_stop = (uint8_t)bad_stop;
    u->rx_start = t;
    u->rx_ready = deadline(u, t, frame_halves(u) - stop_halves(u) + 1u);
    u->rx_end = deadline(u, t, frame_halves(u));
}

/* Collects a byte from the end when the receiver is idle (and, with flow control, empty). */
static void pump(mx_ns8250 *u, uint64_t t) {
    if (u->pumping || !u->end.rx || (u->mcr & MCR_LOOP) || u->plug) return;
    u->pumping = 1;
    if (!u->rx_busy && !u->brk_in && (!u->flow || !u->dr)) {
        uint8_t b = 0;
        if (u->end.rx(u->end.ctx, u->unit, &b)) start_rx(u, b, 0, 0, t);
    }
    u->pumping = 0;
}

static void set_break_in(mx_ns8250 *u, int on) {
    if (on) {
        if (u->brk_in) return;
        u->brk_in = 1;
        u->brk_seen = 0;
        u->brk_start = mx_dev_now(&u->dev);
        u->brk_at = deadline(u, u->brk_start, frame_halves(u));
    } else if (u->brk_in) {
        u->brk_in = 0;
        u->brk_seen = 0;
        pump(u, mx_dev_now(&u->dev));
    }
}

/* --- the transmitter ------------------------------------------------------------------------------- */

/* The THR's byte goes into the shift register at time t; its start bit begins. */
static void start_tx(mx_ns8250 *u, uint64_t t) {
    u->tsr = u->thr;
    u->thr_full = 0;
    u->thre_int = 1;
    u->tx_busy = 1;
    u->tx_start = t;
    u->tx_end = deadline(u, t, frame_halves(u));
    const uint8_t data = (uint8_t)(u->tsr & data_mask(u));
    if ((u->mcr & MCR_LOOP) || u->plug) {
        if (!u->rx_busy && !u->brk_in) start_rx(u, data, 0, 0, t);        /* a busy receiver loses it, as on a wire */
    } else if (u->end.tx) u->end.tx(u->end.ctx, u->unit, data, data_bits(u));
}

/* The break the LCR holds on TxD: to the end, or round to the receiver in loopback mode or with a plug. */
static void update_break(mx_ns8250 *u) {
    const int on = (u->lcr & LCR_BRK) != 0, loop = (u->mcr & MCR_LOOP) != 0 || u->plug;
    const uint8_t to_end = (uint8_t)(on && !loop), to_rx = (uint8_t)(on && loop);
    if (to_end != u->brk_end) {
        u->brk_end = to_end;
        if (u->end.tx_break) u->end.tx_break(u->end.ctx, u->unit, to_end);
    }
    if (to_rx != u->brk_rx) {
        u->brk_rx = to_rx;
        set_break_in(u, to_rx);
    }
}

/* --- events ------------------------------------------------------------------------------------------ */

static void run_events(mx_ns8250 *u, uint64_t now) {
    for (;;) {
        enum { EV_NONE, EV_RX_READY, EV_RX_END, EV_BREAK, EV_TX_END } which = EV_NONE;
        uint64_t t = MX_NEVER;
        if (u->rx_busy && !u->rx_landed && u->rx_ready <= now && u->rx_ready < t) { t = u->rx_ready; which = EV_RX_READY; }
        if (u->rx_busy && u->rx_end <= now && u->rx_end < t) { t = u->rx_end; which = EV_RX_END; }
        if (u->brk_in && !u->brk_seen && u->brk_at <= now && u->brk_at < t) { t = u->brk_at; which = EV_BREAK; }
        if (u->tx_busy && u->tx_end <= now && u->tx_end < t) { t = u->tx_end; which = EV_TX_END; }
        if (which == EV_NONE) break;
        switch (which) {
        case EV_RX_READY:
            u->rx_landed = 1;
            land(u, u->rx_data, u->rx_bad_parity, u->rx_bad_stop, 0);
            break;
        case EV_RX_END:
            u->rx_busy = 0;
            pump(u, t);
            break;
        case EV_BREAK:
            /* Held low for a whole word: a zero character with a framing error, and the break indication. */
            u->brk_seen = 1;
            land(u, 0, (u->lcr & LCR_PEN) && parity_bit(u, 0) != 0, 1, 1);
            break;
        default:
            u->tx_busy = 0;
            if (u->thr_full) start_tx(u, t);
            break;
        }
    }
}

static void settle(mx_ns8250 *u) {
    update_irq(u);
    arm(u);
}

static void ns_event(void *dev, uint64_t now) {
    mx_ns8250 *u = dev;
    run_events(u, now);
    settle(u);
}

/* --- the register window ----------------------------------------------------------------------------- */

static uint32_t ns_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    mx_ns8250 *u = dev;
    (void)space;
    (void)size;
    uint32_t v = 0xFFu;
    switch (offset & 7u) {
    case 0:
        if (u->lcr & LCR_DLAB) v = u->divisor & 0xFFu;
        else {
            v = u->rbr;
            u->dr = 0;
            pump(u, mx_dev_now(&u->dev));
        }
        break;
    case 1:
        v = (u->lcr & LCR_DLAB) ? (uint32_t)(u->divisor >> 8) : u->ier;
        break;
    case 2:
        v = pending(u);
        if (v == 2u) u->thre_int = 0;                   /* reading it acknowledges the THRE interrupt */
        break;
    case 3: v = u->lcr; break;
    case 4: v = u->mcr; break;
    case 5:
        v = lsr_value(u);
        u->oe = u->pe = u->fe = u->bi = 0;
        break;
    case 6:
        v = msr_value(u);
        u->msr_delta = 0;
        break;
    default: break;                                      /* no scratch register on the 8250 */
    }
    settle(u);
    return v;
}

static void ns_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_ns8250 *u = dev;
    (void)space;
    (void)size;
    const uint8_t v = (uint8_t)value;
    switch (offset & 7u) {
    case 0:
        if (u->lcr & LCR_DLAB) {
            u->divisor = (uint16_t)((u->divisor & 0xFF00u) | v);
            retime(u);
            pump(u, mx_dev_now(&u->dev));
        } else {
            u->thr = v;
            u->thr_full = 1;
            u->thre_int = 0;
            if (!u->tx_busy) start_tx(u, mx_dev_now(&u->dev));
        }
        break;
    case 1:
        if (u->lcr & LCR_DLAB) {
            u->divisor = (uint16_t)((u->divisor & 0x00FFu) | (uint16_t)v << 8);
            retime(u);
            pump(u, mx_dev_now(&u->dev));
        } else {
            const uint8_t old = u->ier;
            u->ier = v & 0x0Fu;
            if (!(old & IER_THRE) && (u->ier & IER_THRE) && !u->thr_full) u->thre_int = 1;
        }
        break;
    case 3:
        u->lcr = v;
        retime(u);
        update_break(u);
        pump(u, mx_dev_now(&u->dev));                    /* the format may have changed under an end that cares */
        break;
    case 4: {
        const uint8_t old = u->mcr;
        u->mcr = v & 0x1Fu;
        msr_update(u);
        update_break(u);
        sync_pins(u);
        if ((old ^ u->mcr) & MCR_LOOP) pump(u, mx_dev_now(&u->dev));
        break;
    }
    default: break;                                      /* LSR and MSR (factory test) and the absent scratch register */
    }
    settle(u);
}

static void ns_reset(void *dev) {
    mx_ns8250 *u = dev;
    /* The master reset: IER, LCR, MCR and the status cleared, THRE and TEMT set, the outputs inactive; the
     * divisor latch, RBR and THR keep what they held; the modem inputs are what the pins say. */
    u->ier = u->lcr = u->mcr = 0;
    u->dr = u->oe = u->pe = u->fe = u->bi = 0;
    u->thr_full = 0;
    u->thre_int = 1;
    u->tx_busy = u->rx_busy = u->rx_landed = 0;
    u->brk_in = u->brk_seen = 0;
    u->pumping = 0;
    u->msr_state = u->pins;
    u->msr_delta = 0;
    sync_pins(u);
    update_break(u);
    update_irq(u);
    mx_dev_schedule(&u->dev, MX_NEVER);
}

static void ns_input(void *dev, int pin, int state) {
    mx_ns8250 *u = dev;
    if (pin < MX_NS8250_CTS || pin > MX_NS8250_DCD) return;
    const uint8_t bit = (uint8_t)(1u << pin);
    u->pins = (uint8_t)(state ? u->pins | bit : u->pins & ~bit);
    msr_update(u);
    settle(u);
}

static const mx_dev_ops NS_OPS = {ns_read, ns_write, ns_event, ns_reset, ns_input, NULL, NULL};

/* --- the connector ----------------------------------------------------------------------------------- */

void mx_ns8250_init(mx_ns8250 *u, const mx_hw_host *host, const char *name, unsigned unit) {
    memset(u, 0, sizeof *u);
    u->dev.ops = &NS_OPS;
    u->dev.host = host;
    u->dev.name = name;
    u->unit = unit;
    u->flow = 1;
}

void mx_ns8250_set_end(mx_ns8250 *u, const mx_ns8250_end *end) {
    if (end) u->end = *end;
    else memset(&u->end, 0, sizeof u->end);
    pump(u, mx_dev_now(&u->dev));
    settle(u);
}

void mx_ns8250_set_plug(mx_ns8250 *u, int on) {
    u->plug = on != 0;
    update_break(u);
    pump(u, mx_dev_now(&u->dev));
    settle(u);
}

void mx_ns8250_set_flow_control(mx_ns8250 *u, int on) { u->flow = on != 0; }

void mx_ns8250_rx_notify(mx_ns8250 *u) {
    pump(u, mx_dev_now(&u->dev));
    settle(u);
}

int mx_ns8250_rx(mx_ns8250 *u, uint8_t byte, int bad_parity, int bad_stop) {
    if ((u->mcr & MCR_LOOP) || u->rx_busy || u->brk_in) return 0;
    start_rx(u, byte, bad_parity, bad_stop, mx_dev_now(&u->dev));
    settle(u);
    return 1;
}

void mx_ns8250_rx_break(mx_ns8250 *u, int on) {
    if (u->mcr & MCR_LOOP) return;                      /* the RxD pin is disconnected */
    set_break_in(u, on);
    settle(u);
}

void mx_ns8250_set_modem(mx_ns8250 *u, int cts, int dsr, int ri, int dcd) {
    ns_input(u, MX_NS8250_CTS, cts);
    ns_input(u, MX_NS8250_DSR, dsr);
    ns_input(u, MX_NS8250_RI, ri);
    ns_input(u, MX_NS8250_DCD, dcd);
}

unsigned mx_ns8250_divisor(const mx_ns8250 *u) { return u->divisor; }
unsigned mx_ns8250_lcr(const mx_ns8250 *u) { return u->lcr; }
