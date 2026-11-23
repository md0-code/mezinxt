/* MezinXT - Intel 8259A programmable interrupt controller. Written from the
 * 8259A data sheet (see i8259.h). MezinXT's own code. */
#include "i8259.h"

#include <string.h>

/* ICW1 bits. */
#define ICW1_INIT 0x10u
#define ICW1_LTIM 0x08u
#define ICW1_SNGL 0x02u
#define ICW1_IC4  0x01u
/* ICW4 bits. */
#define ICW4_AEOI 0x02u
#define ICW4_SFNM 0x10u
/* OCW3 bits (D3 set, D4 clear). */
#define OCW3_SELECT 0x08u
#define OCW3_POLL   0x04u
#define OCW3_RR     0x02u
#define OCW3_RIS    0x01u
#define OCW3_ESMM   0x40u
#define OCW3_SMM    0x20u

/* Priority rank of a level, 0 the highest: the level after the lowest one
 * is the highest, and so on round the ring. */
static unsigned rank(const mx_i8259 *c, unsigned level) { return (level - (unsigned)c->lowest - 1u) & 7u; }
static unsigned level_of(const mx_i8259 *c, unsigned r) { return ((unsigned)c->lowest + 1u + r) & 7u; }

/* The highest-priority level of a bit set, -1 when empty. */
static int highest(const mx_i8259 *c, unsigned set) {
    for (unsigned r = 0; r < 8; r++) {
        const unsigned level = level_of(c, r);
        if (set & (1u << level)) return (int)level;
    }
    return -1;
}

/* The level that would be delivered now, -1 for none: the highest unmasked
 * request that is of higher priority than every level in service (equal
 * priority too in the special fully nested mode). In special mask mode the
 * in-service bits of masked levels do not count. */
static int resolve(const mx_i8259 *c) {
    const int p = highest(c, (unsigned)(c->irr & (uint8_t)~c->imr));
    if (p < 0) return -1;
    unsigned in_service = c->isr;
    if (c->special_mask) in_service &= (uint8_t)~c->imr;
    const int s = highest(c, in_service);
    if (s < 0) return p;
    const unsigned rp = rank(c, (unsigned)p), rs = rank(c, (unsigned)s);
    if (rp < rs || (rp == rs && (c->icw4 & ICW4_SFNM))) return p;
    return -1;
}

/* Drives INT from the state: called after anything that changes it. */
static void update(mx_i8259 *c) {
    const uint8_t want = resolve(c) >= 0;
    if (want != c->int_out) {
        c->int_out = want;
        mx_dev_line(&c->dev, MX_I8259_INT, want);
    }
}

/* What acknowledging `level` does to the registers (the INTA cycles, or the
 * poll command's read). */
static void acknowledge(mx_i8259 *c, unsigned level) {
    if (!(c->icw1 & ICW1_LTIM)) c->irr &= (uint8_t)~(1u << level);   /* level: the IRR follows the line */
    if (c->icw4 & ICW4_AEOI) {
        if (c->auto_rotate) c->lowest = (uint8_t)level;
    } else {
        c->isr |= (uint8_t)(1u << level);
    }
}

uint8_t mx_i8259_inta(mx_i8259 *c) {
    int level = resolve(c);
    if (level < 0) level = 7;                 /* spurious: IR7, no ISR bit */
    else acknowledge(c, (unsigned)level);
    update(c);
    return (uint8_t)((c->icw2 & 0xF8u) | (unsigned)level);
}

int mx_i8259_int(const mx_i8259 *c) { return c->int_out; }

static void write_icw1(mx_i8259 *c, uint8_t v) {
    c->icw1 = v;
    c->imr = 0;
    c->isr = 0;
    /* The edge sense circuit is reset: an edge-triggered input must go low
     * and rise again to request; a level-triggered one follows its line. */
    c->irr = (v & ICW1_LTIM) ? c->lines : 0;
    c->lowest = 7;
    c->special_mask = 0;
    c->read_isr = 0;
    c->poll = 0;
    c->auto_rotate = 0;
    if (!(v & ICW1_IC4)) c->icw4 = 0;         /* without ICW4 every function in it is off */
    c->icw_next = 2;
}

static void write_ocw2(mx_i8259 *c, uint8_t v) {
    const unsigned level = v & 7u;
    int done;
    switch (v >> 5) {
    case 1:                                    /* non-specific EOI */
        done = highest(c, c->isr);
        if (done >= 0) c->isr &= (uint8_t)~(1u << done);
        break;
    case 3:                                    /* specific EOI */
        c->isr &= (uint8_t)~(1u << level);
        break;
    case 5:                                    /* rotate on non-specific EOI */
        done = highest(c, c->isr);
        if (done >= 0) { c->isr &= (uint8_t)~(1u << done); c->lowest = (uint8_t)done; }
        break;
    case 7:                                    /* rotate on specific EOI */
        c->isr &= (uint8_t)~(1u << level);
        c->lowest = (uint8_t)level;
        break;
    case 6:                                    /* set priority */
        c->lowest = (uint8_t)level;
        break;
    case 4: c->auto_rotate = 1; break;         /* rotate in automatic EOI mode: set */
    case 0: c->auto_rotate = 0; break;         /* ... clear */
    default: break;                            /* 2: no operation */
    }
}

static void write_ocw3(mx_i8259 *c, uint8_t v) {
    if (v & OCW3_ESMM) c->special_mask = (v & OCW3_SMM) != 0;
    if (v & OCW3_POLL) c->poll = 1;
    if (v & OCW3_RR) c->read_isr = (v & OCW3_RIS) != 0;
}

static void i8259_write(void *d, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_i8259 *c = d;
    const uint8_t v = (uint8_t)value;
    (void)space; (void)size;
    if (!(offset & 1u)) {
        if (v & ICW1_INIT) write_icw1(c, v);
        else if (v & OCW3_SELECT) write_ocw3(c, v);
        else write_ocw2(c, v);
    } else if (c->icw_next == 2) {
        c->icw2 = v;
        c->icw_next = !(c->icw1 & ICW1_SNGL) ? 3 : (c->icw1 & ICW1_IC4) ? 4 : 0;
    } else if (c->icw_next == 3) {
        c->icw3 = v;
        c->icw_next = (c->icw1 & ICW1_IC4) ? 4 : 0;
    } else if (c->icw_next == 4) {
        c->icw4 = v;
        c->icw_next = 0;
    } else {
        c->imr = v;                            /* OCW1 */
    }
    update(c);
}

static uint32_t i8259_read(void *d, unsigned space, uint32_t offset, unsigned size) {
    mx_i8259 *c = d;
    (void)space; (void)size;
    if (offset & 1u) return c->imr;
    if (c->poll) {
        /* The poll command: this read is the acknowledge; the word is the
         * request flag and the highest level asking. */
        c->poll = 0;
        const int level = resolve(c);
        if (level < 0) return 0;
        acknowledge(c, (unsigned)level);
        update(c);
        return 0x80u | (unsigned)level;
    }
    return c->read_isr ? c->isr : c->irr;
}

static void i8259_event(void *d, uint64_t now) { (void)d; (void)now; }

static void i8259_reset(void *d) {
    mx_i8259 *c = d;
    /* The 8259A has no reset pin; this is the power-on state: nothing
     * requests until the BIOS programs it (all levels masked). */
    c->irr = c->isr = 0;
    c->imr = 0xFF;
    c->icw1 = c->icw2 = c->icw3 = c->icw4 = 0;
    c->icw_next = 0;
    c->lowest = 7;
    c->read_isr = c->poll = c->special_mask = c->auto_rotate = 0;
    update(c);
}

static void i8259_input(void *d, int pin, int state) {
    mx_i8259 *c = d;
    if (pin < 0 || pin > 7) return;
    const uint8_t bit = (uint8_t)(1u << pin);
    if (state) {
        if (!(c->lines & bit)) { c->lines |= bit; c->irr |= bit; }    /* the rising edge, or the level */
    } else if (c->lines & bit) {
        c->lines &= (uint8_t)~bit;
        if (c->icw1 & ICW1_LTIM) c->irr &= (uint8_t)~bit;            /* a level request goes with its line */
    }
    update(c);
}

static const mx_dev_ops I8259_OPS = {i8259_read, i8259_write, i8259_event, i8259_reset, i8259_input, NULL, NULL};

void mx_i8259_init(mx_i8259 *c, const mx_hw_host *host) {
    memset(c, 0, sizeof *c);
    c->dev.ops = &I8259_OPS;
    c->dev.host = host;
    c->dev.name = "i8259";
    i8259_reset(c);
}
