/* MezinXT - the NEC uPD765A floppy disk controller, written from the data sheet. A chip as hw.h defines it: it talks
 * to the board only through the host callbacks (INT and DRQ out, TC and RESET
 * in) and reaches its drives, plain structs (fdd.h), through pointers the
 * diskette adapter (fdc_card.h) hands it.
 *
 * Bus cycles: I/O space, offset bit 0: 0 the main status register (read; a
 * write is ignored), 1 the data register. The IBM adapter and the Junior put
 * them at 3F4h and 3F5h. Main status register: bit 7 RQM, 6 DIO (1 =
 * controller to processor), 5 EXM (execution phase in non-DMA mode), 4 CB
 * (command in progress), 3-0 drive n is seeking. RQM comes up a few
 * microseconds (12 us) after each data register access in the command and
 * result phases, as the data sheet's service interval says.
 *
 * Pins (state 1 is the logical assertion). Inputs, ops->input:
 * MX_UPD765_PIN_TC the terminal count (the 8237's EOP; also delivered by the
 * ops->dack cycle that reaches it); MX_UPD765_PIN_RESET holds the chip in
 * reset while 1: every state is cleared, the main status register reads 0,
 * and on the falling edge (release) the chip raises its reset interrupt after
 * 100 us. Outputs, host->line: MX_UPD765_PIN_INT, MX_UPD765_PIN_DRQ.
 * ops->reset is a pulse on RESET: reset and release at once.
 *
 * DMA: with the Specify command's ND bit 0 (the default) every byte of the
 * execution phase is a DMA request: DRQ rises when a byte waits or is wanted
 * and falls on the ops->dack cycle (the 8237 calls it with the memory-write
 * direction, to_device 0, for a read command; memory-read, to_device 1, for a
 * write or a format). With ND 1 the data register is polled through the main
 * status register (RQM, EXM) with INT on every byte. The DMA channel number
 * is ignored.
 *
 * Commands (the low five bits of the first byte; bit 7 MT, 6 MFM, 5 SK where
 * the data sheet gives them): 02h Read Track, 03h Specify, 04h Sense Drive
 * Status, 05h Write Data, 06h Read Data, 07h Recalibrate, 08h Sense Interrupt
 * Status, 09h Write Deleted Data, 0Ah Read ID, 0Ch Read Deleted Data, 0Dh
 * Format Track, 0Fh Seek. The three Scan commands (11h, 19h, 1Dh) are not
 * implemented and answer as invalid commands, as does any other code: a
 * result phase of one byte, ST0 = 80h.
 *
 * Status registers: ST0 (IC, SE, EC, NR, HD, US), ST1 (EN end of cylinder,
 * OR overrun or underrun, ND no data, NW not writable, MA missing address
 * mark), ST2 (CM control mark, WC wrong cylinder), ST3 (FT, WP, RY, T0, TS, HD,
 * US). Not raised, because the media cannot show them: DE and DD (CRC
 * errors), BC, MD, FT (the drive never faults), SH and SN (no Scan).
 *
 * Execution. The controller reads and writes at the head's cylinder (the C
 * of a command is only compared with the disk's ID; there is no implied
 * seek) and finds sectors as the disk turns: it looks for an ID whose C, H,
 * R and N match, ID after ID, until the second index pulse after the start of
 * the search, then ends with ND (and MA when no ID field was seen at all; a
 * different C also sets WC). A data transfer takes 32 us a byte at the
 * pace of the rotation (fdd.h lays the track out); a byte the host has not
 * taken by the time the next arrives (or a byte not supplied in time on a
 * write) is an overrun, OR. After each sector: TC, or a skipped or control
 * mark sector with SK 0, ends the command normally; the sector number EOT
 * ends it with EN (abnormal termination) unless TC came, or, with MT on side
 * 0, continues with sector 1 of side 1; any other sector goes on to the
 * next. The result's C, H, R, N are those of the next sector as the data
 * sheet's table has it, except after an error, which leaves them at the
 * sector that failed. A transfer ended by TC in the middle of a sector lets
 * the rest of the sector pass (a write fills the rest of it with zeros).
 * Read Track starts at the index, reads up to EOT sectors in physical
 * order with no ID comparison. Read ID gives the next ID that passes. Format
 * Track takes four bytes (C, H, R, N) per sector from the host, one revolution
 * after the index it waits for; a drive that cannot hold the geometry (fdd.h)
 * answers ND, a write-protected one NW. Write commands on a write-protected
 * disk end at once with NW.
 *
 * Before a data command the head loads: the Specify HLT time passes unless the
 * head is still loaded from an earlier command (HUT after its end). SRT sets
 * the step rate: (16 - SRT) * 2 ms (the data sheet's 4 MHz clock, 250 kbit/s
 * MFM), HUT * 32 ms (0 is 512), HLT * 4 ms (0 is 512). Seek and Recalibrate
 * step the drive pulse by pulse (Recalibrate gives up with EC after 77
 * pulses) and signal seek end with an interrupt; the controller accepts
 * commands for other drives meanwhile. A drive that is not turning leaves
 * a data command waiting (no index, no ID) until it is, as on the IBM
 * adapter, whose READY input is wired high: the controller then sees every
 * drive as ready; mx_upd765_ready_wiring(f, 0) makes the READY input follow
 * the drive (a disk turning at speed), which gives NR and ready-change
 * interrupts.
 *
 * Interrupts. INT rises at the start of a data command's result phase (and
 * per byte in non-DMA mode), falls on the first result byte read; at the end
 * of a seek or recalibrate (falls on Sense Interrupt Status, which returns
 * ST0 and PCN; one more result is then reported for each further finished
 * drive); and after a reset: one interrupt, then four Sense Interrupt Status
 * results, ST0 = C0h, C1h, C2h, C3h (the ready lines of the four drives
 * changing, IC = 11), PCN 0, and 80h after; a program that reads only the
 * first (the Junior's BIOS does) finds the rest gone once any other
 * interrupt is raised (a seek end, a data command's result), as in the
 * MiSTer core's floppy. With the READY input following
 * the drive, only the drives that are ready report, and later changes of a
 * drive's ready state interrupt the same way.
 *
 * Time: OSC periods (hw.h); one deadline through host->schedule for the
 * whole chip.
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_UPD765_H
#define MX_UPD765_H

#include "hw.h"
#include "fdd.h"

/* Input pins (ops->input) and output pins (host->line). */
enum { MX_UPD765_PIN_TC, MX_UPD765_PIN_RESET };
enum { MX_UPD765_PIN_INT, MX_UPD765_PIN_DRQ };

/* Main status register bits. */
enum { MX_UPD765_RQM = 0x80, MX_UPD765_DIO = 0x40, MX_UPD765_EXM = 0x20, MX_UPD765_CB = 0x10 };

/* Command codes (the low five bits of the first byte). */
enum {
    MX_UPD765_READ_TRACK = 0x02, MX_UPD765_SPECIFY = 0x03, MX_UPD765_SENSE_DRIVE = 0x04,
    MX_UPD765_WRITE_DATA = 0x05, MX_UPD765_READ_DATA = 0x06, MX_UPD765_RECALIBRATE = 0x07,
    MX_UPD765_SENSE_INT = 0x08, MX_UPD765_WRITE_DELETED = 0x09, MX_UPD765_READ_ID = 0x0A,
    MX_UPD765_READ_DELETED = 0x0C, MX_UPD765_FORMAT = 0x0D, MX_UPD765_SEEK = 0x0F
};
/* Command byte flags. */
enum { MX_UPD765_MT = 0x80, MX_UPD765_MFM = 0x40, MX_UPD765_SK = 0x20 };

/* Status register bits. */
enum {
    MX_ST0_IC_ABNORMAL = 0x40, MX_ST0_IC_INVALID = 0x80, MX_ST0_IC_READY = 0xC0,
    MX_ST0_SE = 0x20, MX_ST0_EC = 0x10, MX_ST0_NR = 0x08, MX_ST0_HD = 0x04,
    MX_ST1_EN = 0x80, MX_ST1_OR = 0x10, MX_ST1_ND = 0x04, MX_ST1_NW = 0x02, MX_ST1_MA = 0x01,
    MX_ST2_CM = 0x40, MX_ST2_WC = 0x10,
    MX_ST3_FT = 0x80, MX_ST3_WP = 0x40, MX_ST3_RY = 0x20, MX_ST3_T0 = 0x10, MX_ST3_TS = 0x08, MX_ST3_HD = 0x04
};

/* The command being executed (read, write, format, read ID, read track). */
typedef struct mx_upd765_exec {
    uint8_t op, mt, mfm, sk;
    uint8_t unit, head;
    mx_fdd *fdd;
    uint8_t c, h, r, n, eot, gpl, dtl;      /* the ID registers and the parameters */
    uint8_t st0, st1, st2, fail_st1;
    uint8_t stage, tc, dir, pending, dreg, stop, skip, found, saw_id;
    unsigned bytepos;
    uint8_t buf[MX_FDD_SECTOR];
    uint64_t slot_t, data_t;
    uint8_t fmt_n, fmt_sc, fmt_gpl, fmt_d;  /* format track parameters */
    unsigned fmt_q;
    uint64_t fmt_t0;
    mx_fdd_id fmt_ids[256];
    unsigned rt_slot, rt_count;             /* read track: the physical slot, sectors read */
} mx_upd765_exec;

typedef struct mx_upd765 {
    mx_dev dev;
    mx_fdd *drive[4];           /* the drive each unit number reaches; NULL: none */
    uint8_t ready_tied;         /* READY wired high (default) */
    uint8_t in_reset;
    uint8_t phase;
    uint8_t cmd[9], cmd_len, cmd_got;
    uint8_t res[8], res_len, res_pos;
    uint64_t rqm_at;            /* RQM is up from this time in the command and result phases */
    uint8_t line_int, line_drq;
    uint8_t int_cmd, int_status;
    uint8_t srt, hut, hlt, nd;  /* Specify */
    uint8_t pcn[4];
    uint8_t seeking[4], seek_recal[4], seek_hd[4], seek_target[4], seek_rem[4], seek_pulses[4];
    int8_t seek_dir[4];
    mx_fdd *seek_drive[4];
    uint8_t se_pending[4], se_st0[4], rc_pending[4], ready_prev[4];
    uint8_t reset_list[4], reset_n, reset_pos;
    uint8_t head_loaded;
    uint64_t head_until;
    uint64_t t_cmd, t_rint, t_poll, t_seek[4], armed;
    mx_upd765_exec x;
} mx_upd765;

/* Fills the chip in its reset state; no host call is made. The board then
 * calls dev.ops->reset (or drives the RESET pin) to bring the lines in step. */
void mx_upd765_init(mx_upd765 *f, const mx_hw_host *host, const char *name);
/* The drive that unit `unit` (0-3) reaches. */
void mx_upd765_attach(mx_upd765 *f, unsigned unit, mx_fdd *drive);
/* 1 (default): the READY input is wired high, as on the IBM adapter. */
void mx_upd765_ready_wiring(mx_upd765 *f, int tied_high);
/* A drive's motor, disk or spindle state changed: wakes a command waiting
 * for the disk and reports ready-line changes. Safe to call at any time. */
void mx_upd765_drive_changed(mx_upd765 *f);

#endif /* MX_UPD765_H */
