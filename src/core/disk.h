/* Disk images: a file held in memory as the drive's
 * medium, written back to the file when it changes. The machine interface
 * (machine.h) borrows these: the caller opens an image, hands it to
 * mx_machine_insert, and closes it after the machine has let it go
 * (mx_machine_eject, mx_machine_destroy).
 *
 * Floppy images are raw sector dumps, cylinder by cylinder, head 0 then
 * head 1, 512 bytes a sector, recognised by their size: 160 KiB (40 x 1 x 8),
 * 180 KiB (40 x 1 x 9), 320 KiB (40 x 2 x 8, the Junior's format), 360 KiB
 * (40 x 2 x 9) and 720 KiB (80 x 2 x 9, for an 80-track drive). The boot
 * sector's BIOS parameter block is checked against the size (a disagreement
 * is a warning, the size decides). Sizes of other diskette formats and files
 * of other image formats are named in the refusal. An image that is all zero
 * is a blank disk: its tracks have no sectors until FORMAT writes them.
 *
 * Write-back. A write of the guest changes the buffer at once; the image
 * file is brought up to date by mx_disk_flush, which the machine calls when
 * the disk is ejected, when it is reset or destroyed, and once the disk has
 * stood changed for a second of guest time (mx_disk_tick): a file is
 * replaced whole or not at all (host.h). A failed write-back keeps the disk
 * changed, says why in mx_disk_error and is tried again. The write
 * protect tab is the caller's choice, forced on for a read-only file; it can
 * be flipped while the disk is in a drive.
 *
 * Fixed disk images (the Winchester coupler's drives, hw/hdc.h) are raw
 * sector dumps too, 17 sectors a track, cylinder by cylinder and head by
 * head, their geometry from the size (mx_hdc_geometry: the drives offered,
 * a partition table's heads, else a fit). They are never held whole: the
 * coupler reads and writes them a sector at a time in place, and the write
 * back only makes the host flush what was written. A read-only file is
 * protected: its writes fail as write faults.
 *
 * File access is the host layer's (host.h). Portable C11. MezinXT's own
 * code. */
#ifndef MX_DISK_H
#define MX_DISK_H

#include "config.h"
#include "machine.h"
#include "../hw/fdc_card.h"
#include "../hw/hdc.h"

typedef struct mx_disk mx_disk;

/* The open images a shell keeps: the diskette drives' (0 to 3) and the fixed disks' (MX_DISK_SLOT_HARD). */
#define MX_DISK_SLOTS (MX_CONFIG_DRIVES + MX_CONFIG_HARD)
#define MX_DISK_SLOT_HARD(n) (MX_CONFIG_DRIVES + (n))

/* The largest image file read (a diskette of another format is named, not loaded). */
#define MX_DISK_LIMIT (4u * 1024u * 1024u)

/* Reads an image file. `write_protect` closes the tab. NULL with the reason
 * in `error` (not there, an unknown size or format, out of memory). */
mx_disk *mx_disk_open(const char *path, int write_protect, char *error, size_t capacity);
/* A new blank (unformatted) disk of 160, 180, 320, 360 or 720 KiB, written
 * to `path`. An existing file is only replaced when `overwrite` is set. */
mx_disk *mx_disk_create(const char *path, unsigned kib, int overwrite, char *error, size_t capacity);
/* A fixed disk image (the size names the geometry); NULL with the reason. */
mx_disk *mx_disk_open_hard(const char *path, char *error, size_t capacity);
/* A new blank fixed disk of an offered type (hdc.h mx_hdc_offered), all zero
 * (a sparse file where the file system has them). An existing file is only
 * replaced when `overwrite` is set. */
mx_disk *mx_disk_create_hard(const char *path, unsigned type, int overwrite, char *error, size_t capacity);
/* 1 for a fixed disk image. */
int mx_disk_is_hard(const mx_disk *disk);
/* Writes back what changed, then frees the disk (it must be out of its
 * drive: the machine lets go of it on eject and destroy). */
void mx_disk_close(mx_disk *disk);

const char *mx_disk_path(const mx_disk *disk);
unsigned mx_disk_kib(const mx_disk *disk);
/* "320 KiB: 40 tracks, 2 sides, 8 sectors" (a fixed disk: "21.4 MB: 615 cylinders, 4 heads, 17 sectors"), into `buffer`. */
const char *mx_disk_describe(const mx_disk *disk, char *buffer, size_t capacity);
int mx_disk_write_protected(const mx_disk *disk);
/* Opens or closes the tab, in a drive too. 0 when a read-only file cannot be
 * unprotected. */
int mx_disk_set_write_protect(mx_disk *disk, int protect);
/* The disk differs from its file. */
int mx_disk_dirty(const mx_disk *disk);
/* 1 when the file is up to date (or was not changed); 0 when it could not be
 * written (mx_disk_error says why). */
int mx_disk_flush(mx_disk *disk);
/* The reason the last write-back failed; empty when it did not. */
const char *mx_disk_error(const mx_disk *disk);
/* Periodic write-back by guest time `now` (OSC periods): flushes a disk that
 * has been changed for a second. */
void mx_disk_tick(mx_disk *disk, uint64_t now);

/* In a floppy drive of the diskette adapter: the disk goes in (1; `error`
 * may carry a warning) or not (0, with the reason). Its changes then reach
 * the file; eject takes it out again, flushed. The caller checks the drive
 * is free and the disk not already in one. */
int mx_disk_insert_floppy(mx_disk *disk, mx_fdc_card *card, unsigned drive, char *error, size_t capacity);
void mx_disk_eject_floppy(mx_disk *disk, mx_fdc_card *card, unsigned drive);
/* On the Winchester coupler as fixed disk `unit` (0 or 1): 1, or 0 with the
 * reason. Its writes then reach the file; eject takes it off again, flushed. */
int mx_disk_insert_hard(mx_disk *disk, mx_hdc *hdc, unsigned unit, char *error, size_t capacity);
void mx_disk_eject_hard(mx_disk *disk, mx_hdc *hdc, unsigned unit);
/* The machine lets go of the disk without its drive (the adapter is going
 * away): flushed and detached. */
void mx_disk_release(mx_disk *disk);

/* The images the configuration names (drives.a.image to drives.d.image, and
 * drives.hd0.image and hd1 when the machine has the Winchester coupler), opened
 * and put into the machine's drives; disks[slot] are the open images (NULL for
 * an empty drive), for mx_disk_close_all after the machine is destroyed.
 * 0 with a message naming the key; on success `error` may carry a warning
 * (empty when none). */
int mx_disk_insert_configured(mx_machine *machine, const mx_config *config, mx_disk *disks[MX_DISK_SLOTS], char *error, size_t capacity);
void mx_disk_close_all(mx_disk *disks[MX_DISK_SLOTS]);
/* The open disks put into a new machine's drives (after the old machine let go
 * of them). 0 with a message naming the drive when one cannot go in; the others
 * still do. */
int mx_disk_insert_all(mx_machine *machine, mx_disk *const disks[MX_DISK_SLOTS], char *error, size_t capacity);
/* 1 when `path` names the file this disk was read from (by name, ignoring
 * case and the kind of slash): two open copies of one file would overwrite
 * each other's changes. */
int mx_disk_is_file(const mx_disk *disk, const char *path);

#endif /* MX_DISK_H */
