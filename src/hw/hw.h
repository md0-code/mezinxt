/* MezinXT - the chip interface.
 *
 * Every chip is a plain struct that starts with an mx_dev and has an
 * mx_dev_ops table. A chip never calls the bus, the scheduler or another
 * chip: it talks to the board only through the mx_hw_host callbacks it is
 * given at init, so each chip is unit-tested against a fake host.
 *
 * Time is a count of periods of the board's 14.31818 MHz crystal (the
 * oscillator, OSC): the CPU clock is OSC/3, the 8253's clock OSC/12, the
 * CGA dot clock OSC/1. A chip's deadlines and the host's now() are in these
 * periods.
 *
 * Address spaces: the 8086 has a memory space (20 bits) and an I/O space
 * (16 bits). The board maps a chip into either or both (the CGA has
 * registers at 3D0h-3DFh and its store at B8000h); read and write receive
 * the space and the offset inside the chip's window. Cycles are byte or
 * word wide: on the Junior's 8-bit slots the board splits words into bytes
 * before a chip sees them, so a slot chip only ever gets size 1.
 *
 * Signals: a chip's output pins reach the board through host->line (IRQ,
 * DRQ, OUT, a port's bits...); the board drives a chip's input pins
 * through ops->input (IR inputs, GATE, port inputs, DACK...). Pin numbers
 * are the chip's own, listed in its header. DMA: the 8237 moves bytes
 * between memory (host->dma) and the device whose DACK it asserts
 * (that device's ops->dack).
 *
 * Portable C11 only: no compiler extensions, no OS or
 * SDL header. MezinXT's own code. */
#ifndef MX_HW_H
#define MX_HW_H

#include <stddef.h>
#include <stdint.h>

/* The crystal: 157.5/11 MHz = 14.318181... MHz, kept exactly as a ratio. */
#define MX_OSC_NUM 157500000u
#define MX_OSC_DEN 11u
#define MX_OSC_HZ  14318182u            /* rounded, for messages and rough sums */
#define MX_NEVER   UINT64_MAX

/* The address spaces of a cycle. */
enum { MX_SPACE_MEM = 0, MX_SPACE_IO = 1 };

typedef struct mx_dev_ops {
    /* A bus cycle on the chip: `space` MX_SPACE_*, `offset` inside the
     * chip's window, `size` 1 or 2 bytes (2 only for chips on the 8086's
     * word path, little-endian). A read returns the value in the low bits;
     * a pin the chip does not drive reads as 1 (open bus is 0xFF). */
    uint32_t (*read)(void *dev, unsigned space, uint32_t offset, unsigned size);
    void     (*write)(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value);
    /* The deadline the chip set with schedule() has come (`now` is it). */
    void     (*event)(void *dev, uint64_t now);
    /* The RESET pin: the chip's power-on / reset state. */
    void     (*reset)(void *dev);
    /* Optional (NULL: none). An input pin changed: state 1 asserted (the
     * chip's header says which level that is on the wire). */
    void     (*input)(void *dev, int pin, int state);
    /* Optional (NULL: no wait states). Extra OSC periods the cycle that is
     * about to run on the chip is held (READY low); `now` is the time its
     * command goes active (the start of T2). The board adds them to the
     * cycle's own length, or, for a window marked MX_BUS_IOCHRDY (bus.h),
     * takes them as the time from `now` at which the chip's release of
     * I/O CH RDY reaches the board's READY synchroniser. */
    uint32_t (*wait)(void *dev, unsigned space, uint32_t offset, int write, uint64_t now);
    /* Optional (NULL: the chip takes no part in DMA). A DMA cycle with the
     * chip's DACK asserted on DMA channel `channel`: with `to_device` the
     * byte `value` read from memory goes to the chip (a memory-read
     * transfer); otherwise the chip supplies the byte, which is returned.
     * `tc` is the 8237's terminal count on this cycle. */
    uint8_t  (*dack)(void *dev, int channel, int to_device, uint8_t value, int tc);
} mx_dev_ops;

/* What the board provides to a chip. */
typedef struct mx_hw_host {
    void *ctx;
    /* Current guest time in OSC periods. */
    uint64_t (*now)(void *ctx);
    /* Arms the chip's single deadline (absolute OSC time); MX_NEVER cancels.
     * Re-arming replaces the previous deadline. */
    void (*schedule)(void *ctx, void *dev, uint64_t when);
    /* An output pin of the chip changed; state 1 asserted. */
    void (*line)(void *ctx, void *dev, int pin, int state);
    /* Optional (NULL when the board has no bus master): the chip as bus
     * master reads (write 0) or writes (write 1) `length` bytes of the
     * memory space at `address`, in address order, byte-wide (DMA on the
     * Junior is always byte-wide). Returns 0 when nothing answers. */
    int (*dma)(void *ctx, uint32_t address, void *data, uint32_t length, int write);
} mx_hw_host;

/* The common header, first in every chip struct. */
typedef struct mx_dev {
    const mx_dev_ops *ops;
    const mx_hw_host *host;
    const char *name;
} mx_dev;

static inline uint64_t mx_dev_now(const mx_dev *d) { return d->host->now(d->host->ctx); }
static inline void mx_dev_schedule(mx_dev *d, uint64_t when) { d->host->schedule(d->host->ctx, d, when); }
static inline void mx_dev_line(mx_dev *d, int pin, int state) { d->host->line(d->host->ctx, d, pin, state); }
static inline int mx_dev_dma(mx_dev *d, uint32_t address, void *data, uint32_t length, int write) {
    return d->host->dma && d->host->dma(d->host->ctx, address, data, length, write);
}

/* The mechanical events of a disk drive, for whatever listens to them (the board hands the drive sound
 * source, drivesnd.h, to the drives and the Winchester coupler). A chip makes one call of its hook for each
 * event, stamped with the guest time it happens at (OSC periods; it may be a little earlier than the host's
 * now() when the chip is catching up); a chip whose hook is NULL (the default) does nothing at all. The drive is numbered as the board does:
 * MX_DRIVE_FLOPPY0 + n for diskette drive n, MX_DRIVE_HARD0 + n for fixed disk n. `a` and `b` by event:
 *   MX_DRIVE_MOTOR      a 1 the spindle motor is switched on, 0 off
 *   MX_DRIVE_STEP       a 1 one step towards the spindle, 0 towards track 0; b 1 when the carriage was
 *                       against its end stop and did not move
 *   MX_DRIVE_HEAD_LOAD  the head is loaded against the disk
 *   MX_DRIVE_HEAD_HOLD  a command that used the head ended at `t`; the controller unloads the head a OSC periods
 *                       later unless it is used again
 *   MX_DRIVE_SPINDLE    a 1 the spindle starts (to full speed), 0 it stops
 *   MX_DRIVE_SEEK       the heads move from here: a cylinders crossed (with MX_DRIVE_SEEK_HOME set in it when they come to
 *                       rest at cylinder 0: a recalibrate), b the seek time in microseconds */
enum { MX_DRIVE_MOTOR, MX_DRIVE_STEP, MX_DRIVE_HEAD_LOAD, MX_DRIVE_HEAD_HOLD, MX_DRIVE_SPINDLE, MX_DRIVE_SEEK };
#define MX_DRIVE_SEEK_HOME 0x80000000u
enum { MX_DRIVE_FLOPPY0 = 0, MX_DRIVE_HARD0 = 4, MX_DRIVE_COUNT = 6 };
typedef void (*mx_drive_hook)(void *ctx, unsigned drive, unsigned event, uint64_t t, uint32_t a, uint32_t b);

/* Little-endian byte access to guest memory images (no unaligned casts). */
static inline uint16_t mx_get16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static inline void mx_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

#endif /* MX_HW_H */
