/* ROM identification (romdb.h): SHA-256 as FIPS 180-4 specifies it, the
 * table of known images, and the index of the roms folders. MezinXT's own
 * code. */
#include "romdb.h"
#include "config.h"
#include "../host/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The hashes of the images tools/fetch_roms.py derives from the archived
 * dumps (tools/sources.lock.json): the EPROM pairs interleaved, the
 * character ROM as archived (CHAR_RF5.bin). */
static const mx_known_rom ROMS[] = {
    {"bios-3.0", MX_ROM_BIOS, MX_BIOS_3_0, "BIOS 3.0", 65536u,
     "3e53b7d92f053101127b9da0ea85f81e173c9eb1ec5aec2ad7e045259e1d0fe3", "bios-3.0.rom"},
    {"bios-4.0", MX_ROM_BIOS, MX_BIOS_4_0, "BIOS 4.0", 65536u,
     "d9edcd90a2c279adf2dd2c4a481fdc75ca24decc84bd96d66597fb1a40a0b27f", "bios-4.0.rom"},
    {"bios-5.0", MX_ROM_BIOS, MX_BIOS_5_0, "BIOS 5.0", 65536u,
     "da6f960a91fdeb739950c425d0982f4074b0b7ea000db87a7557a595aa3779f5", "bios-5.0.rom"},
    {"bios-5.0-alt", MX_ROM_BIOS, MX_BIOS_5_0_ALT, "BIOS 5.0 (alternative dump)", 65536u,
     "ba4e79d297f52a9ba90fadd004c86f3d108303512d913dd2baeb2345c31bc3e7", "bios-5.0-alt.rom"},
    {"chargen", MX_ROM_CHARGEN, -1, "CGA character ROM", 2048u,
     "36ec4e835203128d7e06aa48c699b5e2a206dc837031be4a06a1de0179ff0c51", "chargen.rom"},
};
#define ROM_COUNT (sizeof ROMS / sizeof ROMS[0])
/* Nothing larger is read whole. */
#define LARGEST 65536u

size_t mx_romdb_count(void) { return ROM_COUNT; }
const mx_known_rom *mx_romdb_entry(size_t i) { return i < ROM_COUNT ? &ROMS[i] : NULL; }

const mx_known_rom *mx_romdb_find(const char *key) {
    for (size_t i = 0; i < ROM_COUNT; ++i) if (!strcmp(ROMS[i].key, key)) return &ROMS[i];
    return NULL;
}
const mx_known_rom *mx_romdb_bios(uint32_t bios) {
    for (size_t i = 0; i < ROM_COUNT; ++i) if (ROMS[i].kind == MX_ROM_BIOS && ROMS[i].bios == (int)bios) return &ROMS[i];
    return NULL;
}
static const mx_known_rom *by_hash(const char *hex, uint64_t size) {
    for (size_t i = 0; i < ROM_COUNT; ++i) if (ROMS[i].size == size && !strcmp(ROMS[i].sha256, hex)) return &ROMS[i];
    return NULL;
}
const mx_known_rom *mx_romdb_identify(const uint8_t *data, size_t size) {
    char hex[65];
    if (size > LARGEST) return NULL;
    mx_sha256_hex(data, size, hex);
    return by_hash(hex, size);
}

void mx_rom_interleave(const uint8_t *even, const uint8_t *odd, size_t half, uint8_t *out) {
    for (size_t i = 0; i < half; ++i) {
        out[2 * i] = even[i];
        out[2 * i + 1] = odd[i];
    }
}

/* --- SHA-256 (FIPS 180-4, sections 4.1.2, 4.2.2, 5.1.1, 6.2) ---------------------------- */

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

static void block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (unsigned t = 0; t < 16; ++t)
        w[t] = (uint32_t)p[4 * t] << 24 | (uint32_t)p[4 * t + 1] << 16 | (uint32_t)p[4 * t + 2] << 8 | p[4 * t + 3];
    for (unsigned t = 16; t < 64; ++t) {
        const uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        const uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (unsigned t = 0; t < 64; ++t) {
        const uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[t] + w[t];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void mx_sha256(const uint8_t *data, size_t size, uint8_t digest[32]) {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    size_t i = 0;
    for (; size - i >= 64; i += 64) block(h, data + i);
    /* The padding: a 1 bit, zeros, the length in bits as 64 big-endian bits. */
    uint8_t tail[128];
    const size_t rest = size - i, total = rest < 56 ? 64 : 128;
    memset(tail, 0, sizeof tail);
    if (rest) memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    const uint64_t bits = (uint64_t)size * 8u;
    for (unsigned b = 0; b < 8; ++b) tail[total - 1 - b] = (uint8_t)(bits >> (8 * b));
    block(h, tail);
    if (total == 128) block(h, tail + 64);
    for (unsigned b = 0; b < 8; ++b) {
        digest[4 * b] = (uint8_t)(h[b] >> 24);
        digest[4 * b + 1] = (uint8_t)(h[b] >> 16);
        digest[4 * b + 2] = (uint8_t)(h[b] >> 8);
        digest[4 * b + 3] = (uint8_t)h[b];
    }
}

void mx_sha256_hex(const uint8_t *data, size_t size, char hex[65]) {
    static const char digits[] = "0123456789abcdef";
    uint8_t d[32];
    mx_sha256(data, size, d);
    for (unsigned i = 0; i < 32; ++i) {
        hex[2 * i] = digits[d[i] >> 4];
        hex[2 * i + 1] = digits[d[i] & 15];
    }
    hex[64] = 0;
}

/* --- the index ------------------------------------------------------------------- */

void mx_rom_index_init(mx_rom_index *ix) { memset(ix, 0, sizeof *ix); }

static mx_rom_file *new_file(mx_rom_index *ix) {
    if (ix->count == ix->capacity) {
        const size_t capacity = ix->capacity ? ix->capacity * 2 : 16;
        mx_rom_file *files = realloc(ix->files, capacity * sizeof *files);
        if (!files) return NULL;
        ix->files = files;
        ix->capacity = capacity;
    }
    mx_rom_file *f = &ix->files[ix->count];
    memset(f, 0, sizeof *f);
    f->duplicate = -1;
    return f;
}

/* Identifies the newest file and settles which of the same ROM is used. */
static void settle(mx_rom_index *ix, mx_rom_file *f, const uint8_t *data) {
    if (f->size <= LARGEST && data) {
        mx_sha256_hex(data, (size_t)f->size, f->sha256);
        f->rom = by_hash(f->sha256, f->size);
    }
    if (!f->rom) return;
    f->standard = !strcmp(f->name, f->rom->file);
    f->data = malloc((size_t)f->size);
    if (f->data) memcpy(f->data, data, (size_t)f->size);
    else f->rom = NULL;
    const size_t self = (size_t)(f - ix->files);
    for (size_t i = 0; f->rom && i < self; ++i) {
        mx_rom_file *o = &ix->files[i];
        if (o->rom != f->rom || o->duplicate >= 0) continue;
        if (f->standard && !o->standard) o->duplicate = (int)self;   /* the standard name wins */
        else f->duplicate = (int)i;
        break;
    }
    /* Duplicates of a file that just lost point to the winner. */
    for (size_t i = 0; i < self; ++i)
        if (ix->files[i].duplicate >= 0 && ix->files[ix->files[i].duplicate].duplicate >= 0)
            ix->files[i].duplicate = ix->files[ix->files[i].duplicate].duplicate;
}

int mx_rom_index_add(mx_rom_index *ix, const char *name, const uint8_t *data, size_t size) {
    mx_rom_file *f = new_file(ix);
    if (!f) return 0;
    snprintf(f->path, sizeof f->path, "%s", name);
    f->name = f->path;
    f->size = size;
    ix->count++;
    settle(ix, f, data);
    return 1;
}

typedef struct names { char (*list)[256]; size_t count, capacity; int failed; } names;
static int collect(void *ctx, const char *name) {
    names *n = ctx;
    if (strlen(name) >= 256) return 1;
    if (n->count == n->capacity) {
        const size_t capacity = n->capacity ? n->capacity * 2 : 32;
        char (*list)[256] = realloc(n->list, capacity * sizeof *list);
        if (!list) { n->failed = 1; return 0; }
        n->list = list;
        n->capacity = capacity;
    }
    snprintf(n->list[n->count++], 256, "%s", name);
    return 1;
}
static int by_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* 1 read, 0 out of memory, -1 no such folder. */
static int scan_folder(mx_rom_index *ix, const char *folder) {
    names n = {NULL, 0, 0, 0};
    if (!mx_host_list_files(folder, collect, &n)) { free(n.list); return -1; }
    if (n.failed) { free(n.list); return 0; }
    if (n.count) qsort(n.list, n.count, sizeof *n.list, by_name);
    uint8_t *buffer = malloc(LARGEST);
    int ok = buffer != NULL;
    for (size_t i = 0; ok && i < n.count; ++i) {
        mx_rom_file *f = new_file(ix);
        if (!f) { ok = 0; break; }
        if (snprintf(f->path, sizeof f->path, "%s%s", folder, n.list[i]) >= (int)sizeof f->path) continue;
        f->name = f->path + strlen(folder);
        FILE *file = fopen(f->path, "rb");
        if (!file) continue;
        long size = -1;
        if (!fseek(file, 0, SEEK_END)) size = ftell(file);
        int whole = 0;
        if (size >= 0) {
            f->size = (uint64_t)size;
            whole = size <= (long)LARGEST && !fseek(file, 0, SEEK_SET) && fread(buffer, 1, (size_t)size, file) == (size_t)size;
        }
        fclose(file);
        if (size < 0) continue;
        ix->count++;
        settle(ix, f, whole ? buffer : NULL);
    }
    free(buffer);
    free(n.list);
    return ok;
}

int mx_rom_index_scan(mx_rom_index *ix, const char *const *folders, size_t count) {
    for (size_t k = 0; k < count && ix->folder_count < 3; ++k) {
        if (!folders[k] || !folders[k][0]) continue;
        int seen = 0;
        for (size_t j = 0; j < ix->folder_count; ++j) seen |= mx_host_same_folder(ix->folders[j], folders[k]);
        if (seen) continue;
        char *folder = ix->folders[ix->folder_count++];
        const size_t n = strlen(folders[k]);
        const int separated = folders[k][n - 1] == '/' || folders[k][n - 1] == '\\';
        snprintf(folder, MX_ROM_PATH, "%s%s", folders[k], separated ? "" : (char[2]){mx_host_separator(), 0});
        const int read = scan_folder(ix, folder);
        if (!read) return 0;
        if (read < 0) ix->folder_count--;      /* missing: not one of the folders */
    }
    return 1;
}

void mx_rom_index_free(mx_rom_index *ix) {
    for (size_t i = 0; i < ix->count; ++i) free(ix->files[i].data);
    free(ix->files);
    memset(ix, 0, sizeof *ix);
}

const mx_rom_file *mx_rom_index_find(const mx_rom_index *ix, const mx_known_rom *rom) {
    if (!ix || !rom) return NULL;
    for (size_t i = 0; i < ix->count; ++i)
        if (ix->files[i].rom == rom && ix->files[i].duplicate < 0 && ix->files[i].data) return &ix->files[i];
    return NULL;
}

const char *mx_rom_file_describe(const mx_rom_index *ix, const mx_rom_file *f, char *buffer, size_t capacity) {
    if (!f->rom) {
        if (!f->sha256[0]) snprintf(buffer, capacity, "unknown (%llu bytes, too large for a ROM)", (unsigned long long)f->size);
        else snprintf(buffer, capacity, "unknown (%llu bytes, SHA-256 %.16s...)", (unsigned long long)f->size, f->sha256);
    } else if (f->duplicate >= 0)
        snprintf(buffer, capacity, "%s, a copy of %s (not used)", f->rom->name, ix->files[f->duplicate].name);
    else if (!f->standard) snprintf(buffer, capacity, "%s (standard name %s)", f->rom->name, f->rom->file);
    else snprintf(buffer, capacity, "%s", f->rom->name);
    return buffer;
}
