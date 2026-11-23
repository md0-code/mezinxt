/* MezinXT - the floppy drive and its disk image (fdd.h). Portable C11.
 * MezinXT's own code. */
#include "fdd.h"

#include <stdio.h>
#include <string.h>

#define DEFAULT_SPINUP_US 400000u

static const struct { size_t size; mx_fdd_geometry g; } SIZES[] = {
    {163840u, {40, 1, 8}},      /* 160 KiB */
    {184320u, {40, 1, 9}},      /* 180 KiB */
    {327680u, {40, 2, 8}},      /* 320 KiB: the Junior's format */
    {368640u, {40, 2, 9}},      /* 360 KiB */
    {737280u, {80, 2, 9}},      /* 720 KiB */
};

static void note(char *error, size_t capacity, const char *text) {
    if (error && capacity) snprintf(error, capacity, "%s", text);
}

void mx_fdd_init(mx_fdd *f, unsigned installed, unsigned tracks) {
    memset(f, 0, sizeof *f);
    f->installed = installed != 0;
    f->tracks = tracks == 80 ? 80 : 40;
    f->spinup = (uint32_t)mx_fdd_us(DEFAULT_SPINUP_US);
}

void mx_fdd_set_writeback(mx_fdd *f, mx_fdd_writeback callback, void *ctx) {
    f->writeback = callback;
    f->writeback_ctx = ctx;
}

int mx_fdd_geometry_for_size(size_t size, mx_fdd_geometry *geometry) {
    for (unsigned i = 0; i < sizeof SIZES / sizeof SIZES[0]; i++) {
        if (SIZES[i].size == size) {
            if (geometry) *geometry = SIZES[i].g;
            return 1;
        }
    }
    return 0;
}

int mx_fdd_check_bpb(const uint8_t *image, size_t size, const mx_fdd_geometry *geometry) {
    if (size < MX_FDD_SECTOR) return MX_FDD_BPB_NONE;
    const unsigned bytes = mx_get16(image + 11), reserved = mx_get16(image + 14), media = image[21];
    const unsigned spc = image[13], fats = image[16];
    const unsigned total = mx_get16(image + 19), spt = mx_get16(image + 24), heads = mx_get16(image + 26);
    /* A boot sector with no BPB (DOS 1.x, a game's own loader) has code or
     * text here; only a block that looks like one is checked. */
    if (bytes != 512 || spc == 0 || (spc & (spc - 1)) || reserved == 0 || fats < 1 || fats > 2 || media < 0xF0) return MX_FDD_BPB_NONE;
    if ((spt == 8 || spt == 9) && (heads == 1 || heads == 2) && spt == geometry->sectors && heads == geometry->heads &&
        (total == 0 || total == (unsigned)geometry->cylinders * geometry->heads * geometry->sectors)) return MX_FDD_BPB_MATCH;
    return MX_FDD_BPB_MISMATCH;
}

int mx_fdd_insert(mx_fdd *f, uint8_t *image, size_t size, unsigned flags, char *error, size_t capacity) {
    mx_fdd_geometry g;
    note(error, capacity, "");
    if (!f->installed) { note(error, capacity, "The drive is not installed"); return 0; }
    if (!image || !mx_fdd_geometry_for_size(size, &g)) {
        note(error, capacity, "The image size is not that of a 160, 180, 320, 360 or 720 KiB diskette");
        return 0;
    }
    if (g.cylinders > f->tracks) {
        note(error, capacity, "The image has 80 tracks and the drive 40");
        return 0;
    }
    f->image = image;
    f->size = size;
    f->geom = g;
    f->wprot = (flags & MX_FDD_WRITE_PROTECT) != 0;
    f->dirty = 0;
    memset(f->deleted, 0, sizeof f->deleted);
    memset(f->formatted, (flags & MX_FDD_BLANK) ? 0 : 1, sizeof f->formatted);
    for (unsigned c = 0; c < MX_FDD_MAX_CYL; c++)
        for (unsigned h = 0; h < 2; h++)
            for (unsigned s = 0; s < MX_FDD_MAX_SPT; s++) f->order[c][h][s] = (uint8_t)(s + 1);
    f->bpb = (uint8_t)mx_fdd_check_bpb(image, size, &g);
    if (f->bpb == MX_FDD_BPB_MISMATCH)
        note(error, capacity, "The boot sector's BIOS parameter block disagrees with the image size; the size decides");
    f->present = 1;
    f->changed = 1;
    return 1;
}

uint8_t *mx_fdd_eject(mx_fdd *f) {
    uint8_t *image = f->present ? f->image : NULL;
    if (f->present) f->changed = 1;
    f->present = 0;
    f->image = NULL;
    f->size = 0;
    f->wprot = 0;
    f->dirty = 0;
    f->bpb = MX_FDD_BPB_NONE;
    memset(&f->geom, 0, sizeof f->geom);
    return image;
}

void mx_fdd_set_motor(mx_fdd *f, int on, uint64_t now) {
    if (!f->installed) return;
    const uint8_t was = f->motor;
    if (on && !f->motor) f->speed_at = now + f->spinup;
    f->motor = on != 0;
    if (f->motor != was) mx_fdd_notify(f, MX_DRIVE_MOTOR, now, f->motor, 0);
}

int mx_fdd_at_speed(const mx_fdd *f, uint64_t now) { return f->installed && f->motor && now >= f->speed_at; }
int mx_fdd_rotating(const mx_fdd *f, uint64_t now) { return mx_fdd_at_speed(f, now) && f->present; }
uint64_t mx_fdd_rotating_at(const mx_fdd *f) { return (f->installed && f->motor && f->present) ? f->speed_at : MX_NEVER; }

int mx_fdd_index(const mx_fdd *f, uint64_t now) {
    return mx_fdd_rotating(f, now) && (now - f->speed_at) % MX_FDD_REV < mx_fdd_us(MX_FDD_INDEX_PULSE_US);
}

unsigned mx_fdd_step(mx_fdd *f, int dir, uint64_t now) {
    if (!f->installed) return 0;
    const uint8_t before = f->cyl;
    if (dir < 0) { if (f->cyl > 0) f->cyl--; }
    else if (f->cyl < f->tracks + 1u) f->cyl++;
    if (f->present) f->changed = 0;
    mx_fdd_notify(f, MX_DRIVE_STEP, now, dir > 0, f->cyl == before);
    return f->cyl;
}

uint64_t mx_fdd_next_index(const mx_fdd *f, uint64_t from) {
    if (from <= f->speed_at) return f->speed_at;
    const uint64_t off = from - f->speed_at;
    return f->speed_at + (off + MX_FDD_REV - 1u) / MX_FDD_REV * MX_FDD_REV;
}

/* The track under a head, when it exists on the disk and has been formatted. */
static int track_ok(const mx_fdd *f, unsigned head) {
    return f->installed && f->present && head < f->geom.heads && f->cyl < f->geom.cylinders && f->formatted[f->cyl][head];
}

int mx_fdd_find_id(const mx_fdd *f, unsigned head, uint64_t from, uint64_t *slot_time, mx_fdd_id *id) {
    if (!f->motor || !track_ok(f, head)) return -1;
    if (from < f->speed_at) from = f->speed_at;
    const uint64_t off = from - f->speed_at;
    uint64_t rev = off / MX_FDD_REV;
    const uint64_t in = off % MX_FDD_REV;
    const unsigned spt = f->geom.sectors;
    unsigned k = 0;
    while (k < spt && mx_fdd_bytes(MX_FDD_PRE + k * MX_FDD_SLOT + MX_FDD_ID_MARK) < in) k++;
    if (k == spt) { k = 0; rev++; }
    *slot_time = f->speed_at + rev * MX_FDD_REV + mx_fdd_bytes(MX_FDD_PRE + k * MX_FDD_SLOT);
    id->c = f->cyl;
    id->h = (uint8_t)head;
    id->r = f->order[f->cyl][head][k];
    id->n = 2;
    return (int)k;
}

static size_t sector_offset(const mx_fdd *f, unsigned head, unsigned r) {
    return (((size_t)f->cyl * f->geom.heads + head) * f->geom.sectors + (r - 1u)) * MX_FDD_SECTOR;
}

int mx_fdd_read_sector(const mx_fdd *f, unsigned head, unsigned r, uint8_t *out, int *deleted) {
    if (!f->installed || !f->present) return MX_FDD_NO_DISK;
    if (!track_ok(f, head) || r < 1 || r > f->geom.sectors) return MX_FDD_NO_SECTOR;
    memcpy(out, f->image + sector_offset(f, head, r), MX_FDD_SECTOR);
    if (deleted) *deleted = (f->deleted[f->cyl][head] >> (r - 1u)) & 1u;
    return MX_FDD_OK;
}

static void changed_range(mx_fdd *f, size_t offset, size_t length) {
    f->dirty = 1;
    if (f->writeback) f->writeback(f->writeback_ctx, (uint32_t)offset, f->image + offset, (uint32_t)length);
}

int mx_fdd_write_sector(mx_fdd *f, unsigned head, unsigned r, const uint8_t *data, int deleted) {
    if (!f->installed || !f->present) return MX_FDD_NO_DISK;
    if (f->wprot) return MX_FDD_PROTECTED;
    if (!track_ok(f, head) || r < 1 || r > f->geom.sectors) return MX_FDD_NO_SECTOR;
    const size_t offset = sector_offset(f, head, r);
    memcpy(f->image + offset, data, MX_FDD_SECTOR);
    if (deleted) f->deleted[f->cyl][head] |= (uint16_t)(1u << (r - 1u));
    else f->deleted[f->cyl][head] &= (uint16_t)~(1u << (r - 1u));
    changed_range(f, offset, MX_FDD_SECTOR);
    return MX_FDD_OK;
}

int mx_fdd_format_track(mx_fdd *f, unsigned head, const mx_fdd_id *ids, unsigned count, uint8_t fill) {
    if (!f->installed || !f->present) return MX_FDD_NO_DISK;
    if (f->wprot) return MX_FDD_PROTECTED;
    if (head >= f->geom.heads || f->cyl >= f->geom.cylinders || count != f->geom.sectors) return MX_FDD_BAD_FORMAT;
    uint16_t seen = 0;
    for (unsigned i = 0; i < count; i++) {
        if (ids[i].c != f->cyl || ids[i].h != head || ids[i].n != 2 || ids[i].r < 1 || ids[i].r > count) return MX_FDD_BAD_FORMAT;
        if (seen & (1u << (ids[i].r - 1u))) return MX_FDD_BAD_FORMAT;
        seen |= (uint16_t)(1u << (ids[i].r - 1u));
    }
    for (unsigned i = 0; i < count; i++) f->order[f->cyl][head][i] = ids[i].r;
    f->formatted[f->cyl][head] = 1;
    f->deleted[f->cyl][head] = 0;
    const size_t offset = sector_offset(f, head, 1);
    memset(f->image + offset, fill, (size_t)count * MX_FDD_SECTOR);
    changed_range(f, offset, (size_t)count * MX_FDD_SECTOR);
    return MX_FDD_OK;
}
