/* MezinXT - the 5.25" floppy drive and its disk image. A plain struct, not a chip: the diskette adapter (fdc_card.h)
 * owns up to four of them and the controller (upd765.h) reaches them through
 * it. A drive never schedules anything; it answers questions about the head,
 * the spindle and the disk at a given time, so everything stays event driven
 * in the controller.
 *
 * The drive: 40 or 80 cylinders, two heads (a double-sided drive), step in
 * and out, the TRK00, write-protect and index signals, the spindle motor.
 * The head reaches cylinders 0 to tracks + 1 (the stepper's end stops); a
 * 40-track disk in an 80-track drive is read at cylinders 0-39 with no
 * double stepping. The motor reaches full speed `spinup` OSC periods after it
 * is switched on (default 400 ms) and stops at once when switched off. The
 * disk turns at 300 rpm, 200 ms a revolution, and its index hole passes the
 * sensor at the instant the spindle reaches speed and every revolution after.
 * A drive without a disk delivers no index pulses and no data, as a real one
 * (the sensor sees no hole); neither does a stopped one.
 *
 * The disk image is the caller's buffer (borrowed, as src/core/machine.h
 * has it: the drive uses it until it is ejected, then the caller closes it):
 * a raw sector dump, cylinder by cylinder, head 0 then head 1 on each
 * cylinder, sectors 1..n in order, 512 bytes each. The geometry comes from
 * the size: 160 KiB 40 x 1 x 8, 180 KiB 40 x 1 x 9, 320 KiB 40 x 2 x 8
 * (the Junior's format, BIOS 4.0's table has EOT = 8), 360 KiB 40 x 2 x 9,
 * 720 KiB 80 x 2 x 9 (80-track drives only). When the boot sector carries a
 * BIOS parameter block it is checked against that geometry (the result is
 * mx_fdd.bpb); the physical layout is always the size's. A write goes into
 * the buffer at once, sets the dirty flag and calls the write-back callback
 * with the changed range of the image, so the host can keep the file in step.
 * A write-protected disk refuses writes. Inserting or ejecting sets the
 * disk-change latch (a drive with the change line, which the IBM PC/XT
 * adapter does not have, clears it on its next step pulse with a disk in).
 *
 * The track as the controller sees it (250 kbit/s MFM, 6250 raw bytes of
 * 32 us a track, IBM format): after the index, 146 bytes of gap, sync and
 * index mark; then one 654-byte slot per sector: the ID field (sync, address
 * mark, C H R N, CRC: 22 bytes), gap 2 (22), the data field (sync, address
 * mark, 512 data bytes, CRC: 530) and gap 3 (80). A track normally holds its
 * sectors in order 1..n (interleave 1); formatting a track with another R
 * order (an interleave) is kept, the data in the image stays in sector-number
 * order. A raw image has no way to hold other IDs or sizes, so a format that
 * writes C or H different from the track's, N other than 2, or a different
 * number of sectors than the image has is refused. A track of a blank image
 * (MX_FDD_BLANK) has no sectors until it is formatted. Each sector also
 * remembers whether it carries a deleted data mark (not part of the image
 * file: it is lost when the disk is ejected).
 *
 * Time is in OSC periods (hw.h). Portable C11. MezinXT's own code. */
#ifndef MX_FDD_H
#define MX_FDD_H

#include <stddef.h>
#include <stdint.h>

#include "hw.h"

/* OSC periods of `us` microseconds (rounded up): the crystal is
 * 157.5/11 MHz, so one microsecond is 315/22 periods. */
static inline uint64_t mx_fdd_us(uint64_t us) { return (us * 315u + 21u) / 22u; }
/* A raw byte of the track: 32 us at 250 kbit/s MFM. */
#define MX_FDD_BYTE_US 32u
static inline uint64_t mx_fdd_bytes(uint64_t n) { return mx_fdd_us(n * MX_FDD_BYTE_US); }

#define MX_FDD_TRACK_BYTES 6250u
#define MX_FDD_REV         2863637u     /* one revolution, 200 ms: mx_fdd_bytes(6250) */
/* Positions, in raw bytes after the index, inside the track. */
#define MX_FDD_PRE         146u         /* gap 4a, sync, index mark, gap 1 */
#define MX_FDD_SLOT        654u         /* one sector: ID field, gap 2, data field, gap 3 */
#define MX_FDD_ID_MARK     12u          /* where the ID's sync starts: the latest a read can lock on */
#define MX_FDD_ID_END      22u          /* after the ID's CRC */
#define MX_FDD_DATA        60u          /* the first data byte */
#define MX_FDD_DATA_END    574u         /* after the 512 data bytes and their CRC */

#define MX_FDD_MAX_CYL 84
#define MX_FDD_MAX_SPT 9
#define MX_FDD_SECTOR  512u

/* mx_fdd_insert flags. */
enum { MX_FDD_WRITE_PROTECT = 1, MX_FDD_BLANK = 2 };
/* mx_fdd.bpb: what the boot sector's BIOS parameter block says. */
enum { MX_FDD_BPB_NONE, MX_FDD_BPB_MATCH, MX_FDD_BPB_MISMATCH };
/* Results of the sector operations. */
enum { MX_FDD_OK, MX_FDD_NO_DISK, MX_FDD_NO_SECTOR, MX_FDD_PROTECTED, MX_FDD_BAD_FORMAT };

typedef struct mx_fdd_geometry { uint8_t cylinders, heads, sectors; } mx_fdd_geometry;
typedef struct mx_fdd_id { uint8_t c, h, r, n; } mx_fdd_id;

/* The image range [offset, offset + length) of the disk has changed; `data`
 * is its new content. Called after every sector write and track format. */
typedef void (*mx_fdd_writeback)(void *ctx, uint32_t offset, const uint8_t *data, uint32_t length);

typedef struct mx_fdd {
    /* The drive. */
    uint8_t installed;
    uint8_t tracks;                 /* 40 or 80 */
    uint8_t cyl;                    /* where the head is */
    uint8_t motor;
    uint32_t spinup;                /* OSC periods from motor on to full speed */
    uint64_t speed_at;              /* motor on: when the spindle is at speed (the index epoch) */
    mx_drive_hook hook;             /* the mechanical events (hw.h), for the drive sound; NULL: none */
    void *hook_ctx;
    uint8_t id;                     /* the number the hook is given for this drive (MX_DRIVE_FLOPPY0 + n) */
    /* The disk. */
    uint8_t present, wprot, changed, dirty, bpb;
    mx_fdd_geometry geom;
    uint8_t *image;
    size_t size;
    mx_fdd_writeback writeback;
    void *writeback_ctx;
    uint8_t order[MX_FDD_MAX_CYL][2][MX_FDD_MAX_SPT];   /* the sector number at each physical slot */
    uint8_t formatted[MX_FDD_MAX_CYL][2];
    uint16_t deleted[MX_FDD_MAX_CYL][2];                /* bit r-1: sector r has a deleted data mark */
} mx_fdd;

/* Sets the drive up (empty, motor off, head at cylinder 0). `tracks` is 40
 * or 80; an uninstalled drive does nothing (no index, no TRK00). */
void mx_fdd_init(mx_fdd *f, unsigned installed, unsigned tracks);
void mx_fdd_set_writeback(mx_fdd *f, mx_fdd_writeback callback, void *ctx);

/* The geometry a raw image of this size has; 0 when it is not a standard size. */
int mx_fdd_geometry_for_size(size_t size, mx_fdd_geometry *geometry);
/* The BIOS parameter block of a boot sector against a geometry: MX_FDD_BPB_*. */
int mx_fdd_check_bpb(const uint8_t *image, size_t size, const mx_fdd_geometry *geometry);

/* Puts a disk in. 1 on success; 0 with the reason in `error` when the drive
 * has no room for it (an uninstalled drive, a size that is not a floppy's, a
 * 720 KiB image in a 40-track drive). On success `error` may carry a warning
 * (a BPB that disagrees with the size; mx_fdd.bpb tells). Replaces a disk
 * already inside. */
int mx_fdd_insert(mx_fdd *f, uint8_t *image, size_t size, unsigned flags, char *error, size_t capacity);
/* Takes the disk out; returns its image buffer (NULL when empty). */
uint8_t *mx_fdd_eject(mx_fdd *f);

/* Signals. */
static inline int mx_fdd_has_disk(const mx_fdd *f) { return f->installed && f->present; }
static inline int mx_fdd_door_open(const mx_fdd *f) { return !f->present; }
static inline int mx_fdd_wprot(const mx_fdd *f) { return f->installed && f->present && f->wprot; }
static inline int mx_fdd_track0(const mx_fdd *f) { return f->installed && f->cyl == 0; }
static inline int mx_fdd_changed(const mx_fdd *f) { return f->changed; }
static inline void mx_fdd_clear_changed(mx_fdd *f) { f->changed = 0; }
static inline int mx_fdd_dirty(const mx_fdd *f) { return f->dirty; }
static inline void mx_fdd_clear_dirty(mx_fdd *f) { f->dirty = 0; }

/* The spindle motor at time `now`: on and off, spin-up from `now`. */
void mx_fdd_set_motor(mx_fdd *f, int on, uint64_t now);
/* Motor at speed at `now`. */
int mx_fdd_at_speed(const mx_fdd *f, uint64_t now);
/* A disk turning at speed at `now`: the index and the data are there. */
int mx_fdd_rotating(const mx_fdd *f, uint64_t now);
/* The time a disk is turning at speed (already past when it is); MX_NEVER
 * without an installed drive, a disk or the motor on. */
uint64_t mx_fdd_rotating_at(const mx_fdd *f);

/* The index signal at `now`: high for the 2 ms the hole takes to pass, once a
 * revolution from the instant the spindle reaches speed; low when the drive
 * is not turning a disk. */
#define MX_FDD_INDEX_PULSE_US 2000u
int mx_fdd_index(const mx_fdd *f, uint64_t now);

/* One step pulse at time `now`: dir +1 towards the spindle, -1 away (towards TRK00).
 * Returns the cylinder. A step with a disk in clears the change latch. The time only stamps the
 * mechanical event the drive reports (hw.h: MX_DRIVE_STEP); the drive's state does not depend on it. */
unsigned mx_fdd_step(mx_fdd *f, int dir, uint64_t now);
/* A mechanical event of the drive (hw.h) at time `t`, to its hook; nothing for a drive that is not installed. */
static inline void mx_fdd_notify(const mx_fdd *f, unsigned event, uint64_t t, uint32_t a, uint32_t b) {
    if (f->hook && f->installed) f->hook(f->hook_ctx, f->id, event, t, a, b);
}

/* The next index pulse at or after `from` (the drive must be turning). */
uint64_t mx_fdd_next_index(const mx_fdd *f, uint64_t from);
/* The first ID field of `head` on the head's cylinder whose address mark has
 * not started before `from`. Returns its physical slot (0 .. sectors - 1), or
 * -1 when the track has no IDs for this head (no disk, not turning, past the
 * disk's last cylinder or side, unformatted). *slot_time is the time the
 * slot starts (the ID's sync); add mx_fdd_bytes(MX_FDD_ID_END),
 * mx_fdd_bytes(MX_FDD_DATA) and mx_fdd_bytes(MX_FDD_DATA_END) for the ends of
 * the ID, the start of the data and the end of the sector. */
int mx_fdd_find_id(const mx_fdd *f, unsigned head, uint64_t from, uint64_t *slot_time, mx_fdd_id *id);

/* The 512 data bytes of sector `r` of the track under `head`; *deleted
 * receives its deleted-mark flag (may be NULL). MX_FDD_OK, NO_DISK or NO_SECTOR. */
int mx_fdd_read_sector(const mx_fdd *f, unsigned head, unsigned r, uint8_t *out, int *deleted);
/* Writes sector `r` (with a deleted mark or not). MX_FDD_OK, NO_DISK,
 * NO_SECTOR or PROTECTED. */
int mx_fdd_write_sector(mx_fdd *f, unsigned head, unsigned r, const uint8_t *data, int deleted);
/* Formats the track under `head` with the IDs in physical order, every data
 * field filled with `fill`. MX_FDD_OK, NO_DISK, PROTECTED or BAD_FORMAT (see
 * the header comment). */
int mx_fdd_format_track(mx_fdd *f, unsigned head, const mx_fdd_id *ids, unsigned count, uint8_t fill);

#endif /* MX_FDD_H */
