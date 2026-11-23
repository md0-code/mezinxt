/* MezinXT - the Winchester coupler (hdc.h). Portable C11. MezinXT's own code,
 * from IBM's documentation of the Fixed Disk Adapter's registers and commands. */
#include "hdc.h"

#include <string.h>

enum { P_IDLE, P_COMMAND, P_BUSY, P_DATA_IN, P_DATA_OUT, P_STATUS };
/* What the next event does. */
enum { S_NONE, S_FINISH, S_SEEKED, S_READ, S_WRITE, S_VERIFY, S_FORMAT };

/* --- time ------------------------------------------------------------------------------------- */

static uint64_t us(uint64_t n) { return n * MX_OSC_NUM / (MX_OSC_DEN * 1000000u); }
/* 3600 rpm, 17 sectors a track. */
#define SECTOR_TIME ((uint64_t)MX_OSC_NUM / ((uint64_t)MX_OSC_DEN * 60u * MX_HDC_SPT))
#define REVOLUTION (SECTOR_TIME * MX_HDC_SPT)
#define OVERHEAD us(50)

static uint64_t seek_time(uint32_t from, uint32_t to) {
    const uint32_t d = from > to ? from - to : to - from;
    return d ? us(1500u + 30u * (uint64_t)d) : 0u;
}
/* The end of sector `s` the next time it passes under the head after `t`. */
static uint64_t sector_end(uint64_t t, uint32_t s) {
    const uint64_t k = t / SECTOR_TIME;
    const uint32_t current = (uint32_t)(k % MX_HDC_SPT);
    const uint32_t wait = (s + 2u * MX_HDC_SPT - current - 1u) % MX_HDC_SPT;
    return (k + 1u + wait + 1u) * SECTOR_TIME;
}

/* The heads of drive `d` move to cylinder `to`; a seek of any length is reported to the drive sound's hook (hw.h) as it
 * starts, at `when`. */
static void move_heads(mx_hdc *c, mx_hdc_drive *d, uint32_t to, uint64_t when) {
    if (c->hook && to != d->cylinder) {
        const uint32_t distance = to > d->cylinder ? to - d->cylinder : d->cylinder - to;
        c->hook(c->hook_ctx, (unsigned)(MX_DRIVE_HARD0 + (unsigned)(d - c->drive)), MX_DRIVE_SEEK, when,
                 distance | (to == 0 ? MX_DRIVE_SEEK_HOME : 0u), 1500u + 30u * distance);
    }
    d->cylinder = to;
}

/* --- lines ------------------------------------------------------------------------------------ */

static void update_outputs(mx_hdc *c) {
    const uint8_t irq = (uint8_t)(c->interrupt && (c->mask & MX_HDC_MASK_IRQ));
    const uint8_t drq = (uint8_t)((c->phase == P_DATA_IN || c->phase == P_DATA_OUT) && c->data_dma && (c->mask & MX_HDC_MASK_DMA) &&
                                  c->data_pos < c->data_length);
    if (irq != c->irq_out) {
        c->irq_out = irq;
        mx_dev_line(&c->dev, MX_HDC_IRQ, irq);
    }
    if (drq != c->drq_out) {
        c->drq_out = drq;
        mx_dev_line(&c->dev, MX_HDC_DRQ, drq);
    }
}

static uint8_t status(const mx_hdc *c) {
    uint8_t v = c->interrupt ? MX_HDC_ST_IRQ : 0u;
    switch (c->phase) {
    case P_COMMAND: v |= MX_HDC_ST_BSY | MX_HDC_ST_CD | MX_HDC_ST_REQ; break;
    case P_BUSY: v |= MX_HDC_ST_BSY; break;
    case P_DATA_IN: v |= MX_HDC_ST_BSY | MX_HDC_ST_IO | MX_HDC_ST_REQ; break;
    case P_DATA_OUT: v |= MX_HDC_ST_BSY | MX_HDC_ST_REQ; break;
    case P_STATUS: v |= MX_HDC_ST_BSY | MX_HDC_ST_CD | MX_HDC_ST_IO | MX_HDC_ST_REQ; break;
    default: break;
    }
    if (c->drq_out) v |= MX_HDC_ST_DRQ;
    return v;
}

/* --- the drive's address ---------------------------------------------------------------------- */

static mx_hdc_drive *drive(mx_hdc *c) { return &c->drive[c->unit]; }
static uint32_t limit_cylinders(const mx_hdc_drive *d) { return d->max_cylinders ? d->max_cylinders : d->media->cylinders; }
static uint32_t limit_heads(const mx_hdc_drive *d) { return d->max_heads ? d->max_heads : d->media->heads; }

/* 0 when the address is good, else its error code. */
static uint8_t check_address(const mx_hdc *c) {
    const mx_hdc_drive *d = &c->drive[c->unit];
    if (c->cylinder >= limit_cylinders(d) || c->head >= limit_heads(d) || c->sector >= MX_HDC_SPT) return MX_HDC_E_ADDRESS;
    if (c->cylinder >= d->media->cylinders || c->head >= d->media->heads) return MX_HDC_E_NOT_FOUND;
    return 0;
}
static uint32_t lba(const mx_hdc *c) {
    const mx_hdc_drive *d = &c->drive[c->unit];
    return (c->cylinder * d->media->heads + c->head) * MX_HDC_SPT + c->sector;
}
/* The next sector of a run: on along the track, then the next head, then the next cylinder. */
static void advance(mx_hdc *c) {
    if (++c->sector < MX_HDC_SPT) return;
    c->sector = 0;
    if (++c->head < limit_heads(drive(c))) return;
    c->head = 0;
    ++c->cylinder;
}

/* --- phases ----------------------------------------------------------------------------------- */

static void finish(mx_hdc *c, uint8_t error, int address_valid) {
    mx_hdc_drive *d = drive(c);
    if (error) {
        d->sense[0] = (uint8_t)(error | (address_valid ? 0x80u : 0u));
        d->sense[1] = (uint8_t)(c->unit << 5 | (c->head & 31u));
        d->sense[2] = (uint8_t)((c->cylinder >> 8 & 3u) << 6 | (c->sector & 63u));
        d->sense[3] = (uint8_t)c->cylinder;
    }
    c->status_byte = (uint8_t)(c->unit << 5 | (error ? 2u : 0u));
    c->phase = P_STATUS;
    c->step = S_NONE;
    c->interrupt = 1;
    c->data_length = c->data_pos = 0;
    d->busy = 0;
    d->last = mx_dev_now(&c->dev);
    mx_dev_schedule(&c->dev, MX_NEVER);
    update_outputs(c);
}

static void after(mx_hdc *c, int step, uint64_t delay) {
    c->phase = P_BUSY;
    c->step = (uint8_t)step;
    mx_dev_schedule(&c->dev, mx_dev_now(&c->dev) + delay);
    update_outputs(c);
}

/* A data phase: `length` bytes of `data` to (in 1) or from the host; `dma`: sector data, through DMA when enabled. */
static void data_phase(mx_hdc *c, int in, uint8_t *data, uint32_t length, int dma) {
    c->phase = (uint8_t)(in ? P_DATA_IN : P_DATA_OUT);
    c->data = data;
    c->data_length = length;
    c->data_pos = 0;
    c->data_dma = (uint8_t)(dma != 0);
    c->step = S_NONE;
    update_outputs(c);
}

static int is_long(const mx_hdc *c) { return c->command == MX_HDC_CMD_READ_LONG || c->command == MX_HDC_CMD_WRITE_LONG; }

/* MezinXT's check bytes of a long transfer: a sum, not the Xebec's ECC. */
static void check_bytes(uint8_t *buffer) {
    uint32_t sum = 0;
    for (unsigned i = 0; i < MX_HDC_SECTOR; ++i) sum = sum * 31u + buffer[i];
    for (unsigned i = 0; i < 4; ++i) buffer[MX_HDC_SECTOR + i] = (uint8_t)(sum >> (24 - 8 * i));
}

/* The sector at the address is read into the buffer and offered. */
static void read_sector(mx_hdc *c) {
    const uint8_t e = check_address(c);
    if (e) { finish(c, e, 1); return; }
    mx_hdc_drive *d = drive(c);
    if (!d->media->read(d->media->ctx, lba(c), c->buffer)) { finish(c, MX_HDC_E_DATA, 1); return; }
    if (is_long(c)) check_bytes(c->buffer);
    data_phase(c, 1, c->buffer, is_long(c) ? MX_HDC_SECTOR + 4u : MX_HDC_SECTOR, 1);
}

/* The next sector of a read or write run: under the head one sector time after the last (a seek first when the
 * run crosses into the next cylinder). */
static void next_sector(mx_hdc *c, int step) {
    const uint32_t cylinder = c->cylinder;
    advance(c);
    const uint8_t e = check_address(c);
    if (e) { finish(c, e, 1); return; }
    mx_hdc_drive *d = drive(c);
    uint64_t delay = SECTOR_TIME;
    if (c->cylinder != cylinder) {
        delay += seek_time(cylinder, c->cylinder);
        move_heads(c, d, c->cylinder, mx_dev_now(&c->dev) + SECTOR_TIME);
    }
    after(c, step, delay);
}

/* The host has taken or given the last byte of a data phase. */
static void data_done(mx_hdc *c) {
    c->data_length = c->data_pos = 0;
    update_outputs(c);
    switch (c->command) {
    case MX_HDC_CMD_INIT: {
        mx_hdc_drive *d = drive(c);
        d->max_cylinders = (uint32_t)c->params[0] << 8 | c->params[1];
        d->max_heads = c->params[2];
        if (d->max_cylinders > 1024u) d->max_cylinders = 1024u;
        if (d->max_heads > 32u) d->max_heads = 32u;
        finish(c, 0, 0);
        break;
    }
    case MX_HDC_CMD_READ:
    case MX_HDC_CMD_READ_LONG:
        if (++c->done >= c->count) finish(c, 0, 0);
        else next_sector(c, S_READ);
        break;
    case MX_HDC_CMD_WRITE:
    case MX_HDC_CMD_WRITE_LONG:
        /* The buffer is full: written when the sector comes round (the first one after its rotational wait). */
        after(c, S_WRITE, c->done ? SECTOR_TIME : sector_end(mx_dev_now(&c->dev), c->sector) - mx_dev_now(&c->dev));
        break;
    default:
        finish(c, 0, 0);
        break;
    }
}

/* A byte moves on the data port or by DMA. */
static uint8_t data_in(mx_hdc *c) {
    const uint8_t v = c->data[c->data_pos++];
    if (c->data_pos >= c->data_length) data_done(c);
    return v;
}
static void data_out(mx_hdc *c, uint8_t v) {
    c->data[c->data_pos++] = v;
    if (c->data_pos >= c->data_length) data_done(c);
}

/* --- commands --------------------------------------------------------------------------------- */

static void start(mx_hdc *c) {
    const uint8_t *b = c->dcb;
    c->command = b[0];
    c->unit = (uint8_t)(b[1] >> 5 & 1u);
    c->head = b[1] & 31u;
    c->sector = b[2] & 63u;
    c->cylinder = (uint32_t)(b[2] & 0xC0u) << 2 | b[3];
    c->count = b[4] ? b[4] : 256u;
    c->done = 0;
    mx_hdc_drive *d = drive(c);
    if (c->command != MX_HDC_CMD_SENSE) memset(d->sense, 0, sizeof d->sense);
    d->busy = 1;
    const int present = d->media != NULL;
    switch (c->command) {
    case MX_HDC_CMD_SENSE:
        memcpy(c->params, d->sense, 4);
        memset(d->sense, 0, sizeof d->sense);
        data_phase(c, 1, c->params, 4, 0);
        return;
    case MX_HDC_CMD_INIT:
        data_phase(c, 0, c->params, 8, 0);
        return;
    case MX_HDC_CMD_ECC_LENGTH:
        c->params[0] = 0;
        data_phase(c, 1, c->params, 1, 0);
        return;
    case MX_HDC_CMD_READ_BUFFER:
        data_phase(c, 1, c->buffer, MX_HDC_SECTOR, 1);
        return;
    case MX_HDC_CMD_WRITE_BUFFER:
        data_phase(c, 0, c->buffer, MX_HDC_SECTOR, 1);
        return;
    case MX_HDC_CMD_RAM_DIAG:
    case MX_HDC_CMD_CTRL_DIAG:
        after(c, S_FINISH, us(1000));
        return;
    case MX_HDC_CMD_TEST_READY:
    case MX_HDC_CMD_RECALIBRATE:
    case MX_HDC_CMD_DRIVE_DIAG:
    case MX_HDC_CMD_GEOMETRY:
    case MX_HDC_CMD_FORMAT_DRIVE:
    case MX_HDC_CMD_VERIFY:
    case MX_HDC_CMD_FORMAT_TRACK:
    case MX_HDC_CMD_FORMAT_BAD:
    case MX_HDC_CMD_READ:
    case MX_HDC_CMD_WRITE:
    case MX_HDC_CMD_SEEK:
    case MX_HDC_CMD_READ_LONG:
    case MX_HDC_CMD_WRITE_LONG:
        break;
    default:
        c->step = S_FINISH;
        c->phase = P_BUSY;
        finish(c, MX_HDC_E_INVALID, 0);
        return;
    }
    if (!present) {
        c->phase = P_BUSY;
        finish(c, MX_HDC_E_NOT_READY, 0);
        return;
    }
    switch (c->command) {
    case MX_HDC_CMD_TEST_READY:
        after(c, S_FINISH, OVERHEAD);
        return;
    case MX_HDC_CMD_GEOMETRY: {
        const mx_hdc_media *m = d->media;
        c->params[0] = (uint8_t)(m->cylinders >> 8);
        c->params[1] = (uint8_t)m->cylinders;
        c->params[2] = (uint8_t)m->heads;
        c->params[3] = (uint8_t)m->sectors;
        c->params[4] = 1;
        c->params[5] = c->params[6] = c->params[7] = 0;
        data_phase(c, 1, c->params, 8, 0);
        return;
    }
    case MX_HDC_CMD_RECALIBRATE:
    case MX_HDC_CMD_DRIVE_DIAG: {
        const uint64_t t = seek_time(d->cylinder, 0);
        move_heads(c, d, 0, mx_dev_now(&c->dev));
        after(c, S_FINISH, OVERHEAD + t);
        return;
    }
    default:
        break;
    }
    /* The commands with an address: checked, then the heads go there. */
    const uint8_t e = check_address(c);
    if (e) {
        c->phase = P_BUSY;
        finish(c, e, 1);
        return;
    }
    const uint64_t t = OVERHEAD + seek_time(d->cylinder, c->cylinder);
    move_heads(c, d, c->cylinder, mx_dev_now(&c->dev));
    after(c, c->command == MX_HDC_CMD_SEEK ? S_FINISH : S_SEEKED, t);
}

static void event(void *dev, uint64_t now) {
    mx_hdc *c = dev;
    if (c->phase != P_BUSY) return;
    mx_hdc_drive *d = drive(c);
    switch (c->step) {
    case S_FINISH:
        finish(c, 0, 0);
        break;
    case S_SEEKED:
        /* On the cylinder: the command's own work begins. */
        switch (c->command) {
        case MX_HDC_CMD_READ:
        case MX_HDC_CMD_READ_LONG:
            after(c, S_READ, sector_end(now, c->sector) - now);
            break;
        case MX_HDC_CMD_WRITE:
        case MX_HDC_CMD_WRITE_LONG:
            data_phase(c, 0, c->buffer, is_long(c) ? MX_HDC_SECTOR + 4u : MX_HDC_SECTOR, 1);
            break;
        case MX_HDC_CMD_VERIFY:
            after(c, S_VERIFY, sector_end(now, c->sector) - now + (uint64_t)(c->count - 1u) * SECTOR_TIME);
            break;
        default:
            /* The format commands: from the next index, one revolution a track. */
            after(c, S_FORMAT, (now / REVOLUTION + 2u) * REVOLUTION - now);
            break;
        }
        break;
    case S_READ:
        read_sector(c);
        break;
    case S_WRITE:
        if (!d->media->write(d->media->ctx, lba(c), c->buffer)) { finish(c, MX_HDC_E_WRITE_FAULT, 1); break; }
        if (++c->done >= c->count) { finish(c, 0, 0); break; }
        {
            const uint32_t cylinder = c->cylinder;
            advance(c);
            const uint8_t e = check_address(c);
            if (e) { finish(c, e, 1); break; }
            if (c->cylinder != cylinder) {
                /* The heads move on before the host fills the buffer again. */
                const uint64_t t = seek_time(cylinder, c->cylinder);
                move_heads(c, d, c->cylinder, now);
                after(c, S_SEEKED, t);
                break;
            }
            data_phase(c, 0, c->buffer, is_long(c) ? MX_HDC_SECTOR + 4u : MX_HDC_SECTOR, 1);
        }
        break;
    case S_VERIFY:
        for (uint32_t i = 0; i < c->count; ++i) {
            if (i) advance(c);
            const uint8_t e = check_address(c);
            if (e) { finish(c, e, 1); return; }
        }
        move_heads(c, d, c->cylinder, now);
        finish(c, 0, 0);
        break;
    case S_FORMAT: {
        /* One track written with zeros. */
        static const uint8_t zero[MX_HDC_SECTOR] = {0};
        const uint32_t first = (c->cylinder * d->media->heads + c->head) * MX_HDC_SPT;
        for (uint32_t s = 0; s < MX_HDC_SPT; ++s)
            if (!d->media->write(d->media->ctx, first + s, zero)) { c->sector = s; finish(c, MX_HDC_E_WRITE_FAULT, 1); return; }
        if (c->command != MX_HDC_CMD_FORMAT_DRIVE) { finish(c, 0, 0); break; }
        /* Format drive: on to the next track until the end of the drive. */
        const uint32_t cylinder = c->cylinder;
        c->sector = 0;
        if (++c->head >= limit_heads(d) || c->head >= d->media->heads) { c->head = 0; ++c->cylinder; }
        if (c->cylinder >= limit_cylinders(d) || c->cylinder >= d->media->cylinders) {
            c->cylinder = cylinder;
            finish(c, 0, 0);
            break;
        }
        uint64_t t = REVOLUTION;
        if (c->cylinder != cylinder) { t += seek_time(cylinder, c->cylinder); move_heads(c, d, c->cylinder, now + REVOLUTION); }
        after(c, S_FORMAT, t);
        break;
    }
    default:
        break;
    }
}

/* --- the register window ---------------------------------------------------------------------- */

static void controller_reset(mx_hdc *c) {
    c->phase = P_IDLE;
    c->step = S_NONE;
    c->dcb_count = 0;
    c->data_length = c->data_pos = 0;
    c->interrupt = 0;
    for (unsigned u = 0; u < MX_HDC_DRIVES; ++u) {
        c->drive[u].max_cylinders = c->drive[u].max_heads = 0;
        c->drive[u].busy = 0;
        memset(c->drive[u].sense, 0, sizeof c->drive[u].sense);
    }
    mx_dev_schedule(&c->dev, MX_NEVER);
    update_outputs(c);
}

static uint32_t hdc_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    mx_hdc *c = dev;
    (void)space; (void)size;
    switch (offset) {
    case 0:
        if (c->phase == P_DATA_IN && c->data_pos < c->data_length) return data_in(c);
        if (c->phase == P_STATUS) {
            const uint8_t v = c->status_byte;
            c->phase = P_IDLE;
            c->interrupt = 0;
            update_outputs(c);
            return v;
        }
        return 0xFFu;
    case 1: return status(c);
    case 2: return 0x00u;                       /* the drive-type switches: MezinXT's ROM asks with command C0h */
    default: return 0xFFu;
    }
}

static void hdc_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_hdc *c = dev;
    (void)space; (void)size;
    const uint8_t v = (uint8_t)value;
    switch (offset) {
    case 0:
        if (c->phase == P_COMMAND) {
            c->dcb[c->dcb_count++] = v;
            if (c->dcb_count == sizeof c->dcb) {
                c->dcb_count = 0;
                c->phase = P_BUSY;
                start(c);
            }
        } else if (c->phase == P_DATA_OUT && c->data_pos < c->data_length) data_out(c, v);
        break;
    case 1: controller_reset(c); break;
    case 2:
        /* Select: a controller at rest takes a command. */
        if (c->phase == P_IDLE) {
            c->phase = P_COMMAND;
            c->dcb_count = 0;
            c->interrupt = 0;
            update_outputs(c);
        }
        break;
    case 3:
        c->mask = v & 3u;
        update_outputs(c);
        break;
    default: break;
    }
}

static void hdc_reset(void *dev) {
    mx_hdc *c = dev;
    c->mask = 0;
    for (unsigned u = 0; u < MX_HDC_DRIVES; ++u) c->drive[u].cylinder = 0;
    controller_reset(c);
}

static uint8_t hdc_dack(void *dev, int channel, int to_device, uint8_t value, int tc) {
    mx_hdc *c = dev;
    (void)channel; (void)tc;
    if (!c->data_dma || c->data_pos >= c->data_length) return 0xFFu;
    if (to_device && c->phase == P_DATA_OUT) { data_out(c, value); return value; }
    if (!to_device && c->phase == P_DATA_IN) return data_in(c);
    return 0xFFu;
}

static const mx_dev_ops HDC_OPS = {hdc_read, hdc_write, event, hdc_reset, NULL, NULL, hdc_dack};

void mx_hdc_init(mx_hdc *c, const mx_hw_host *host, const char *name) {
    memset(c, 0, sizeof *c);
    c->dev.ops = &HDC_OPS;
    c->dev.host = host;
    c->dev.name = name;
}

int mx_hdc_attach(mx_hdc *c, unsigned unit, const mx_hdc_media *media) {
    if (unit >= MX_HDC_DRIVES) return 0;
    if (media && (media->cylinders < 1 || media->cylinders > 1024u || media->heads < 1 || media->heads > 16u || media->sectors != MX_HDC_SPT ||
                  !media->read || !media->write))
        return 0;
    const int had = c->drive[unit].media != NULL;
    c->drive[unit].media = media;
    c->drive[unit].cylinder = 0;
    if (c->hook && had != (media != NULL)) c->hook(c->hook_ctx, MX_DRIVE_HARD0 + unit, MX_DRIVE_SPINDLE, mx_dev_now(&c->dev), media != NULL, 0);
    return 1;
}

void mx_hdc_set_drive_hook(mx_hdc *c, mx_drive_hook hook, void *ctx) {
    c->hook = hook;
    c->hook_ctx = ctx;
    for (unsigned u = 0; hook && u < MX_HDC_DRIVES; ++u)
        if (c->drive[u].media) hook(ctx, MX_DRIVE_HARD0 + u, MX_DRIVE_SPINDLE, mx_dev_now(&c->dev), 1, 0);
}

unsigned mx_hdc_activity(const mx_hdc *c, uint64_t now, uint64_t hold) {
    unsigned v = 0;
    for (unsigned u = 0; u < MX_HDC_DRIVES; ++u) {
        const mx_hdc_drive *d = &c->drive[u];
        if (d->media && (d->busy || (d->last && now - d->last < hold))) v |= 1u << u;
    }
    return v;
}

/* --- geometry --------------------------------------------------------------------------------- */

static const mx_hdc_type OFFERED[MX_HDC_OFFERED] = {
    {20, 615, 4, "21.4 MB"}, {30, 733, 5, "31.9 MB"}, {40, 977, 5, "42.5 MB"}, {60, 977, 7, "59.5 MB"}, {70, 1024, 8, "71.3 MB"},
};

const mx_hdc_type *mx_hdc_offered(unsigned index) { return index < MX_HDC_OFFERED ? &OFFERED[index] : NULL; }

uint64_t mx_hdc_image_size(uint32_t cylinders, uint32_t heads) { return (uint64_t)cylinders * heads * MX_HDC_SPT * MX_HDC_SECTOR; }

int mx_hdc_geometry(uint64_t size, const uint8_t *mbr, uint32_t *cylinders, uint32_t *heads) {
    const uint64_t track = (uint64_t)MX_HDC_SPT * MX_HDC_SECTOR;
    if (!size || size % track) return 0;
    const uint64_t tracks = size / track;
    for (unsigned i = 0; i < MX_HDC_OFFERED; ++i)
        if (mx_hdc_image_size(OFFERED[i].cylinders, OFFERED[i].heads) == size) {
            *cylinders = OFFERED[i].cylinders;
            *heads = OFFERED[i].heads;
            return 1;
        }
    /* The 10 MB drive of the IBM PC/XT (306 x 4). */
    if (tracks == 306u * 4u) { *cylinders = 306; *heads = 4; return 1; }
    /* A partition table that ends its partitions on the last sector of a track names the heads. */
    if (mbr && mbr[510] == 0x55 && mbr[511] == 0xAA)
        for (unsigned p = 0; p < 4; ++p) {
            const uint8_t *e = mbr + 446 + 16 * p;
            const unsigned h = (unsigned)e[5] + 1u;
            if (!e[4] || (e[6] & 63u) != MX_HDC_SPT || h > 16u || tracks % h || tracks / h > 1024u) continue;
            *cylinders = (uint32_t)(tracks / h);
            *heads = h;
            return 1;
        }
    static const unsigned ORDER[] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 2, 3, 1};
    for (unsigned i = 0; i < sizeof ORDER / sizeof *ORDER; ++i)
        if (tracks % ORDER[i] == 0 && tracks / ORDER[i] <= 1024u && tracks / ORDER[i] >= 2u) {
            *cylinders = (uint32_t)(tracks / ORDER[i]);
            *heads = ORDER[i];
            return 1;
        }
    return 0;
}
