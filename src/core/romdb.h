/* ROM identification. Every file in a roms folder is known by the SHA-256
 * of its content, whatever its name: the four archived FEPER BIOS versions
 * (64 KiB, IC63 on the even bytes, IC64 on the odd ones, at F0000h) and the
 * CGA character ROM (2 KiB, 8x8 glyphs). tools/fetch_roms.py makes them
 * from the archived EPROM dumps under their standard names. */

#ifndef MX_ROMDB_H
#define MX_ROMDB_H

#include <stddef.h>
#include <stdint.h>

#define MX_ROM_PATH 1024

typedef enum mx_rom_kind { MX_ROM_BIOS, MX_ROM_CHARGEN } mx_rom_kind;

typedef struct mx_known_rom {
    const char *key;           /* "bios-4.0", "chargen" */
    mx_rom_kind kind;
    int bios;                  /* MX_BIOS_* (config.h) for a BIOS, -1 otherwise */
    const char *name;          /* "BIOS 4.0" */
    uint32_t size;
    const char *sha256;        /* lower-case hex */
    const char *file;          /* standard file name in roms/ */
} mx_known_rom;

size_t mx_romdb_count(void);
const mx_known_rom *mx_romdb_entry(size_t index);
/* The known ROM with this content, or NULL. */
const mx_known_rom *mx_romdb_identify(const uint8_t *data, size_t size);
const mx_known_rom *mx_romdb_find(const char *key);
const mx_known_rom *mx_romdb_bios(uint32_t bios);       /* MX_BIOS_* */

/* SHA-256 (FIPS 180-4) of data, as 32 bytes and as lower-case hex. */
void mx_sha256(const uint8_t *data, size_t size, uint8_t digest[32]);
void mx_sha256_hex(const uint8_t *data, size_t size, char hex[65]);

/* The files of the roms folders, read once at start and identified by
 * content. A recognised file's content is kept in memory. */
typedef struct mx_rom_file {
    char path[MX_ROM_PATH];
    const char *name;          /* the file name, inside path */
    uint64_t size;
    char sha256[65];           /* empty for files too large to be a ROM */
    const mx_known_rom *rom;   /* NULL: not a known ROM */
    uint8_t *data;             /* a known ROM's content */
    int standard;              /* under its standard file name */
    int duplicate;             /* index of the file used for the same ROM, or -1 */
} mx_rom_file;

typedef struct mx_rom_index {
    mx_rom_file *files;
    size_t count, capacity;
    char folders[3][MX_ROM_PATH];   /* the folders read (those that exist), each with its separator */
    size_t folder_count;
} mx_rom_index;

void mx_rom_index_init(mx_rom_index *index);
/* Reads every file of each folder (missing folders are skipped; the same
 * folder named twice is read once), the files of a folder in name order.
 * Of files holding the same ROM the one under the standard name is used,
 * else the first found. 0 when out of memory. */
int mx_rom_index_scan(mx_rom_index *index, const char *const *folders, size_t count);
/* Adds a file's content as if found under `name` (tests); 0 when out of
 * memory. */
int mx_rom_index_add(mx_rom_index *index, const char *name, const uint8_t *data, size_t size);
void mx_rom_index_free(mx_rom_index *index);
/* The file used for a known ROM, or NULL when it was not found. */
const mx_rom_file *mx_rom_index_find(const mx_rom_index *index, const mx_known_rom *rom);
/* What a file was recognised as, in words. */
const char *mx_rom_file_describe(const mx_rom_index *index, const mx_rom_file *file, char *buffer, size_t capacity);

/* The interleaving of the BIOS EPROMs: IC63 holds the even bytes, IC64 the
 * odd bytes (the 8086's low and high byte lanes). `even` and `odd` are
 * `half` bytes each; out receives 2 x half bytes. */
void mx_rom_interleave(const uint8_t *even, const uint8_t *odd, size_t half, uint8_t *out);

#endif /* MX_ROMDB_H */
