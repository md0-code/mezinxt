/* The Winchester coupler against a fake board with the real 8237 on DMA
 * channel 3: a sector written from memory through the command block and DMA
 * reaches the disk, and reading it back returns it; the option ROM's header
 * and checksum. MezinXT's own code. */
#include "hdc.h"
#include "i8237.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long va_ = (a), vb_ = (b); if (va_ != vb_) { printf("FAIL %s:%d: %s = %llx, expected %llx\n", __FILE__, __LINE__, #a, va_, vb_); failures++; } } while (0)

/* --- the fake board --------------------------------------------------------------------------- */

static uint64_t now_t, deadline = MX_NEVER;
static uint8_t irq_level, drq_level;
static mx_hdc hdc;
static mx_i8237 dma;
static uint8_t mem[0x40000];

static uint64_t ms(uint64_t n) { return n * MX_OSC_NUM / (MX_OSC_DEN * 1000u); }

static uint64_t c_now(void *ctx) { (void)ctx; return now_t; }
static void c_schedule(void *ctx, void *dev, uint64_t when) { (void)ctx; (void)dev; deadline = when; }
static void c_line(void *ctx, void *dev, int pin, int state) {
    (void)ctx; (void)dev;
    if (pin == MX_HDC_IRQ) {
        CHECK(irq_level != (state != 0));
        irq_level = (uint8_t)(state != 0);
    } else {
        CHECK(pin == MX_HDC_DRQ);
        CHECK(drq_level != (state != 0));
        drq_level = (uint8_t)(state != 0);
        dma.dev.ops->input(&dma, MX_I8237_DREQ(3), state != 0);
    }
}
static const mx_hw_host hdc_host = {NULL, c_now, c_schedule, c_line, NULL};

static uint64_t d_now(void *ctx) { (void)ctx; return now_t; }
static void d_schedule(void *ctx, void *dev, uint64_t when) { (void)ctx; (void)dev; (void)when; }
static void d_line(void *ctx, void *dev, int pin, int state) { (void)ctx; (void)dev; (void)pin; (void)state; }
static int d_dma(void *ctx, uint32_t address, void *data, uint32_t length, int write) {
    (void)ctx;
    if ((uint64_t)address + length > sizeof mem) return 0;
    if (write) memcpy(mem + address, data, length);
    else memcpy(data, mem + address, length);
    return 1;
}
static const mx_hw_host dma_host = {NULL, d_now, d_schedule, d_line, d_dma};

static void pump(void) {
    for (unsigned i = 0; i < 200000 && mx_i8237_hrq(&dma); i++) mx_i8237_bus(&dma);
}
static int step(uint64_t limit) {
    pump();
    if (deadline > limit) return 0;
    now_t = deadline;
    deadline = MX_NEVER;
    hdc.dev.ops->event(&hdc, now_t);
    pump();
    return 1;
}

static uint8_t in(unsigned port) { return (uint8_t)hdc.dev.ops->read(&hdc, MX_SPACE_IO, port - MX_HDC_IO_BASE, 1); }
static void out(unsigned port, uint8_t v) { hdc.dev.ops->write(&hdc, MX_SPACE_IO, port - MX_HDC_IO_BASE, 1, v); }

/* --- media ------------------------------------------------------------------------------------ */

typedef struct disk {
    mx_hdc_media media;
    uint8_t *data;
} disk;

static uint8_t pattern(unsigned unit, uint32_t lba, unsigned i) { return (uint8_t)(unit * 101u + lba * 7u + i * 13u + (lba >> 8) * 3u + 1u); }

static int m_read(void *ctx, uint32_t lba, uint8_t *data) {
    disk *d = ctx;
    memcpy(data, d->data + (size_t)lba * MX_HDC_SECTOR, MX_HDC_SECTOR);
    return 1;
}
static int m_write(void *ctx, uint32_t lba, const uint8_t *data) {
    disk *d = ctx;
    memcpy(d->data + (size_t)lba * MX_HDC_SECTOR, data, MX_HDC_SECTOR);
    return 1;
}
static void disk_make(disk *d, unsigned unit, uint32_t cylinders, uint32_t heads) {
    memset(d, 0, sizeof *d);
    d->media.ctx = d;
    d->media.cylinders = cylinders;
    d->media.heads = heads;
    d->media.sectors = MX_HDC_SPT;
    d->media.read = m_read;
    d->media.write = m_write;
    const size_t sectors = (size_t)cylinders * heads * MX_HDC_SPT;
    d->data = malloc(sectors * MX_HDC_SECTOR);
    for (uint32_t s = 0; d->data && s < sectors; ++s)
        for (unsigned i = 0; i < MX_HDC_SECTOR; ++i) d->data[(size_t)s * MX_HDC_SECTOR + i] = pattern(unit, s, i);
}
static uint32_t lba_of(const disk *d, uint32_t c, uint32_t h, uint32_t s) { return (c * d->media.heads + h) * MX_HDC_SPT + s; }

/* --- the host's side of the protocol ---------------------------------------------------------- */

/* Waits (in guest time) until (status AND mask) = value; 0 when it does not come within `limit`. */
static int wait_status(uint8_t mask, uint8_t value, uint64_t limit) {
    const uint64_t end = now_t + limit;
    while ((in(0x321) & mask) != value) {
        if (!step(end)) return (in(0x321) & mask) == value;      /* the DMA may have moved the last byte */
    }
    return 1;
}

static void dcb(uint8_t *b, uint8_t command, unsigned unit, uint32_t c, uint32_t h, uint32_t s, uint8_t count) {
    b[0] = command;
    b[1] = (uint8_t)(unit << 5 | h);
    b[2] = (uint8_t)((c >> 8) << 6 | s);
    b[3] = (uint8_t)c;
    b[4] = count;
    b[5] = 5;
}

/* Select and the six bytes; 1 when the coupler took them as it should. */
static int send(const uint8_t *b, uint8_t mask) {
    out(0x323, mask);
    out(0x322, 0);
    if ((in(0x321) & 0x0F) != 0x0D) return 0;
    for (unsigned i = 0; i < 6; ++i) {
        if ((in(0x321) & 0x07) != 0x05) return 0;
        out(0x320, b[i]);
    }
    return 1;
}

/* The completion byte, read when it is offered (-1 when it is not within a second). */
static int completion(void) {
    if (!wait_status(0x0F, 0x0F, ms(1000))) return -1;
    CHECK(in(0x321) & MX_HDC_ST_IRQ);
    const int v = in(0x320);
    CHECK_EQ(in(0x321), 0);
    return v;
}

/* The 8237's channel 3 for a transfer of `bytes` at `address` (mode 47h to memory, 4Bh from memory). */
static void dma_setup(uint8_t mode, uint32_t address, uint32_t bytes) {
    mx_dev *d = &dma.dev;
    d->ops->write(d, MX_SPACE_IO, 0x0C, 1, 0);
    d->ops->write(d, MX_SPACE_IO, 0x0B, 1, mode);
    d->ops->write(d, MX_SPACE_IO, 0x06, 1, address & 0xFF);
    d->ops->write(d, MX_SPACE_IO, 0x06, 1, (address >> 8) & 0xFF);
    mx_dma_page_write(&dma, 2, (uint8_t)(address >> 16));
    d->ops->write(d, MX_SPACE_IO, 0x07, 1, (bytes - 1) & 0xFF);
    d->ops->write(d, MX_SPACE_IO, 0x07, 1, ((bytes - 1) >> 8) & 0xFF);
    d->ops->write(d, MX_SPACE_IO, 0x0A, 1, 3);
}

/* --- the checks ------------------------------------------------------------------------------- */

int main(void) {
    disk d;
    disk_make(&d, 0, 615, 4);
    if (!d.data) { printf("FAIL: out of memory\n"); return 1; }
    now_t = 1000;
    mx_hdc_init(&hdc, &hdc_host, "hdc");
    mx_i8237_init(&dma, &dma_host, "dma");
    dma.dev.ops->reset(&dma);
    mx_i8237_attach(&dma, 3, &hdc.dev);
    hdc.dev.ops->reset(&hdc);
    CHECK(mx_hdc_attach(&hdc, 0, &d.media));

    /* Two sectors written from memory to cylinder 2, head 1 (DMA to the device), then read back to another place. */
    uint8_t b[6];
    for (unsigned i = 0; i < 1024; ++i) mem[0x20000 + i] = (uint8_t)(i * 5 + 3);
    dma_setup(0x4B, 0x20000, 1024);
    dcb(b, MX_HDC_CMD_WRITE, 0, 2, 1, 0, 2);
    CHECK(send(b, MX_HDC_MASK_DMA));
    CHECK_EQ(completion(), 0);
    CHECK(!memcmp(d.data + (size_t)lba_of(&d, 2, 1, 0) * 512, mem + 0x20000, 1024));
    memset(mem + 0x30000, 0, 1024);
    dma_setup(0x47, 0x30000, 1024);
    dcb(b, MX_HDC_CMD_READ, 0, 2, 1, 0, 2);
    CHECK(send(b, MX_HDC_MASK_DMA));
    CHECK_EQ(completion(), 0);
    CHECK(!memcmp(mem + 0x30000, mem + 0x20000, 1024));

    /* The option ROM: header, size, entry, checksum. */
    CHECK_EQ(mx_hdc_rom[0], 0x55);
    CHECK_EQ(mx_hdc_rom[1], 0xAA);
    CHECK_EQ(mx_hdc_rom[2] * 512u, MX_HDC_ROM_SIZE);
    unsigned sum = 0;
    for (unsigned i = 0; i < MX_HDC_ROM_SIZE; ++i) sum += mx_hdc_rom[i];
    CHECK_EQ(sum & 255u, 0);

    free(d.data);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_hdc: all passed\n");
    return 0;
}
