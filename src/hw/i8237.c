/* MezinXT - the Intel 8237A DMA controller (i8237.h).
 * Written from Intel's 8237A data sheet and the IBM PC/XT technical
 * reference's use of it. MezinXT's own code. */
#include "i8237.h"

#include <string.h>

enum { SEL_DEMAND = 0, SEL_SINGLE = 1, SEL_BLOCK = 2, SEL_CASCADE = 3 };
enum { CMD_M2M = 0x01, CMD_HOLD0 = 0x02, CMD_DISABLE = 0x04, CMD_COMPRESS = 0x08, CMD_ROTATE = 0x10 };
enum { MODE_AUTOINIT = 0x10, MODE_DECREMENT = 0x20 };

/* The 74LS670 register index (port 80h + index) of each channel, and back. */
static const uint8_t CH_OF_INDEX[4] = {0, 2, 3, 1};

static int selection(const mx_i8237 *d, int ch) { return d->mode[ch] >> 6; }

/* A channel wants service: an unmasked DREQ, or a software request (which
 * the mask does not affect). */
static int active(const mx_i8237 *d, int ch) {
    return (((d->dreq & ~d->mask) | d->request) >> ch) & 1;
}

/* The channel that would get the bus now, -1 for none. */
static int pick(const mx_i8237 *d) {
    if (d->command & CMD_DISABLE) return -1;
    if (d->owner >= 0) {
        int sel = selection(d, d->owner);
        if (sel == SEL_BLOCK || (sel == SEL_DEMAND && active(d, d->owner))) return d->owner;
    }
    for (int i = 0; i < 4; i++) {
        int ch = (d->prio + i) & 3;
        if ((d->command & CMD_M2M) && ch == 1) continue;     /* the destination of memory-to-memory */
        if (active(d, ch)) return ch;
    }
    return -1;
}

static void set_dack(mx_i8237 *d, int ch, int on) {
    uint8_t bit = (uint8_t)(1u << ch);
    if (((d->dack & bit) != 0) == (on != 0)) return;
    if (on) d->dack |= bit; else d->dack &= (uint8_t)~bit;
    mx_dev_line(&d->dev, MX_I8237_DACK(ch), on);
}

static void set_eop(mx_i8237 *d, int on) {
    if (d->eop == (on != 0)) return;
    d->eop = on != 0;
    mx_dev_line(&d->dev, MX_I8237_EOP, on);
}

/* HRQ follows the requests; a cascade channel's DACK follows its DREQ. */
static void refresh(mx_i8237 *d) {
    for (int ch = 0; ch < 4; ch++) {
        if (selection(d, ch) == SEL_CASCADE && (d->dack & (1u << ch)) && !active(d, ch)) set_dack(d, ch, 0);
    }
    int want = pick(d) >= 0;
    if (!want) d->cpu_turn = 0;
    if (want != d->hrq) {
        d->hrq = (uint8_t)want;
        mx_dev_line(&d->dev, MX_I8237_HRQ, want);
    }
}

static void state_reset(mx_i8237 *d) {
    memset(d->base_addr, 0, sizeof d->base_addr);
    memset(d->base_count, 0, sizeof d->base_count);
    memset(d->cur_addr, 0, sizeof d->cur_addr);
    memset(d->cur_count, 0, sizeof d->cur_count);
    memset(d->mode, 0, sizeof d->mode);
    d->command = d->status = d->request = d->temp = d->flipflop = 0;
    d->mask = 0x0F;
    d->owner = -1;
    d->cpu_turn = 0;
    d->prio = 0;
}

/* Master clear: command, status, request, temporary register and the
 * flip-flop clear, all channels masked. Addresses and counts stay. */
static void master_clear(mx_i8237 *d) {
    d->command = d->status = d->request = d->temp = d->flipflop = 0;
    d->mask = 0x0F;
    d->owner = -1;
    d->cpu_turn = 0;
    d->prio = 0;
    set_eop(d, 0);
    for (int ch = 0; ch < 4; ch++) set_dack(d, ch, 0);
    refresh(d);
}

/* A channel reached terminal count. */
static void terminal(mx_i8237 *d, int ch) {
    d->status |= (uint8_t)(1u << ch);
    d->request &= (uint8_t)~(1u << ch);
    if (d->mode[ch] & MODE_AUTOINIT) {
        d->cur_addr[ch] = d->base_addr[ch];
        d->cur_count[ch] = d->base_count[ch];
    } else {
        d->mask |= (uint8_t)(1u << ch);
    }
}

static void step_address(mx_i8237 *d, int ch) {
    if (d->mode[ch] & MODE_DECREMENT) d->cur_addr[ch]--; else d->cur_addr[ch]++;
}

/* End of a cycle: keep the bus for a block, or a demand channel that still
 * has DREQ; otherwise give it to the CPU for a cycle. */
static void finish(mx_i8237 *d, int ch, int tc) {
    int sel = selection(d, ch);
    int keep = !tc && (sel == SEL_BLOCK || (sel == SEL_DEMAND && active(d, ch)));
    if (keep) {
        d->owner = (int8_t)ch;
        return;
    }
    d->owner = -1;
    d->cpu_turn = 1;
    if (d->command & CMD_ROTATE) d->prio = (uint8_t)((ch + 1) & 3);
}

static unsigned cycle_length(const mx_i8237 *d) {
    unsigned n = d->cycle_clocks;
    if ((d->command & CMD_COMPRESS) && n > 2) n--;
    return n;
}

/* One memory-to-memory cycle: channel 0's address is read into the
 * temporary register, channel 1's address written from it; channel 1's
 * count ends the transfer. */
static unsigned memory_cycle(mx_i8237 *d) {
    const int tc = d->cur_count[1] == 0;
    uint8_t byte = 0xFF;
    mx_dev_dma(&d->dev, ((uint32_t)d->page[0] << 16) | d->cur_addr[0], &byte, 1, 0);
    d->temp = byte;
    if (tc) set_eop(d, 1);
    mx_dev_dma(&d->dev, ((uint32_t)d->page[1] << 16) | d->cur_addr[1], &byte, 1, 1);
    if (!(d->command & CMD_HOLD0)) step_address(d, 0);
    step_address(d, 1);
    d->cur_count[1]--;
    if (tc) {
        d->status |= 0x02;
        d->request &= (uint8_t)~0x03;
        if (d->mode[1] & MODE_AUTOINIT) {
            d->cur_addr[0] = d->base_addr[0];
            d->cur_addr[1] = d->base_addr[1];
            d->cur_count[1] = d->base_count[1];
        } else {
            d->mask |= 0x02;
        }
    }
    set_eop(d, 0);
    finish(d, 0, tc);
    refresh(d);
    return 2 * cycle_length(d);
}

/* One transfer cycle of a channel. */
static unsigned transfer_cycle(mx_i8237 *d, int ch) {
    const unsigned type = (d->mode[ch] >> 2) & 3;           /* 1 write, 2 read; 0 and 3 verify */
    const int tc = d->cur_count[ch] == 0;
    const uint32_t address = ((uint32_t)d->page[ch] << 16) | d->cur_addr[ch];
    mx_dev *dev = d->device[ch];
    set_dack(d, ch, 1);
    if (tc) set_eop(d, 1);
    if (type == 2) {
        if (dev) {
            uint8_t byte = 0xFF;
            mx_dev_dma(&d->dev, address, &byte, 1, 0);
            if (dev->ops->dack) dev->ops->dack(dev, ch, 1, byte, tc);
        }
    } else if (type == 1) {
        uint8_t byte = 0xFF;
        if (dev && dev->ops->dack) byte = dev->ops->dack(dev, ch, 0, 0, tc);
        mx_dev_dma(&d->dev, address, &byte, 1, 1);
    }
    step_address(d, ch);
    d->cur_count[ch]--;
    if (tc) terminal(d, ch);
    set_eop(d, 0);
    finish(d, ch, tc);
    set_dack(d, ch, 0);
    refresh(d);
    return cycle_length(d);
}

unsigned mx_i8237_bus(mx_i8237 *d) {
    if (d->cpu_turn) {
        d->cpu_turn = 0;
        refresh(d);
        return 0;
    }
    if (d->owner >= 0 && !(d->command & CMD_DISABLE) && pick(d) != d->owner) d->owner = -1;
    int ch = pick(d);
    if (ch < 0) return 0;
    if (selection(d, ch) == SEL_CASCADE) {
        set_dack(d, ch, 1);                                 /* the cascaded controller takes the bus */
        return 0;
    }
    if ((d->command & CMD_M2M) && ch == 0) return memory_cycle(d);
    return transfer_cycle(d, ch);
}

int mx_i8237_hrq(const mx_i8237 *d) { return d->hrq; }

/* --- registers ----------------------------------------------------------------- */

static uint8_t read8(mx_i8237 *d, uint32_t reg) {
    reg &= 15u;
    if (reg < 8) {
        const uint16_t value = (reg & 1u) ? d->cur_count[reg >> 1] : d->cur_addr[reg >> 1];
        uint8_t v = d->flipflop ? (uint8_t)(value >> 8) : (uint8_t)value;
        d->flipflop ^= 1;
        return v;
    }
    if (reg == 8) {
        uint8_t v = (uint8_t)((d->status & 0x0F) | (((d->dreq | d->request) & 0x0F) << 4));
        d->status = 0;
        return v;
    }
    if (reg == 13) return d->temp;
    return 0xFF;
}

static void write_reg(uint16_t *base, uint16_t *cur, int high, uint8_t v) {
    if (high) {
        *base = (uint16_t)((*base & 0x00FF) | ((unsigned)v << 8));
        *cur = (uint16_t)((*cur & 0x00FF) | ((unsigned)v << 8));
    } else {
        *base = (uint16_t)((*base & 0xFF00) | v);
        *cur = (uint16_t)((*cur & 0xFF00) | v);
    }
}

static void write8(mx_i8237 *d, uint32_t reg, uint8_t v) {
    reg &= 15u;
    if (reg < 8) {
        int ch = (int)(reg >> 1);
        if (reg & 1u) write_reg(&d->base_count[ch], &d->cur_count[ch], d->flipflop, v);
        else write_reg(&d->base_addr[ch], &d->cur_addr[ch], d->flipflop, v);
        d->flipflop ^= 1;
        return;
    }
    switch (reg) {
    case 8:                                                 /* command */
        d->command = v;
        if (!(v & CMD_ROTATE)) d->prio = 0;
        break;
    case 9:                                                 /* request */
        if (v & 4) d->request |= (uint8_t)(1u << (v & 3)); else d->request &= (uint8_t)~(1u << (v & 3));
        break;
    case 10:                                                /* single mask */
        if (v & 4) d->mask |= (uint8_t)(1u << (v & 3)); else d->mask &= (uint8_t)~(1u << (v & 3));
        break;
    case 11: d->mode[v & 3] = v; break;                     /* mode */
    case 12: d->flipflop = 0; break;                        /* clear the first/last flip-flop */
    case 13: master_clear(d); return;                       /* master clear */
    case 14: d->mask = 0; break;                            /* clear the mask register */
    case 15: d->mask = v & 0x0F; break;                     /* write all mask bits */
    default: break;
    }
    refresh(d);
}

static uint32_t i_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    mx_i8237 *d = dev;
    if (space != MX_SPACE_IO) return size == 2 ? 0xFFFFu : 0xFFu;
    uint32_t v = read8(d, offset);
    if (size == 2) v |= (uint32_t)read8(d, offset + 1) << 8;
    return v;
}

static void i_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_i8237 *d = dev;
    if (space != MX_SPACE_IO) return;
    write8(d, offset, (uint8_t)value);
    if (size == 2) write8(d, offset + 1, (uint8_t)(value >> 8));
}

static void i_event(void *dev, uint64_t now) { (void)dev; (void)now; }   /* nothing is scheduled */

static void i_reset(void *dev) {
    mx_i8237 *d = dev;
    state_reset(d);
    set_eop(d, 0);
    for (int ch = 0; ch < 4; ch++) set_dack(d, ch, 0);
    refresh(d);
}

static void i_input(void *dev, int pin, int state) {
    mx_i8237 *d = dev;
    if (pin < 0 || pin > 3) return;
    if (state) d->dreq |= (uint8_t)(1u << pin); else d->dreq &= (uint8_t)~(1u << pin);
    refresh(d);
}

static const mx_dev_ops I8237_OPS = {i_read, i_write, i_event, i_reset, i_input, NULL, NULL};

void mx_i8237_init(mx_i8237 *d, const mx_hw_host *host, const char *name) {
    memset(d, 0, sizeof *d);
    d->dev.ops = &I8237_OPS;
    d->dev.host = host;
    d->dev.name = name ? name : "i8237";
    d->cycle_clocks = 4;
    state_reset(d);
}

void mx_i8237_attach(mx_i8237 *d, int channel, mx_dev *device) {
    if (channel >= 0 && channel < 4) d->device[channel] = device;
}

void mx_dma_page_write(mx_i8237 *d, unsigned index, uint8_t value) {
    d->page[CH_OF_INDEX[index & 3u]] = value & 0x0F;
}

uint8_t mx_dma_page_read(const mx_i8237 *d, unsigned index) {
    return d->page[CH_OF_INDEX[index & 3u]];
}
