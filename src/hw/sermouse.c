/* MezinXT - the Microsoft serial mouse (sermouse.h). Portable C11. MezinXT's
 * own code, from the Microsoft serial mouse protocol as publicly documented. */
#include "sermouse.h"

#include <string.h>

#define ACC_LIMIT 32767                 /* the accumulators saturate here */

void mx_sermouse_init(mx_sermouse *m) {
    memset(m, 0, sizeof *m);
}

static int clamp_acc(int v) { return v > ACC_LIMIT ? ACC_LIMIT : v < -ACC_LIMIT ? -ACC_LIMIT : v; }

/* The three bytes of a packet for deltas already within -128..127. */
static void packet(mx_sermouse *m, int dx, int dy) {
    const uint8_t x = (uint8_t)dx, y = (uint8_t)dy;
    m->queue[0] = (uint8_t)(0x40u | (m->buttons & MX_SERMOUSE_LEFT ? 0x20u : 0u) | (m->buttons & MX_SERMOUSE_RIGHT ? 0x10u : 0u) |
                            (unsigned)((y >> 6) & 3u) << 2 | (unsigned)((x >> 6) & 3u));
    m->queue[1] = (uint8_t)(x & 0x3Fu);
    m->queue[2] = (uint8_t)(y & 0x3Fu);
    m->head = 0;
    m->count = 3;
}

/* The mouse powered down, or reset: nothing queued and nothing owed. */
static void clear(mx_sermouse *m) {
    m->head = m->count = 0;
    m->acc_x = m->acc_y = 0;
    m->reported = 0;
}

static void notify(mx_sermouse *m) {
    if (m->uart && mx_sermouse_pending(m)) mx_ns8250_rx_notify(m->uart);
}

void mx_sermouse_motion(mx_sermouse *m, int dx, int dy, unsigned buttons) {
    m->buttons = buttons & (MX_SERMOUSE_LEFT | MX_SERMOUSE_RIGHT);
    if (!m->powered) return;                            /* an unpowered mouse sees nothing */
    m->acc_x = clamp_acc(m->acc_x + dx);
    m->acc_y = clamp_acc(m->acc_y + dy);
    notify(m);
}

void mx_sermouse_modem(mx_sermouse *m, int dtr, int rts) {
    const int was_powered = m->powered, was_rts = m->rts;
    m->dtr = dtr != 0;
    m->rts = rts != 0;
    m->powered = (uint8_t)(m->dtr || m->rts);
    if (!m->powered) {
        clear(m);
        return;
    }
    if (!was_powered || (m->rts && !was_rts)) {         /* power-up reset: the identification */
        clear(m);
        m->queue[0] = 'M';
        m->head = 0;
        m->count = 1;
        notify(m);
    }
}

int mx_sermouse_pending(const mx_sermouse *m) {
    if (!m->powered) return 0;
    return m->count || m->acc_x || m->acc_y || m->buttons != m->reported;
}

int mx_sermouse_next(mx_sermouse *m, uint8_t *byte) {
    if (!m->powered) return 0;
    if (!m->count) {
        if (!m->acc_x && !m->acc_y && m->buttons == m->reported) return 0;
        /* A new packet from what has accumulated: at most -128..127 of it, the rest stays. */
        const int dx = m->acc_x > 127 ? 127 : m->acc_x < -128 ? -128 : m->acc_x;
        const int dy = m->acc_y > 127 ? 127 : m->acc_y < -128 ? -128 : m->acc_y;
        m->acc_x -= dx;
        m->acc_y -= dy;
        m->reported = m->buttons;
        packet(m, dx, dy);
    }
    *byte = m->queue[m->head++];
    --m->count;
    return 1;
}

/* --- on an 8250's connector -------------------------------------------------------------------------- */

/* The mouse's bytes come only to a receiver set to 1200 baud, 7 data bits, no parity: divisor 96, LCR bits 1-0
 * equal to 10, parity enable clear. */
static int receiver_matches(const mx_ns8250 *u) {
    return mx_ns8250_divisor(u) == 96u && (mx_ns8250_lcr(u) & 0x0Bu) == 0x02u;
}

static int end_rx(void *ctx, unsigned unit, uint8_t *byte) {
    mx_sermouse *m = ctx;
    (void)unit;
    if (!m->uart || !receiver_matches(m->uart)) return 0;
    return mx_sermouse_next(m, byte);
}

static void end_modem(void *ctx, unsigned unit, int rts, int dtr) {
    (void)unit;
    mx_sermouse_modem(ctx, dtr, rts);
}

void mx_sermouse_attach(mx_sermouse *m, mx_ns8250 *uart) {
    m->uart = uart;
    mx_ns8250_end end;
    memset(&end, 0, sizeof end);
    end.ctx = m;
    end.rx = end_rx;
    end.modem = end_modem;
    mx_ns8250_set_end(uart, &end);
}
