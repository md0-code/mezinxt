/* MezinXT - the Intel 8255A programmable peripheral interface (i8255.h).
 * Written from Intel's 8255A data sheet. MezinXT's own code. */
#include "i8255.h"

#include <string.h>

enum { G_A = 0, G_B = 1 };

/* Group g has a strobed input side / an acknowledged output side. Group A in
 * mode 2 has both; group B (mode 1) one or the other by its direction. */
static int has_in(const mx_i8255 *d, int g) {
    return g == G_A ? (d->mode_a == 2 || (d->mode_a == 1 && d->dir_a)) : (d->mode_b == 1 && d->dir_b);
}
static int has_out(const mx_i8255 *d, int g) {
    return g == G_A ? (d->mode_a == 2 || (d->mode_a == 1 && !d->dir_a)) : (d->mode_b == 1 && !d->dir_b);
}
/* The port C bits of the handshake inputs. The INTE flip-flop of a side
 * sits at its input's bit: PC4 (STB) or PC6 (ACK) for A, PC2 for B. */
static uint8_t stb_bit(int g) { return g == G_A ? 0x10 : 0x04; }
static uint8_t ack_bit(int g) { return g == G_A ? 0x40 : 0x04; }

/* Where the bits of port C come from in the current configuration. */
typedef struct layout {
    uint8_t free;                   /* plain I/O bits (direction by nibble) */
    uint8_t out_mask, out_val;      /* handshake outputs: driven bits and levels */
    uint8_t st_mask, st_val;        /* bits a read of port C takes from the flags */
} layout;

static void hs_out(layout *l, uint8_t bit, int level) {
    l->free &= (uint8_t)~bit;
    l->out_mask |= bit;
    l->st_mask |= bit;
    if (level) { l->out_val |= bit; l->st_val |= bit; }
}
static void hs_inte(layout *l, uint8_t bit, uint8_t inte) {
    l->free &= (uint8_t)~bit;
    l->st_mask |= bit;
    if (inte & bit) l->st_val |= bit;
}
static void make_layout(const mx_i8255 *d, layout *l) {
    memset(l, 0, sizeof *l);
    l->free = 0xFF;
    if (d->mode_a) {
        hs_out(l, 0x08, d->intr_in[G_A] | d->intr_out[G_A]);          /* INTR A */
        if (has_in(d, G_A)) { hs_out(l, 0x20, d->ibf[G_A]); hs_inte(l, 0x10, d->inte); }
        if (has_out(d, G_A)) { hs_out(l, 0x80, !d->obf[G_A]); hs_inte(l, 0x40, d->inte); }
    }
    if (d->mode_b) {
        hs_out(l, 0x01, d->intr_in[G_B] | d->intr_out[G_B]);          /* INTR B */
        hs_out(l, 0x02, has_in(d, G_B) ? d->ibf[G_B] : !d->obf[G_B]); /* IBF B or OBF B */
        hs_inte(l, 0x04, d->inte);
    }
}

/* The levels the chip drives on each port and the mask of driven pins. */
static void outputs(const mx_i8255 *d, uint8_t out[3], uint8_t drv[3]) {
    memset(out, 0, 3);
    memset(drv, 0, 3);
    if (d->mode_a == 2) {
        if (!(d->pin[2] & 0x40)) { drv[0] = 0xFF; out[0] = d->latch[0]; }   /* ACK A low enables the bus drivers */
    } else if (!d->dir_a) {
        drv[0] = 0xFF;
        out[0] = d->latch[0];
    }
    if (!d->dir_b) { drv[1] = 0xFF; out[1] = d->latch[1]; }
    layout l;
    make_layout(d, &l);
    uint8_t fo = (uint8_t)(l.free & ((d->dir_cu ? 0x00 : 0xF0) | (d->dir_cl ? 0x00 : 0x0F)));
    drv[2] = (uint8_t)(l.out_mask | fo);
    out[2] = (uint8_t)(l.out_val | (d->latch[2] & fo));
}

/* Reports every pin whose driven level changed. The level is recorded
 * before the call, so the board may call back into the chip. */
static void update(mx_i8255 *d) {
    uint8_t out[3], drv[3];
    outputs(d, out, drv);
    for (int port = 0; port < 3; port++) {
        uint8_t diff = (uint8_t)(out[port] ^ d->reported[port]);
        for (int bit = 0; diff; bit++, diff >>= 1) {
            if (!(diff & 1)) continue;
            d->reported[port] ^= (uint8_t)(1u << bit);
            mx_dev_line(&d->dev, MX_I8255_PIN(port, bit), (out[port] >> bit) & 1);
        }
    }
}

/* INTR conditions that already hold when an INTE flag is set. */
static void check_intr(mx_i8255 *d, int g) {
    if (has_in(d, g) && d->ibf[g] && (d->pin[2] & stb_bit(g)) && (d->inte & stb_bit(g))) d->intr_in[g] = 1;
    if (has_out(d, g) && !d->obf[g] && (d->pin[2] & ack_bit(g)) && (d->inte & ack_bit(g))) d->intr_out[g] = 1;
}

static void stb_fall(mx_i8255 *d, int g) { d->ibf[g] = 1; d->inlatch[g] = d->pin[g]; }
static void stb_rise(mx_i8255 *d, int g) {
    d->inlatch[g] = d->pin[g];
    if (d->ibf[g] && (d->inte & stb_bit(g))) d->intr_in[g] = 1;
}
static void ack_fall(mx_i8255 *d, int g) { d->obf[g] = 0; }
static void ack_rise(mx_i8255 *d, int g) {
    if (!d->obf[g] && (d->inte & ack_bit(g))) d->intr_out[g] = 1;
}

/* A port's input levels are now `v`. */
static void port_in(mx_i8255 *d, int port, uint8_t v) {
    uint8_t old = d->pin[port];
    d->pin[port] = v;
    if (port == MX_I8255_PORT_C) {
        uint8_t changed = (uint8_t)(old ^ v);
        for (int g = G_A; g <= G_B; g++) {
            if (has_in(d, g) && (changed & stb_bit(g))) {
                if (v & stb_bit(g)) stb_rise(d, g); else stb_fall(d, g);
            }
            if (has_out(d, g) && (changed & ack_bit(g))) {
                if (v & ack_bit(g)) ack_rise(d, g); else ack_fall(d, g);
            }
        }
    } else if (has_in(d, port) && !(d->pin[2] & stb_bit(port))) {
        d->inlatch[port] = v;                       /* the latch is transparent while STB is low */
    }
    update(d);
}

static uint8_t read_in_group(mx_i8255 *d, int g, int dir_in, uint8_t latch_out) {
    if (has_in(d, g)) {
        uint8_t v = d->inlatch[g];
        d->ibf[g] = 0;
        d->intr_in[g] = 0;
        return v;
    }
    return dir_in ? d->pin[g] : latch_out;
}

static uint8_t read8(mx_i8255 *d, uint32_t reg) {
    uint8_t v = 0xFF;
    switch (reg & 3u) {
    case 0: v = read_in_group(d, G_A, d->dir_a, d->latch[0]); break;
    case 1: v = read_in_group(d, G_B, d->dir_b, d->latch[1]); break;
    case 2: {
        layout l;
        make_layout(d, &l);
        uint8_t fin = (uint8_t)(l.free & ((d->dir_cu ? 0xF0 : 0x00) | (d->dir_cl ? 0x0F : 0x00)));
        uint8_t fout = (uint8_t)(l.free & ~fin);
        v = (uint8_t)(l.st_val | (d->pin[2] & fin) | (d->latch[2] & fout));
        break;
    }
    default: break;                                 /* the control register cannot be read */
    }
    update(d);
    return v;
}

static void clear_flags(mx_i8255 *d) {
    memset(d->latch, 0, sizeof d->latch);
    memset(d->inlatch, 0, sizeof d->inlatch);
    memset(d->ibf, 0, sizeof d->ibf);
    memset(d->obf, 0, sizeof d->obf);
    memset(d->intr_in, 0, sizeof d->intr_in);
    memset(d->intr_out, 0, sizeof d->intr_out);
    d->inte = 0;
}

static void state_reset(mx_i8255 *d) {
    d->mode_a = d->mode_b = 0;
    d->dir_a = d->dir_b = d->dir_cu = d->dir_cl = 1;
    clear_flags(d);
}

static void control(mx_i8255 *d, uint8_t v) {
    if (v & 0x80) {
        d->mode_a = (uint8_t)((v >> 5) & 3);
        if (d->mode_a == 3) d->mode_a = 2;
        d->dir_a = (v >> 4) & 1;
        d->dir_cu = (v >> 3) & 1;
        d->mode_b = (v >> 2) & 1;
        d->dir_b = (v >> 1) & 1;
        d->dir_cl = v & 1;
        clear_flags(d);
        return;
    }
    /* Port C bit set/reset. The INTE flip-flops are bits of it. */
    uint8_t bit = (uint8_t)(1u << ((v >> 1) & 7));
    int set = v & 1;
    uint8_t inte_bits = 0;
    if (has_in(d, G_A)) inte_bits |= 0x10;
    if (has_out(d, G_A)) inte_bits |= 0x40;
    if (d->mode_b == 1) inte_bits |= 0x04;
    if (!(bit & inte_bits)) {
        if (set) d->latch[2] |= bit; else d->latch[2] &= (uint8_t)~bit;
        return;
    }
    if (set) d->inte |= bit; else d->inte &= (uint8_t)~bit;
    for (int g = G_A; g <= G_B; g++) {
        if (!(d->inte & stb_bit(g))) d->intr_in[g] = 0;
        if (!(d->inte & ack_bit(g))) d->intr_out[g] = 0;
        check_intr(d, g);
    }
}

static void write8(mx_i8255 *d, uint32_t reg, uint8_t v) {
    switch (reg & 3u) {
    case 0:
    case 1: {
        int g = (int)(reg & 1u);
        d->latch[g] = v;
        if (has_out(d, g)) { d->obf[g] = 1; d->intr_out[g] = 0; }
        break;
    }
    case 2: d->latch[2] = v; break;
    default: control(d, v); break;
    }
    update(d);
}

static uint32_t i_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    mx_i8255 *d = dev;
    if (space != MX_SPACE_IO) return size == 2 ? 0xFFFFu : 0xFFu;
    uint32_t v = read8(d, offset);
    if (size == 2) v |= (uint32_t)read8(d, offset + 1) << 8;
    return v;
}

static void i_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_i8255 *d = dev;
    if (space != MX_SPACE_IO) return;
    write8(d, offset, (uint8_t)value);
    if (size == 2) write8(d, offset + 1, (uint8_t)(value >> 8));
}

static void i_event(void *dev, uint64_t now) { (void)dev; (void)now; }   /* nothing is scheduled */

static void i_reset(void *dev) {
    mx_i8255 *d = dev;
    state_reset(d);
    update(d);
}

static void i_input(void *dev, int pin, int state) {
    mx_i8255 *d = dev;
    if (pin >= MX_I8255_PORT(0) && pin <= MX_I8255_PORT(2)) {
        port_in(d, pin - MX_I8255_PORT(0), (uint8_t)state);
    } else if (pin >= 0 && pin < 24) {
        int port = pin >> 3;
        uint8_t m = (uint8_t)(1u << (pin & 7));
        port_in(d, port, state ? (uint8_t)(d->pin[port] | m) : (uint8_t)(d->pin[port] & ~m));
    }
}

static const mx_dev_ops I8255_OPS = {i_read, i_write, i_event, i_reset, i_input, NULL, NULL};

void mx_i8255_init(mx_i8255 *d, const mx_hw_host *host, const char *name) {
    memset(d, 0, sizeof *d);
    d->dev.ops = &I8255_OPS;
    d->dev.host = host;
    d->dev.name = name ? name : "i8255";
    memset(d->pin, 0xFF, sizeof d->pin);
    state_reset(d);
}

uint8_t mx_i8255_out(const mx_i8255 *d, int port) {
    uint8_t out[3], drv[3];
    outputs(d, out, drv);
    return out[port];
}

uint8_t mx_i8255_driven(const mx_i8255 *d, int port) {
    uint8_t out[3], drv[3];
    outputs(d, out, drv);
    return drv[port];
}
