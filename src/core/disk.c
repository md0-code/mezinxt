/* Disk images (disk.h). Portable C11, the files through the host layer.
 * MezinXT's own code. */
#include "disk.h"
#include "text.h"
#include "../host/host.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A changed disk is written back after this much guest time (one second). */
#define FLUSH_DELAY MX_OSC_HZ

struct mx_disk {
    char path[MX_CONFIG_PATH];
    uint8_t *data;
    size_t size;
    mx_fdd_geometry geometry;
    int write_protect;          /* the tab */
    int read_only;              /* the file cannot be written: the tab stays closed */
    int dirty;                  /* the buffer differs from the file */
    int timing;                 /* dirty_at is set */
    uint64_t dirty_at;          /* guest time the disk was first seen changed */
    mx_fdd *drive;              /* while in a drive */
    char error[256];
    /* a fixed disk image: read and written in place */
    int hard;
    mx_host_file *file;
    uint64_t length;
    mx_hdc_media media;         /* its geometry and sector callbacks, for the coupler */
    mx_hdc *hdc;                /* while on the coupler, as unit `unit` */
    unsigned unit;
};

/* --- what an image file is ------------------------------------------------------------------- */

/* Other diskette formats, by size: named in the refusal (the string table's text). */
static const struct { size_t size; int name; } OTHER_SIZES[] = {
    {1228800u, MX_T_DISK_KIND_12},
    {1474560u, MX_T_DISK_KIND_144},
    {2949120u, MX_T_DISK_KIND_288},
};

/* Other image formats, by their first bytes. */
static const struct { const char *magic; size_t length; int name; } FORMATS[] = {
    {"IMD ", 4, MX_T_DISK_KIND_IMD},
    {"TD", 2, MX_T_DISK_KIND_TD0},
    {"td", 2, MX_T_DISK_KIND_TD0},
    {"HxCPICFE", 8, MX_T_DISK_KIND_HFE},
    {"86BF", 4, MX_T_DISK_KIND_86F},
};

static void note(char *error, size_t capacity, const char *format, ...) {
    if (!error || !capacity) return;
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    snprintf(error, capacity, "%s", text);
}

/* A message of the string table (text.h) in the language in use. */
static void note_id(char *error, size_t capacity, int id, ...) {
    if (!error || !capacity) return;
    char text[512];
    va_list args;
    va_start(args, id);
    vsnprintf(text, sizeof text, mx_t(id), args);
    va_end(args);
    snprintf(error, capacity, "%s", text);
}

static const char *recognise_other(const uint8_t *data, size_t size, char *buffer, size_t capacity) {
    for (size_t i = 0; i < sizeof FORMATS / sizeof FORMATS[0]; ++i)
        if (size >= FORMATS[i].length && !memcmp(data, FORMATS[i].magic, FORMATS[i].length)) return mx_t(FORMATS[i].name);
    for (size_t i = 0; i < sizeof OTHER_SIZES / sizeof OTHER_SIZES[0]; ++i)
        if (size == OTHER_SIZES[i].size) return mx_t(OTHER_SIZES[i].name);
    /* A DOS boot sector tells what the image was made for. */
    if (size >= MX_FDD_SECTOR && mx_get16(data + 11) == 512 && data[16] >= 1 && data[16] <= 2) {
        const unsigned total = mx_get16(data + 19);
        if (total) {
            mx_text_format(buffer, capacity, MX_T_DISK_KIND_BPB, total / 2u, total);
            return buffer;
        }
    }
    return NULL;
}

static int all_zero(const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i)
        if (data[i]) return 0;
    return 1;
}

/* --- open, create, close --------------------------------------------------------------------- */

static mx_disk *make(const char *path, uint8_t *data, size_t size, const mx_fdd_geometry *geometry, int write_protect, int read_only) {
    mx_disk *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    snprintf(d->path, sizeof d->path, "%s", path);
    d->data = data;
    d->size = size;
    d->geometry = *geometry;
    d->read_only = read_only;
    d->write_protect = write_protect || read_only;
    return d;
}

mx_disk *mx_disk_open(const char *path, int write_protect, char *error, size_t capacity) {
    uint8_t *data = NULL;
    size_t size = 0;
    int writable = 1;
    if (strlen(path) >= MX_CONFIG_PATH) { note(error, capacity, "%s", MX_TXT(DISK_PATH_TOO_LONG)); return NULL; }
    if (!mx_host_read_file(path, MX_DISK_LIMIT, &data, &size, &writable, error, capacity)) return NULL;
    mx_fdd_geometry geometry;
    if (!mx_fdd_geometry_for_size(size, &geometry)) {
        char what[96];
        const char *other = recognise_other(data, size, what, sizeof what);
        if (other) note_id(error, capacity, MX_T_DISK_OTHER_KIND, path, other);
        else note_id(error, capacity, MX_T_DISK_BAD_SIZE, path, size);
        free(data);
        return NULL;
    }
    mx_disk *d = make(path, data, size, &geometry, write_protect, !writable);
    if (!d) { note(error, capacity, "Out of memory"); free(data); return NULL; }
    note(error, capacity, "");
    return d;
}

mx_disk *mx_disk_create(const char *path, unsigned kib, int overwrite, char *error, size_t capacity) {
    mx_fdd_geometry geometry;
    const size_t size = (size_t)kib * 1024u;
    if (strlen(path) >= MX_CONFIG_PATH) { note(error, capacity, "%s", MX_TXT(DISK_PATH_TOO_LONG)); return NULL; }
    if (!mx_fdd_geometry_for_size(size, &geometry)) {
        note(error, capacity, "%s", MX_TXT(DISK_SIZES));
        return NULL;
    }
    if (!overwrite && mx_host_file_exists(path)) { note_id(error, capacity, MX_T_DISK_EXISTS, path); return NULL; }
    uint8_t *data = calloc(size, 1);
    if (!data) { note(error, capacity, "Out of memory"); return NULL; }
    if (!mx_host_write_file(path, data, size, error, capacity)) { free(data); return NULL; }
    mx_disk *d = make(path, data, size, &geometry, 0, 0);
    if (!d) { note(error, capacity, "Out of memory"); free(data); return NULL; }
    note(error, capacity, "");
    return d;
}

void mx_disk_close(mx_disk *d) {
    if (!d) return;
    mx_disk_release(d);
    mx_host_file_close(d->file);
    free(d->data);
    free(d);
}

/* --- fixed disks ----------------------------------------------------------------------------- */

static int hard_read(void *ctx, uint32_t lba, uint8_t *data) {
    mx_disk *d = ctx;
    const uint64_t at = (uint64_t)lba * MX_HDC_SECTOR;
    return at + MX_HDC_SECTOR <= d->length && mx_host_file_read_at(d->file, at, data, MX_HDC_SECTOR);
}

static int hard_write(void *ctx, uint32_t lba, const uint8_t *data) {
    mx_disk *d = ctx;
    const uint64_t at = (uint64_t)lba * MX_HDC_SECTOR;
    if (d->write_protect || at + MX_HDC_SECTOR > d->length) return 0;
    if (!mx_host_file_write_at(d->file, at, data, MX_HDC_SECTOR)) {
        note_id(d->error, sizeof d->error, MX_T_DISK_HARD_WRITE, d->path);
        return 0;
    }
    d->dirty = 1;
    return 1;
}

mx_disk *mx_disk_open_hard(const char *path, char *error, size_t capacity) {
    int writable = 0;
    if (strlen(path) >= MX_CONFIG_PATH) { note(error, capacity, "%s", MX_TXT(DISK_PATH_TOO_LONG)); return NULL; }
    mx_host_file *f = mx_host_file_open(path, &writable, error, capacity);
    if (!f) return NULL;
    const uint64_t length = mx_host_file_length(f);
    uint8_t mbr[MX_HDC_SECTOR];
    const int have_mbr = length >= MX_HDC_SECTOR && mx_host_file_read_at(f, 0, mbr, sizeof mbr);
    uint32_t cylinders = 0, heads = 0;
    if (!mx_hdc_geometry(length, have_mbr ? mbr : NULL, &cylinders, &heads)) {
        note_id(error, capacity, MX_T_DISK_HARD_BAD_SIZE, path, (unsigned long long)length);
        mx_host_file_close(f);
        return NULL;
    }
    mx_disk *d = calloc(1, sizeof *d);
    if (!d) { note(error, capacity, "Out of memory"); mx_host_file_close(f); return NULL; }
    snprintf(d->path, sizeof d->path, "%s", path);
    d->hard = 1;
    d->file = f;
    d->length = length;
    d->size = (size_t)length;
    d->read_only = !writable;
    d->write_protect = !writable;
    d->media.ctx = d;
    d->media.cylinders = cylinders;
    d->media.heads = heads;
    d->media.sectors = MX_HDC_SPT;
    d->media.read = hard_read;
    d->media.write = hard_write;
    note(error, capacity, "");
    return d;
}

mx_disk *mx_disk_create_hard(const char *path, unsigned type, int overwrite, char *error, size_t capacity) {
    const mx_hdc_type *t = mx_hdc_offered(type);
    if (strlen(path) >= MX_CONFIG_PATH) { note(error, capacity, "%s", MX_TXT(DISK_PATH_TOO_LONG)); return NULL; }
    if (!t) { note(error, capacity, "%s", MX_TXT(DISK_HARD_SIZES)); return NULL; }
    if (!overwrite && mx_host_file_exists(path)) { note_id(error, capacity, MX_T_DISK_EXISTS, path); return NULL; }
    if (!mx_host_file_create(path, mx_hdc_image_size(t->cylinders, t->heads), overwrite, error, capacity)) return NULL;
    return mx_disk_open_hard(path, error, capacity);
}

int mx_disk_is_hard(const mx_disk *d) { return d->hard; }

int mx_disk_insert_hard(mx_disk *d, mx_hdc *hdc, unsigned unit, char *error, size_t capacity) {
    if (!d->hard) { note(error, capacity, "%s", MX_TXT(DISK_NOT_HARD)); return 0; }
    if (!mx_hdc_attach(hdc, unit, &d->media)) { note(error, capacity, "%s", MX_TXT(DISK_HARD_GEOMETRY)); return 0; }
    d->hdc = hdc;
    d->unit = unit;
    return 1;
}

void mx_disk_eject_hard(mx_disk *d, mx_hdc *hdc, unsigned unit) {
    mx_disk_flush(d);
    mx_hdc_attach(hdc, unit, NULL);
    d->hdc = NULL;
}

/* --- state ----------------------------------------------------------------------------------- */

const char *mx_disk_path(const mx_disk *d) { return d->path; }
unsigned mx_disk_kib(const mx_disk *d) { return (unsigned)((d->hard ? d->length : d->size) / 1024u); }

const char *mx_disk_describe(const mx_disk *d, char *buffer, size_t capacity) {
    if (d->hard) {
        const unsigned tenths = (unsigned)((d->length + 50000u) / 100000u);
        mx_text_format(buffer, capacity, MX_T_DISK_HARD_DESCRIBE, tenths / 10u, tenths % 10u, (unsigned)d->media.cylinders,
                       (unsigned)d->media.heads, (unsigned)d->media.sectors);
        return buffer;
    }
    snprintf(buffer, capacity, "%u KiB: %u tracks, %u side%s, %u sectors", mx_disk_kib(d), d->geometry.cylinders, d->geometry.heads,
             d->geometry.heads == 1 ? "" : "s", d->geometry.sectors);
    return buffer;
}

int mx_disk_write_protected(const mx_disk *d) { return d->write_protect; }

int mx_disk_set_write_protect(mx_disk *d, int protect) {
    if (!protect && d->read_only) return 0;
    d->write_protect = protect != 0;
    if (d->drive) d->drive->wprot = (uint8_t)(d->write_protect && d->drive->present);
    return 1;
}

int mx_disk_dirty(const mx_disk *d) { return d->dirty; }
const char *mx_disk_error(const mx_disk *d) { return d->error; }

int mx_disk_flush(mx_disk *d) {
    if (!d->dirty) return 1;
    if (d->hard) {
        /* The sectors are in the file already: the host is asked to put them on its disk. */
        if (!mx_host_file_flush(d->file)) { note_id(d->error, sizeof d->error, MX_T_DISK_HARD_WRITE, d->path); return 0; }
        d->dirty = 0;
        d->timing = 0;
        d->error[0] = 0;
        return 1;
    }
    if (!mx_host_write_file(d->path, d->data, d->size, d->error, sizeof d->error)) return 0;
    d->dirty = 0;
    d->timing = 0;
    d->error[0] = 0;
    if (d->drive) mx_fdd_clear_dirty(d->drive);
    return 1;
}

void mx_disk_tick(mx_disk *d, uint64_t now) {
    if (!d->dirty) { d->timing = 0; return; }
    if (!d->timing) { d->timing = 1; d->dirty_at = now; return; }
    if (now - d->dirty_at < FLUSH_DELAY) return;
    d->dirty_at = now;                  /* a failed write is tried again a second later */
    mx_disk_flush(d);
}

/* --- in a floppy drive ----------------------------------------------------------------------- */

/* The drive wrote a sector or formatted a track. */
static void changed(void *ctx, uint32_t offset, const uint8_t *data, uint32_t length) {
    mx_disk *d = ctx;
    (void)offset; (void)data; (void)length;
    d->dirty = 1;
}

int mx_disk_insert_floppy(mx_disk *d, mx_fdc_card *card, unsigned drive, char *error, size_t capacity) {
    const unsigned flags = (d->write_protect ? MX_FDD_WRITE_PROTECT : 0u) | (all_zero(d->data, d->size) ? MX_FDD_BLANK : 0u);
    if (!mx_fdc_card_insert(card, drive, d->data, d->size, flags, error, capacity)) return 0;
    d->drive = mx_fdc_card_drive(card, drive);
    mx_fdd_set_writeback(d->drive, changed, d);
    return 1;
}

void mx_disk_eject_floppy(mx_disk *d, mx_fdc_card *card, unsigned drive) {
    mx_disk_flush(d);
    mx_fdc_card_eject(card, drive);
    mx_fdd_set_writeback(mx_fdc_card_drive(card, drive), NULL, NULL);
    d->drive = NULL;
}

void mx_disk_release(mx_disk *d) {
    mx_disk_flush(d);
    if (d->drive) mx_fdd_set_writeback(d->drive, NULL, NULL);
    d->drive = NULL;
    d->hdc = NULL;
}

/* --- the configuration's drives -------------------------------------------------------------- */

void mx_disk_close_all(mx_disk *disks[MX_DISK_SLOTS]) {
    for (unsigned i = 0; i < MX_DISK_SLOTS; ++i) {
        mx_disk_close(disks[i]);
        disks[i] = NULL;
    }
}

int mx_disk_is_file(const mx_disk *d, const char *path) {
    const char *a = d->path, *b = path;
    for (; *a && *b; ++a, ++b) {
        char x = *a == '\\' ? '/' : *a, y = *b == '\\' ? '/' : *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return !*a && !*b;
}

int mx_disk_insert_all(mx_machine *machine, mx_disk *const disks[MX_DISK_SLOTS], char *error, size_t capacity) {
    char text[512] = "";
    note(error, capacity, "");
    for (unsigned i = 0; i < MX_CONFIG_DRIVES && i < mx_machine_drives(machine, MX_MEDIA_FLOPPY); ++i) {
        char reason[256] = "";
        if (disks[i] && !mx_machine_insert(machine, MX_MEDIA_FLOPPY, i, disks[i], reason, sizeof reason) && !text[0])
            mx_text_format(text, sizeof text, MX_T_DISK_DRIVE_REASON, (char)('A' + i), reason);
    }
    for (unsigned n = 0; n < MX_CONFIG_HARD; ++n) {
        mx_disk *d = disks[MX_DISK_SLOT_HARD(n)];
        char reason[256] = "";
        /* Without the Winchester coupler the images wait for a machine that has one. */
        if (!d || n >= mx_machine_drives(machine, MX_MEDIA_HARD_DISK)) continue;
        if (!mx_machine_insert(machine, MX_MEDIA_HARD_DISK, n, d, reason, sizeof reason) && !text[0])
            mx_text_format(text, sizeof text, MX_T_DISK_HARD_REASON, n, reason);
    }
    note(error, capacity, "%s", text);
    return !text[0];
}

int mx_disk_insert_configured(mx_machine *machine, const mx_config *config, mx_disk *disks[MX_DISK_SLOTS], char *error, size_t capacity) {
    static const char KEYS[MX_CONFIG_DRIVES] = {'a', 'b', 'c', 'd'};
    char warning[256] = "", text[512];
    for (unsigned i = 0; i < MX_DISK_SLOTS; ++i) disks[i] = NULL;
    note(error, capacity, "");
    for (unsigned i = 0; i < MX_CONFIG_DRIVES && i < mx_machine_drives(machine, MX_MEDIA_FLOPPY); ++i) {
        const mx_drive_config *c = &config->drives[i];
        if (!c->image[0]) continue;
        char reason[256] = "";
        mx_disk *d = NULL;
        for (unsigned k = 0; k < i && !reason[0]; ++k)
            if (disks[k] && mx_disk_is_file(disks[k], c->image)) snprintf(reason, sizeof reason, "the image is already in drive %c", (char)('A' + k));
        if (!reason[0]) d = mx_disk_open(c->image, c->write_protect, reason, sizeof reason);
        if (d && !mx_machine_insert(machine, MX_MEDIA_FLOPPY, i, d, reason, sizeof reason)) { mx_disk_close(d); d = NULL; }
        if (!d) {
            snprintf(text, sizeof text, "drives.%c.image: %s", KEYS[i], reason);
            note(error, capacity, "%s", text);
            for (unsigned k = 0; k < i; ++k) mx_machine_eject(machine, MX_MEDIA_FLOPPY, k);
            mx_disk_close_all(disks);
            return 0;
        }
        disks[i] = d;
        if (reason[0] && !warning[0]) snprintf(warning, sizeof warning, "drives.%c.image: %s", KEYS[i], reason);
    }
    /* The fixed disks, on the Winchester coupler when it is fitted (without it their images wait unused). */
    for (unsigned n = 0; n < MX_CONFIG_HARD && n < mx_machine_drives(machine, MX_MEDIA_HARD_DISK); ++n) {
        const char *image = config->hard[n].image;
        if (!image[0]) continue;
        char reason[256] = "";
        mx_disk *d = NULL;
        for (unsigned k = 0; k < MX_DISK_SLOTS && !reason[0]; ++k)
            if (disks[k] && mx_disk_is_file(disks[k], image)) snprintf(reason, sizeof reason, "the image is in use already");
        if (!reason[0]) d = mx_disk_open_hard(image, reason, sizeof reason);
        if (d && !mx_machine_insert(machine, MX_MEDIA_HARD_DISK, n, d, reason, sizeof reason)) { mx_disk_close(d); d = NULL; }
        if (!d) {
            snprintf(text, sizeof text, "drives.%s.image: %s", mx_config_hard_key(n), reason);
            note(error, capacity, "%s", text);
            for (unsigned k = 0; k < MX_CONFIG_DRIVES; ++k) mx_machine_eject(machine, MX_MEDIA_FLOPPY, k);
            for (unsigned k = 0; k < n; ++k) mx_machine_eject(machine, MX_MEDIA_HARD_DISK, k);
            mx_disk_close_all(disks);
            return 0;
        }
        disks[MX_DISK_SLOT_HARD(n)] = d;
    }
    note(error, capacity, "%s", warning);
    return 1;
}
