/* MezinXT - Intel 8253 programmable interval timer.
 *
 * Three independent 16-bit counters, modes 0-5 (interrupt on terminal
 * count, programmable one-shot, rate generator, square wave, software and
 * hardware triggered strobe), binary or BCD, LSB / MSB / LSB-then-MSB
 * access, counter latch, a GATE input and an OUT output each. Written from
 * the 8253 data sheet; the 8254's read-back command is not part of the
 * 8253, a control word with SC = 11 does nothing.
 *
 * Event driven: nothing runs per clock. The chip knows the time of every
 * clock edge from the host's now() (an edge happens at the OSC time
 * ceil(k * num / den) for the clock's ratio), keeps each counter's state at
 * its last visit and, on every access, replays the clocks since, so a count
 * read is exact at any `now`. Its one host deadline is the next OUT
 * transition (or load) of the counters that are watched.
 *
 * The clock is a ratio of OSC periods per clock edge, given at init: the
 * PC's timer clock is OSC/12 (MX_I8253_PIT: 1.193182 MHz), the SIO adapter's
 * baud timer takes another ratio. Clock edges are on the absolute time grid
 * (edge k of the ratio at ceil(k * num / den)); an edge that falls at
 * exactly `now` has already happened when an access at `now` runs.
 *
 * Pins (hw.h): the board drives GATE0-GATE2 through ops->input, pin
 * numbers 0-2, state 1 = high (enabled); they start high, as GATE0 and
 * GATE1 are tied high on the PC (GATE2 follows port B bit 0). The chip
 * drives OUT0-OUT2 through host->line, pin numbers 0-2, state 1 = high
 * (OUT0 is IRQ0, OUT1 the refresh request, OUT2 feeds the speaker).
 *
 * Bus cycles: I/O space, offset bits 1-0: 0-2 the counters, 3 the control
 * word (write only: a read returns 0xFF). The Junior decodes 40h-43h.
 *
 * Details settled where the data sheet leaves room: a count written to a
 * counter is loaded on the first clock edge after the write (a read in
 * between still returns the old count); in mode 3 the count falls by two
 * per clock (the data sheet's odd-count rule included) so reads show it; a
 * count of 1 in modes 2 and 3 is illegal and leaves OUT high; invalid BCD
 * digits in a loaded count are taken as their binary value, modulo 10000.
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_I8253_H
#define MX_I8253_H

#include "hw.h"

/* Input pins (ops->input) and output pins (host->line). */
enum { MX_I8253_GATE0, MX_I8253_GATE1, MX_I8253_GATE2 };
enum { MX_I8253_OUT0, MX_I8253_OUT1, MX_I8253_OUT2 };

/* The PC timer's clock: one edge per 12 OSC periods. */
#define MX_I8253_PIT_NUM 12u
#define MX_I8253_PIT_DEN 1u

typedef struct mx_i8253_counter {
    uint8_t mode, bcd, rw;      /* rw: 1 LSB, 2 MSB, 3 LSB then MSB */
    uint8_t gate, out, out_reported;
    uint8_t loaded;             /* the counting element holds a loaded count */
    uint8_t pending;            /* the count register loads on the next clock edge */
    uint8_t armed;              /* modes 0, 1, 4, 5: the terminal count has not come yet */
    uint8_t strobe;             /* modes 4, 5: OUT is low for the strobe's clock */
    uint8_t phase;              /* mode 3: 0 the high half, 1 the low half */
    uint8_t write_hi, read_hi;  /* the byte flip-flops */
    uint8_t latched;            /* a latched count is waiting to be read */
    uint8_t wbyte;              /* the LSB waiting for its MSB */
    uint16_t cr, latch;         /* count register, latched count (as written / read: BCD digits in BCD) */
    uint32_t nload;             /* modes 2, 3: the count of the cycle running, 1..modulus */
    uint32_t ce;                /* the counting element, 0..modulus-1 (0 stands for the modulus) */
    uint32_t k;                 /* mode 3: clock edges into the current half */
    uint64_t tick;              /* the clock edge count the state is valid at */
} mx_i8253_counter;

typedef struct mx_i8253 {
    mx_dev dev;
    uint32_t clk_num, clk_den;  /* OSC periods per clock edge = num / den */
    uint8_t watch;              /* counters whose OUT changes are scheduled and reported (bit n) */
    mx_i8253_counter c[3];
} mx_i8253;

/* Sets the chip up on `host` with a clock of `clk_num / clk_den` OSC periods
 * per edge (MX_I8253_PIT_NUM, MX_I8253_PIT_DEN for the PC) and resets it. */
void mx_i8253_init(mx_i8253 *t, const mx_hw_host *host, uint32_t clk_num, uint32_t clk_den);
/* Which counters' OUT changes the chip schedules and reports through
 * host->line (default 7: all three). An unwatched counter is only replayed
 * when it is accessed, which spares the host an event per OUT edge on a
 * line nobody listens to (OUT1, the refresh request); the board reads its
 * level with mx_i8253_out. A counter added to the mask has its OUT level
 * reported at once if it differs from the last report. */
void mx_i8253_watch(mx_i8253 *t, unsigned mask);
/* OUT of a counter (0-2) at the host's now(). */
int mx_i8253_out(mx_i8253 *t, unsigned counter);

#endif /* MX_I8253_H */
