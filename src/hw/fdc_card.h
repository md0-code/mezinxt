/* MezinXT - the diskette adapter: the IBM
 * diskette drive adapter as the Junior's FDA repeats it (the
 * original BIOS drives it through the uPD765 and the 8237 only). One mx_dev
 * (hw.h) that owns the controller (upd765.h) and up to four drives (fdd.h)
 * as plain structs and talks to the board only through its host callbacks.
 *
 * Board interface: one I/O window of 8 bytes, 3F0h-3F7h (MX_FDC_IO_BASE,
 * MX_FDC_IO_SIZE), offsets inside it:
 *   2  digital output register, write only (a read is open bus, 0xFF)
 *        bits 1-0 drive select, bit 2 /FDC RESET (0 holds the controller in
 *        reset), bit 3 DMA and interrupt enable, bits 7-4 motor enable of
 *        drives 0-3
 *   4  uPD765 main status register, read
 *   5  uPD765 data register, read and write
 * The other offsets are unused: reads give 0xFF, writes are ignored (the IBM
 * adapter has no 3F7h; the AT's disk-change bit is not there).
 *
 * Pins. Outputs, host->line: MX_FDC_CARD_IRQ and MX_FDC_CARD_DRQ, state 1
 * asserted: the controller's INT and DRQ gated by the DOR's enable bit (with
 * it clear both stay low whatever the controller does). The board routes
 * them to IRQ 6 and DMA channel 2 (mx_bus_route; the route makes the card the
 * device on the channel's DACK, whose cycles reach ops->dack). Input,
 * ops->input: MX_FDC_CARD_TC the terminal count, the 8237's EOP (ops->dack
 * carries the same TC on the cycle that reaches it).
 *
 * The drive the controller works on is the one the DOR's select bits name, as
 * on the IBM adapter (the controller's own unit select outputs are not
 * wired): all four unit numbers of the controller reach that drive, while its
 * per-unit state (present cylinder numbers, the unit in ST0) follows the
 * command's unit. A drive responds to its motor bit independently of the
 * select bits; a motor that is switched on takes the drive's spin-up time
 * (fdd.h, 400 ms) to reach speed, and until it does a data command waits for
 * the disk. The controller's READY input is wired high as on the IBM
 * adapter; mx_fdc_card_ready_wired(card, 0) gives the other kind of wiring: READY is
 * the drive's real ready state (a disk turning at speed), which gives NR
 * and ready-change interrupts, and the controller's unit number picks the
 * drive (unit n is drive n), the DOR's select bits being left to the motors.
 *
 * Reset: the DOR is 0, the controller is held in reset, the motors are off
 * and the lines low; the disks stay in their drives. A drive that is not
 * installed (mx_fdc_card_init `drives`) has no motor, no track 0 and no disk:
 * a data command for it never ends, a recalibrate ends with an equipment
 * check, as with an empty socket.
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_FDC_CARD_H
#define MX_FDC_CARD_H

#include "hw.h"
#include "fdd.h"
#include "upd765.h"

#define MX_FDC_IO_BASE 0x3F0u
#define MX_FDC_IO_SIZE 8u

/* Output pins (host->line) and the input pin (ops->input). */
enum { MX_FDC_CARD_IRQ, MX_FDC_CARD_DRQ };
enum { MX_FDC_CARD_TC };

/* DOR bits. */
enum { MX_FDC_DOR_SELECT = 3, MX_FDC_DOR_NRESET = 4, MX_FDC_DOR_ENABLE = 8, MX_FDC_DOR_MOTOR0 = 0x10 };

typedef struct mx_fdc_card {
    mx_dev dev;
    mx_upd765 fdc;
    mx_hw_host fdc_host;        /* the host the controller sees */
    mx_fdd drive[4];
    unsigned drives;            /* installed drives, 1-4 */
    uint8_t dor;
    uint8_t int_in, drq_in;     /* the controller's INT and DRQ */
    uint8_t irq_out, drq_out;   /* what the card reports to the board */
    uint8_t ready_wired;        /* 1 (default): READY tied high */
} mx_fdc_card;

/* Fills in the adapter with `drives` (1-4) 40-track drives, empty; no host
 * call is made. The board then calls dev.ops->reset. */
void mx_fdc_card_init(mx_fdc_card *c, const mx_hw_host *host, const char *name, unsigned drives);
/* Drive n (0-3); NULL when n is out of range. */
mx_fdd *mx_fdc_card_drive(mx_fdc_card *c, unsigned n);
/* Makes drive n a 40- or 80-track drive; 0 when a disk is in it. */
int mx_fdc_card_set_tracks(mx_fdc_card *c, unsigned n, unsigned tracks);
/* Disk in and out (fdd.h: the image is the caller's buffer; 1 on success,
 * 0 with the reason in `error`). Both tell the controller. */
int mx_fdc_card_insert(mx_fdc_card *c, unsigned n, uint8_t *image, size_t size, unsigned flags, char *error, size_t capacity);
uint8_t *mx_fdc_card_eject(mx_fdc_card *c, unsigned n);
/* The mechanical events of all the drives (hw.h: motor, steps, head load) go to `hook`; NULL stops them. The
 * drives are numbered MX_DRIVE_FLOPPY0 + n. */
void mx_fdc_card_set_drive_hook(mx_fdc_card *c, mx_drive_hook hook, void *ctx);
/* Bit n set while drive n's motor runs, for the status line. */
unsigned mx_fdc_card_activity(const mx_fdc_card *c);
/* 1: READY tied high (default); 0: it follows the drive. */
void mx_fdc_card_ready_wired(mx_fdc_card *c, int wired);

#endif /* MX_FDC_CARD_H */
