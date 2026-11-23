/* MezinXT - the Winchester coupler: FEPER's
 * catalogue lists a Winchester coupler (703.300.006) and drives of 20 to 70 MB
 * (703.805), but the manual does not document their interface and none of the
 * four Junior BIOSes holds fixed-disk code. This is a
 * reconstruction, register-compatible with the IBM Fixed Disk Adapter of the
 * PC/XT (a Xebec 1210-class controller) as IBM's Technical Reference
 * documents it; its BIOS is MezinXT's own option ROM (guest/hdrom.asm).
 *
 * Board interface: one I/O window of 4 bytes, 320h-323h (MX_HDC_IO_BASE),
 * offsets inside it:
 *   0  data register: command bytes and data to the controller (write), data
 *      and the completion status byte from it (read)
 *   1  read: the status register (MX_HDC_ST_*); write: controller reset
 *   2  read: the drive-type switches (00h: MezinXT's ROM asks the controller
 *      for the geometry with command C0h instead); write: select (the
 *      controller starts taking a command)
 *   3  write: the DMA and interrupt mask register (MX_HDC_MASK_*); read: open
 *      bus (FFh)
 *
 * The protocol. A write to the select port makes the controller busy and
 * ready for a six-byte command block (DCB) on the data port, one byte for
 * each request (status BSY, C/D and REQ):
 *   byte 0  command (bits 7-5 class, 4-0 opcode)
 *   byte 1  bit 5 drive (0 or 1), bits 4-0 head
 *   byte 2  bits 7-6 cylinder bits 9-8, bits 5-0 sector (0-based)
 *   byte 3  cylinder bits 7-0
 *   byte 4  block count (0 is 256), the interleave for the format commands
 *   byte 5  control: bit 7 no retries, bit 6 no ECC retry, bits 2-0 step rate
 *           (kept, no effect)
 * A command with a data phase then asks for or offers its bytes on the data
 * port (status BSY and REQ with C/D clear, I/O set from the controller);
 * sector data (512 bytes a sector) go through DMA channel 3 when the mask
 * register's DMA bit is set (the DRQ line, status bit DRQ), else byte by byte
 * through the data port; the parameter and sense bytes always go through the
 * data port. At the end the controller offers one completion byte (status
 * BSY, C/D, I/O and REQ; status bit IRQ set, and the IRQ line when the mask's
 * interrupt bit is set): bit 1 an error, bit 5 the drive. Reading it frees
 * the controller (status 0) and clears the interrupt.
 *
 * Commands (the IBM adapter's set; MX_HDC_CMD_*): 00h test drive ready, 01h
 * recalibrate, 03h request sense (4 bytes: error code with bit 7 for a valid
 * address, drive and head, cylinder high bits and sector, cylinder low), 04h
 * format drive (from the address to the end), 05h verify, 06h format track,
 * 07h format bad track (as 06h: no defect list is kept), 08h read, 0Ah write,
 * 0Bh seek, 0Ch initialize drive characteristics (8 bytes: cylinders high
 * and low, heads, reduced write current cylinder (2), write precompensation
 * cylinder (2), ECC burst length), 0Dh read ECC burst error length (1 byte),
 * 0Eh read sector buffer, 0Fh write sector buffer, E0h RAM diagnostic, E3h
 * drive diagnostic, E4h controller internal diagnostic, E5h read long and E6h
 * write long (512 data bytes and 4 check bytes a sector; the check bytes are
 * MezinXT's own sum, not the Xebec's ECC polynomial: a write long's are
 * ignored). MezinXT's own: C0h read drive geometry (8 bytes: cylinders high
 * and low, heads, sectors a track, then 1 and three zeros; a missing drive
 * answers not ready). Other opcodes end with error 20h (invalid command).
 * Sense codes: MX_HDC_E_*.
 *
 * Addresses. The drive's cylinders and heads are those of its image
 * (mx_hdc_media); command 0Ch narrows the cylinders and heads the controller
 * accepts (an address beyond them is error 21h), and an address beyond the
 * drive itself is record not found (14h). Multi-sector commands run on
 * through the following sectors, heads and cylinders.
 *
 * Time, from the guest's clock (hw.h): a seek takes 1.5 ms plus 30 us a
 * cylinder (a drive faster than the period's 65-85 ms average, so the disk is
 * never a wait), the platter turns at 3600 rpm with 17 sectors a track, the
 * first sector of a command waits for its place under the head and the next
 * ones follow one sector time apart (as a well-interleaved disk reads), a
 * format takes one revolution a track. The sector buffer is one sector: data
 * the host is slow to take or give hold the controller.
 *
 * Pins. Outputs, host->line: MX_HDC_IRQ and MX_HDC_DRQ, state 1 asserted,
 * each gated by its mask bit. The board routes them to IRQ 5 and DMA channel
 * 3; the card is then the device on DACK 3 (ops->dack). The 8237's terminal
 * count is not used (the block count ends a transfer).
 *
 * Media: the drives' sectors are the board's (mx_hdc_media): the controller
 * reads and writes one sector at a time through its callbacks, by LBA. A drive
 * without media is not present (not ready).
 *
 * Portable C11. MezinXT's own code, from IBM's documentation of the adapter's
 * registers and command set. */
#ifndef MX_HDC_H
#define MX_HDC_H

#include "hw.h"

#define MX_HDC_IO_BASE   0x320u
#define MX_HDC_IO_SIZE   4u
#define MX_HDC_ROM_BASE  0xC8000u
#define MX_HDC_ROM_SIZE  0x2000u        /* the option ROM: 8 KiB */
#define MX_HDC_DRIVES    2u
#define MX_HDC_SECTOR    512u
#define MX_HDC_SPT       17u            /* sectors a track (MFM) */
#define MX_HDC_IRQ_LINE  5
#define MX_HDC_DMA_CHANNEL 3

/* Output pins (host->line). */
enum { MX_HDC_IRQ, MX_HDC_DRQ };

/* Status register bits (port 321h). */
enum {
    MX_HDC_ST_REQ = 0x01,       /* a byte may move on the data port */
    MX_HDC_ST_IO = 0x02,        /* 1: from the controller to the host */
    MX_HDC_ST_CD = 0x04,        /* 1: command or status byte; 0: data */
    MX_HDC_ST_BSY = 0x08,       /* the controller is selected */
    MX_HDC_ST_DRQ = 0x10,       /* a DMA request is pending */
    MX_HDC_ST_IRQ = 0x20        /* the command has ended (interrupt pending) */
};
/* DMA and interrupt mask register (port 323h). */
enum { MX_HDC_MASK_DMA = 1, MX_HDC_MASK_IRQ = 2 };

/* Commands. */
enum {
    MX_HDC_CMD_TEST_READY = 0x00, MX_HDC_CMD_RECALIBRATE = 0x01, MX_HDC_CMD_SENSE = 0x03, MX_HDC_CMD_FORMAT_DRIVE = 0x04,
    MX_HDC_CMD_VERIFY = 0x05, MX_HDC_CMD_FORMAT_TRACK = 0x06, MX_HDC_CMD_FORMAT_BAD = 0x07, MX_HDC_CMD_READ = 0x08,
    MX_HDC_CMD_WRITE = 0x0A, MX_HDC_CMD_SEEK = 0x0B, MX_HDC_CMD_INIT = 0x0C, MX_HDC_CMD_ECC_LENGTH = 0x0D,
    MX_HDC_CMD_READ_BUFFER = 0x0E, MX_HDC_CMD_WRITE_BUFFER = 0x0F, MX_HDC_CMD_RAM_DIAG = 0xE0, MX_HDC_CMD_DRIVE_DIAG = 0xE3,
    MX_HDC_CMD_CTRL_DIAG = 0xE4, MX_HDC_CMD_READ_LONG = 0xE5, MX_HDC_CMD_WRITE_LONG = 0xE6,
    MX_HDC_CMD_GEOMETRY = 0xC0          /* MezinXT's own */
};
/* Sense error codes (sense byte 0, bits 5-0). */
enum {
    MX_HDC_E_NONE = 0x00, MX_HDC_E_NO_INDEX = 0x01, MX_HDC_E_NO_SEEK = 0x02, MX_HDC_E_WRITE_FAULT = 0x03,
    MX_HDC_E_NOT_READY = 0x04, MX_HDC_E_NO_TRACK0 = 0x06, MX_HDC_E_ID_ECC = 0x10, MX_HDC_E_DATA = 0x11,
    MX_HDC_E_NO_AM = 0x12, MX_HDC_E_NOT_FOUND = 0x14, MX_HDC_E_SEEK = 0x15, MX_HDC_E_CORRECTED = 0x18,
    MX_HDC_E_BAD_TRACK = 0x19, MX_HDC_E_INVALID = 0x20, MX_HDC_E_ADDRESS = 0x21, MX_HDC_E_RAM = 0x30,
    MX_HDC_E_CHECKSUM = 0x31, MX_HDC_E_ECC_POLY = 0x32
};

/* A drive's medium: its geometry and its sectors, one at a time by LBA
 * ((cylinder x heads + head) x sectors + sector). The callbacks return 0 when
 * the host cannot read or write (a read error, a write fault). */
typedef struct mx_hdc_media {
    void *ctx;
    uint32_t cylinders, heads, sectors;
    int (*read)(void *ctx, uint32_t lba, uint8_t *data);
    int (*write)(void *ctx, uint32_t lba, const uint8_t *data);
} mx_hdc_media;

typedef struct mx_hdc_drive {
    const mx_hdc_media *media;          /* NULL: no drive */
    uint32_t cylinder;                  /* where the heads are */
    uint32_t max_cylinders, max_heads;  /* what command 0Ch set (0: the drive's own) */
    uint8_t sense[4];                   /* the last command's error */
    uint64_t last;                      /* the end of its last command (activity) */
    uint8_t busy;                       /* a command runs on it */
} mx_hdc_drive;

typedef struct mx_hdc {
    mx_dev dev;
    mx_hdc_drive drive[MX_HDC_DRIVES];
    uint8_t mask;                       /* port 323h */
    uint8_t phase;                      /* internal: idle, command, busy, data in, data out, status */
    uint8_t dcb[6];
    uint8_t dcb_count;
    uint8_t buffer[MX_HDC_SECTOR + 4];  /* the sector buffer (+4 check bytes of a long transfer) */
    uint8_t params[8];
    uint8_t *data;                      /* the bytes of the data phase */
    uint32_t data_length, data_pos;
    uint8_t data_dma;                   /* the data phase may use DMA */
    uint8_t status_byte;
    mx_drive_hook hook;                 /* the mechanical events (hw.h), for the drive sound; NULL: none */
    void *hook_ctx;
    uint8_t interrupt;                  /* the completion is pending (status bit IRQ) */
    uint8_t irq_out, drq_out;
    /* the command in progress */
    uint8_t command, unit, step;
    uint32_t cylinder, head, sector, count;
    uint32_t done;                      /* sectors moved */
    uint64_t next_sector;               /* when the next sector of a run is under the head */
} mx_hdc;

/* Fills in the controller with no drives; no host call is made. The board
 * then calls dev.ops->reset. */
void mx_hdc_init(mx_hdc *c, const mx_hw_host *host, const char *name);
/* A drive's medium in or out (NULL); the media struct stays the caller's. 0
 * for a unit out of range or a geometry the controller cannot address (1 to
 * 1024 cylinders, 1 to 16 heads, 17 sectors). */
int mx_hdc_attach(mx_hdc *c, unsigned unit, const mx_hdc_media *media);
/* The drives' mechanical events go to `hook` (hw.h): the spindle of a drive with a medium (running from the moment it is
 * attached, reported again here for those already attached) and each seek, by the cylinders it crosses and the time it
 * takes, as it starts. NULL stops them. The drives are numbered MX_DRIVE_HARD0 + n. */
void mx_hdc_set_drive_hook(mx_hdc *c, mx_drive_hook hook, void *ctx);
/* Bit n set while drive n works or has worked within `hold` OSC periods
 * before `now`, for the status line and the fast-disk mode. */
unsigned mx_hdc_activity(const mx_hdc *c, uint64_t now, uint64_t hold);

/* Geometry of an image of `size` bytes (cylinders, heads, 17 sectors): the
 * period drives MezinXT offers (mx_hdc_offered), then the heads an MBR's
 * partition table names (`mbr`, 512 bytes or NULL), then the first fit of
 * 4 to 16 heads and at most 1024 cylinders. 0 when the size is not a whole
 * number of 17-sector tracks that fits. */
int mx_hdc_geometry(uint64_t size, const uint8_t *mbr, uint32_t *cylinders, uint32_t *heads);
/* The new blank disks offered (the setup screen's sizes): index 0.. of
 * MX_HDC_OFFERED, with their nominal megabytes and geometry. */
#define MX_HDC_OFFERED 5u
typedef struct mx_hdc_type {
    unsigned megabytes;
    uint32_t cylinders, heads;
    const char *capacity;               /* the formatted capacity, "21.4 MB" */
} mx_hdc_type;
const mx_hdc_type *mx_hdc_offered(unsigned index);
/* Bytes of an image of the geometry (17 sectors a track). */
uint64_t mx_hdc_image_size(uint32_t cylinders, uint32_t heads);

/* MezinXT's option ROM for the coupler (guest/hdrom.c, built from
 * guest/hdrom.asm by tools/hdrom.py): MX_HDC_ROM_SIZE bytes. */
extern const uint8_t mx_hdc_rom[MX_HDC_ROM_SIZE];

#endif /* MX_HDC_H */
