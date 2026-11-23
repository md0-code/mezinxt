/* MezinXT - the PC/XT keyboard and its interface (xtkbd.h).
 * Written from the IBM PC/XT technical reference's description of the
 * keyboard and its interface. MezinXT's own code. */
#include "xtkbd.h"

#include <string.h>

enum { FIFO_CODES = 16 };

static void arm(mx_xtkbd *k) {
    uint64_t t = k->t_bat;
    if (k->t_frame < t) t = k->t_frame;
    if (k->t_repeat < t) t = k->t_repeat;
    mx_dev_schedule(&k->dev, t);
}

/* The outputs the board sees: IRQ1 and the shift register's bits. */
static void publish(mx_xtkbd *k) {
    if (k->irq != k->reported_irq) {
        k->reported_irq = k->irq;
        mx_dev_line(&k->dev, MX_XTKBD_OUT_IRQ, k->irq);
    }
    uint8_t diff = (uint8_t)(k->sr ^ k->reported_sr);
    for (int bit = 0; diff; bit++, diff >>= 1) {
        if (!(diff & 1)) continue;
        k->reported_sr ^= (uint8_t)(1u << bit);
        mx_dev_line(&k->dev, MX_XTKBD_OUT_DATA(bit), (k->sr >> bit) & 1);
    }
}

/* A code into the keyboard's buffer; the 17th place holds the overrun code. */
static void push(mx_xtkbd *k, uint8_t code) {
    if (k->count < FIFO_CODES) k->fifo[k->count++] = code;
    else if (k->count == FIFO_CODES) k->fifo[k->count++] = 0xFF;
}

/* Starts a frame at time `at` when the keyboard may send: it is not
 * resetting, the clock is free, the interface is clear and empty. */
static void try_start(mx_xtkbd *k, uint64_t at) {
    if (k->sending || k->t_bat != MX_NEVER || !k->clock_high || k->clear || k->irq || k->count == 0) return;
    k->sending = 1;
    k->t_frame = at + k->frame_time;
}

static void abort_frame(mx_xtkbd *k) {
    k->sending = 0;
    k->t_frame = MX_NEVER;
}

/* The frame is on the wire's end: the byte is in the shift register. */
static void frame_done(mx_xtkbd *k) {
    k->sending = 0;
    k->t_frame = MX_NEVER;
    if (k->clear || !k->clock_high) return;         /* nobody listens: it is sent again later */
    k->sr = k->fifo[0];
    memmove(k->fifo, k->fifo + 1, (size_t)--k->count);
    k->irq = 1;
    publish(k);
}

static void reset_keyboard(mx_xtkbd *k, uint64_t at) {
    k->count = 0;
    memset(k->down, 0, sizeof k->down);
    k->repeat_key = 0;
    abort_frame(k);
    k->t_repeat = MX_NEVER;
    k->t_bat = at + k->bat_delay;
}

static uint32_t i_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    mx_xtkbd *k = dev;
    (void)offset;
    if (space != MX_SPACE_IO) return size == 2 ? 0xFFFFu : 0xFFu;
    return k->sr;
}

static void i_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    (void)dev; (void)space; (void)offset; (void)size; (void)value;
}

static void i_event(void *dev, uint64_t now) {
    mx_xtkbd *k = dev;
    for (;;) {
        uint64_t t = k->t_bat;
        int which = 0;
        if (k->t_frame < t) { t = k->t_frame; which = 1; }
        if (k->t_repeat < t) { t = k->t_repeat; which = 2; }
        if (t == MX_NEVER || t > now) break;
        if (which == 0) {
            k->t_bat = MX_NEVER;
            push(k, 0xAA);                          /* self test passed */
            try_start(k, t);
        } else if (which == 1) {
            frame_done(k);
            try_start(k, t);
        } else if (k->repeat_key) {
            push(k, k->repeat_key);
            k->t_repeat = t + k->repeat_period;
            try_start(k, t);
        } else {
            k->t_repeat = MX_NEVER;
        }
    }
    arm(k);
}

static void i_reset(void *dev) {
    mx_xtkbd *k = dev;
    const uint64_t now = mx_dev_now(&k->dev);
    k->sr = 0;
    k->irq = 0;
    k->clock_high = 0;
    k->clear = 0;
    k->clock_low_since = now;
    reset_keyboard(k, now);
    publish(k);
    arm(k);
}

static void i_input(void *dev, int pin, int state) {
    mx_xtkbd *k = dev;
    const uint64_t now = mx_dev_now(&k->dev);
    const uint8_t level = state != 0;
    if (pin == MX_XTKBD_IN_CLOCK) {
        if (level == k->clock_high) return;
        k->clock_high = level;
        if (!level) {
            k->clock_low_since = now;
            abort_frame(k);
        } else {
            if (now - k->clock_low_since >= k->reset_hold) reset_keyboard(k, now);
            try_start(k, now);
        }
    } else if (pin == MX_XTKBD_IN_CLEAR) {
        if (level == k->clear) return;
        k->clear = level;
        if (level) {
            k->sr = 0;
            k->irq = 0;
            publish(k);
        } else {
            try_start(k, now);
        }
    } else {
        return;
    }
    arm(k);
}

static const mx_dev_ops XTKBD_OPS = {i_read, i_write, i_event, i_reset, i_input, NULL, NULL};

void mx_xtkbd_init(mx_xtkbd *k, const mx_hw_host *host, const char *name) {
    memset(k, 0, sizeof *k);
    k->dev.ops = &XTKBD_OPS;
    k->dev.host = host;
    k->dev.name = name ? name : "xtkbd";
    k->frame_time = MX_XTKBD_US(900);
    k->reset_hold = MX_XTKBD_US(20000);
    k->bat_delay = MX_XTKBD_US(10000);
    k->repeat_delay = MX_XTKBD_US(500000);
    k->repeat_period = MX_XTKBD_US(91743);
    k->t_frame = k->t_repeat = k->t_bat = MX_NEVER;
}

int mx_xtkbd_key(mx_xtkbd *k, uint8_t make_code, int down) {
    const uint8_t code = make_code & 0x7F;
    if (code == 0 || k->t_bat != MX_NEVER) return 1;        /* no such key; or the keyboard is resetting */
    const uint64_t now = mx_dev_now(&k->dev);
    const uint8_t bit = (uint8_t)(1u << (code & 7));
    const int held = (k->down[code >> 3] & bit) != 0;
    if (down) {
        if (held) return 1;
        if (k->count >= FIFO_CODES) return 0;
        k->down[code >> 3] |= bit;
        push(k, code);
        k->repeat_key = code;
        k->t_repeat = now + k->repeat_delay;
    } else {
        if (!held) return 1;
        if (k->count >= FIFO_CODES) return 0;
        k->down[code >> 3] &= (uint8_t)~bit;
        push(k, (uint8_t)(code | 0x80));
        if (k->repeat_key == code) {
            k->repeat_key = 0;
            k->t_repeat = MX_NEVER;
        }
    }
    try_start(k, now);
    arm(k);
    return 1;
}

uint8_t mx_xtkbd_data(const mx_xtkbd *k) { return k->sr; }
int mx_xtkbd_irq(const mx_xtkbd *k) { return k->irq; }
