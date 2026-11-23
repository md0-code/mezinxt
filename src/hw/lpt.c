/* MezinXT - the parallel printer port (lpt.h). Portable C11. MezinXT's own
 * code, from the Junior's manual (3.4) and the printer adapter's schematic
 * (sheet 19). */
#include "lpt.h"

static int busy_pin(const mx_lpt *p) {
    return p->phase == MX_LPT_BUSY || !p->select || p->error;
}

static void update_irq(mx_lpt *p) {
    const uint8_t irq = (p->ctl & MX_LPT_CTL_IRQEN) && p->phase == MX_LPT_ACK;
    if (irq != p->irq_out) {
        p->irq_out = irq;
        mx_dev_line(&p->dev, MX_LPT_IRQ, irq);
    }
}

static void schedule(mx_lpt *p) {
    mx_dev_schedule(&p->dev, p->phase == MX_LPT_IDLE ? MX_NEVER : p->phase_end);
}

static uint32_t lpt_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    mx_lpt *p = dev;
    (void)space; (void)size;
    switch (offset & 3u) {
    case 0:
        return p->data;
    case 1: {
        uint32_t v = 0x07;
        if (!busy_pin(p)) v |= MX_LPT_ST_BUSY;
        if (p->phase != MX_LPT_ACK) v |= MX_LPT_ST_ACK;
        if (p->paper_end) v |= MX_LPT_ST_PAPER;
        if (p->select) v |= MX_LPT_ST_SELECT;
        if (!p->error) v |= MX_LPT_ST_ERROR;
        return v;
    }
    case 2:
        return 0xE0u | (p->ctl & 0x1Fu);
    default:
        return 0xFFu;
    }
}

static void lpt_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_lpt *p = dev;
    (void)space; (void)size;
    switch (offset & 3u) {
    case 0:
        p->data = (uint8_t)value;
        break;
    case 2: {
        const uint8_t old = p->ctl;
        p->ctl = (uint8_t)(value & 0x1Fu);
        /* /INIT going low resets the printer. */
        if ((old & MX_LPT_CTL_INIT) && !(p->ctl & MX_LPT_CTL_INIT)) p->phase = MX_LPT_IDLE;
        /* The leading edge of STROBE (the pin goes low) latches the data. */
        if (!(old & MX_LPT_CTL_STROBE) && (p->ctl & MX_LPT_CTL_STROBE) && (p->ctl & MX_LPT_CTL_INIT) &&
            p->phase != MX_LPT_BUSY && !busy_pin(p)) {
            if (p->sink) p->sink(p->sink_ctx, p->data);
            p->phase = MX_LPT_BUSY;
            p->phase_end = mx_dev_now(&p->dev) + p->busy_osc;
        }
        update_irq(p);
        schedule(p);
        break;
    }
    default:
        break;
    }
}

static void lpt_event(void *dev, uint64_t now) {
    mx_lpt *p = dev;
    if (p->phase == MX_LPT_BUSY && now >= p->phase_end) {
        /* BUSY falls and /ACK goes low together. */
        p->phase = MX_LPT_ACK;
        p->phase_end += p->ack_osc;
    }
    if (p->phase == MX_LPT_ACK && now >= p->phase_end) p->phase = MX_LPT_IDLE;
    update_irq(p);
    schedule(p);
}

static void lpt_reset(void *dev) {
    mx_lpt *p = dev;
    p->ctl = 0;
    p->phase = MX_LPT_IDLE;
    update_irq(p);
    schedule(p);
}

static void lpt_input(void *dev, int pin, int state) {
    mx_lpt *p = dev;
    const uint8_t v = state != 0;
    if (pin == MX_LPT_IN_SELECT) p->select = v;
    else if (pin == MX_LPT_IN_PAPER_END) p->paper_end = v;
    else if (pin == MX_LPT_IN_ERROR) p->error = v;
}

static const mx_dev_ops LPT_OPS = {lpt_read, lpt_write, lpt_event, lpt_reset, lpt_input, NULL, NULL};

void mx_lpt_init(mx_lpt *p, const mx_hw_host *host, const char *name) {
    p->dev.ops = &LPT_OPS;
    p->dev.host = host;
    p->dev.name = name;
    p->data = 0;
    p->ctl = 0;
    p->select = 1;
    p->paper_end = 0;
    p->error = 0;
    p->phase = MX_LPT_IDLE;
    p->irq_out = 0;
    p->busy_osc = MX_LPT_US(10);
    p->ack_osc = MX_LPT_US(5);
    p->phase_end = 0;
    p->sink = NULL;
    p->sink_ctx = NULL;
}

void mx_lpt_set_sink(mx_lpt *p, void (*sink)(void *ctx, uint8_t byte), void *ctx) {
    p->sink = sink;
    p->sink_ctx = ctx;
}

void mx_lpt_set_timing(mx_lpt *p, uint64_t busy_osc, uint64_t ack_osc) {
    p->busy_osc = busy_osc ? busy_osc : 1;
    p->ack_osc = ack_osc ? ack_osc : 1;
}
