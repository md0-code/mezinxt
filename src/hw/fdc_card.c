/* MezinXT - the diskette adapter (fdc_card.h). Portable C11. MezinXT's own
 * code, from the IBM diskette drive adapter's documented behaviour. */
#include "fdc_card.h"

#include <string.h>

/* --- what the controller sees of the board ---------------------------------------------------- */

static uint64_t fh_now(void *ctx) {
    const mx_fdc_card *c = ctx;
    return mx_dev_now(&c->dev);
}

static void fh_schedule(void *ctx, void *dev, uint64_t when) {
    mx_fdc_card *c = ctx;
    (void)dev;
    mx_dev_schedule(&c->dev, when);
}

/* The controller's INT and DRQ reach the board only through the DOR's enable bit. */
static void update_outputs(mx_fdc_card *c) {
    const uint8_t enable = (c->dor & MX_FDC_DOR_ENABLE) != 0;
    const uint8_t irq = (uint8_t)(c->int_in && enable), drq = (uint8_t)(c->drq_in && enable);
    if (irq != c->irq_out) {
        c->irq_out = irq;
        mx_dev_line(&c->dev, MX_FDC_CARD_IRQ, irq);
    }
    if (drq != c->drq_out) {
        c->drq_out = drq;
        mx_dev_line(&c->dev, MX_FDC_CARD_DRQ, drq);
    }
}

static void fh_line(void *ctx, void *dev, int pin, int state) {
    mx_fdc_card *c = ctx;
    (void)dev;
    if (pin == MX_UPD765_PIN_INT) c->int_in = (uint8_t)(state != 0);
    else if (pin == MX_UPD765_PIN_DRQ) c->drq_in = (uint8_t)(state != 0);
    update_outputs(c);
}

/* --- the register file ------------------------------------------------------------------------ */

/* On the IBM adapter all four unit numbers of the controller reach the drive
 * the DOR selects (its unit select outputs are not wired); with READY and the
 * unit select taken from the drives (ready_wired 0) unit n is drive n. */
static void bind_drives(mx_fdc_card *c) {
    const unsigned sel = c->dor & MX_FDC_DOR_SELECT;
    for (unsigned u = 0; u < 4; u++) {
        const unsigned n = c->ready_wired ? sel : u;
        mx_upd765_attach(&c->fdc, u, n < c->drives ? &c->drive[n] : NULL);
    }
}

static void write_dor(mx_fdc_card *c, uint8_t v) {
    const uint8_t old = c->dor;
    const uint64_t now = mx_dev_now(&c->dev);
    c->dor = v;
    for (unsigned u = 0; u < c->drives; u++) mx_fdd_set_motor(&c->drive[u], (v >> (4 + u)) & 1, now);
    bind_drives(c);
    if (!(v & MX_FDC_DOR_NRESET)) {
        if (old & MX_FDC_DOR_NRESET) c->fdc.dev.ops->input(&c->fdc, MX_UPD765_PIN_RESET, 1);
    } else if (!(old & MX_FDC_DOR_NRESET)) c->fdc.dev.ops->input(&c->fdc, MX_UPD765_PIN_RESET, 0);
    update_outputs(c);
    if ((old ^ v) >> 4) mx_upd765_drive_changed(&c->fdc);
}

static uint32_t card_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    mx_fdc_card *c = dev;
    (void)size;
    if (offset == 4 || offset == 5) return c->fdc.dev.ops->read(&c->fdc, space, offset & 1u, 1);
    return 0xFFu;                               /* the DOR is write only; the rest is not decoded */
}

static void card_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_fdc_card *c = dev;
    (void)size;
    if (offset == 2) write_dor(c, (uint8_t)value);
    else if (offset == 5) c->fdc.dev.ops->write(&c->fdc, space, 1, 1, value);
}

static void card_event(void *dev, uint64_t now) {
    mx_fdc_card *c = dev;
    c->fdc.dev.ops->event(&c->fdc, now);
}

static void card_reset(void *dev) {
    mx_fdc_card *c = dev;
    const uint64_t now = mx_dev_now(&c->dev);
    c->dor = 0;
    for (unsigned u = 0; u < c->drives; u++) mx_fdd_set_motor(&c->drive[u], 0, now);
    bind_drives(c);
    c->fdc.dev.ops->input(&c->fdc, MX_UPD765_PIN_RESET, 1);
    update_outputs(c);
}

static void card_input(void *dev, int pin, int state) {
    mx_fdc_card *c = dev;
    if (pin == MX_FDC_CARD_TC) c->fdc.dev.ops->input(&c->fdc, MX_UPD765_PIN_TC, state);
}

static uint8_t card_dack(void *dev, int channel, int to_device, uint8_t value, int tc) {
    mx_fdc_card *c = dev;
    if (!(c->dor & MX_FDC_DOR_ENABLE)) return 0xFF;
    return c->fdc.dev.ops->dack(&c->fdc, channel, to_device, value, tc);
}

static const mx_dev_ops CARD_OPS = {card_read, card_write, card_event, card_reset, card_input, NULL, card_dack};

void mx_fdc_card_init(mx_fdc_card *c, const mx_hw_host *host, const char *name, unsigned drives) {
    memset(c, 0, sizeof *c);
    c->dev.ops = &CARD_OPS;
    c->dev.host = host;
    c->dev.name = name;
    c->drives = drives < 1 ? 1 : drives > 4 ? 4 : drives;
    c->ready_wired = 1;
    c->fdc_host.ctx = c;
    c->fdc_host.now = fh_now;
    c->fdc_host.schedule = fh_schedule;
    c->fdc_host.line = fh_line;
    c->fdc_host.dma = NULL;
    mx_upd765_init(&c->fdc, &c->fdc_host, "upd765");
    for (unsigned u = 0; u < 4; u++) {
        mx_fdd_init(&c->drive[u], u < c->drives, 40);
        c->drive[u].id = (uint8_t)(MX_DRIVE_FLOPPY0 + u);
    }
    bind_drives(c);
}

void mx_fdc_card_set_drive_hook(mx_fdc_card *c, mx_drive_hook hook, void *ctx) {
    for (unsigned u = 0; u < 4; u++) {
        c->drive[u].hook = hook;
        c->drive[u].hook_ctx = ctx;
    }
}

mx_fdd *mx_fdc_card_drive(mx_fdc_card *c, unsigned n) { return n < 4 ? &c->drive[n] : NULL; }

int mx_fdc_card_set_tracks(mx_fdc_card *c, unsigned n, unsigned tracks) {
    if (n >= 4 || c->drive[n].present) return 0;
    c->drive[n].tracks = tracks == 80 ? 80 : 40;
    return 1;
}

int mx_fdc_card_insert(mx_fdc_card *c, unsigned n, uint8_t *image, size_t size, unsigned flags, char *error, size_t capacity) {
    if (n >= 4) return 0;
    const int ok = mx_fdd_insert(&c->drive[n], image, size, flags, error, capacity);
    if (ok) mx_upd765_drive_changed(&c->fdc);
    return ok;
}

uint8_t *mx_fdc_card_eject(mx_fdc_card *c, unsigned n) {
    if (n >= 4) return NULL;
    uint8_t *image = mx_fdd_eject(&c->drive[n]);
    mx_upd765_drive_changed(&c->fdc);
    return image;
}

unsigned mx_fdc_card_activity(const mx_fdc_card *c) {
    unsigned v = 0;
    for (unsigned u = 0; u < c->drives; u++)
        if (c->drive[u].motor) v |= 1u << u;
    return v;
}

void mx_fdc_card_ready_wired(mx_fdc_card *c, int wired) {
    c->ready_wired = wired != 0;
    bind_drives(c);
    mx_upd765_ready_wiring(&c->fdc, wired);
    mx_upd765_drive_changed(&c->fdc);
}
