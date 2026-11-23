/* MezinXT - the Junior's parallel printer port (the printer adapter of
 * 703.910.050, schematic sheet 19; manual 3.4, pages 60-62): a Centronics
 * output with the registers of the IBM printer adapter. Three ports at 378h
 * (alternative strap 278h): MX_LPT_IO_BASE, MX_LPT_IO_SIZE, offsets in the
 * window, decoded by a 74LS155 on A1-A0 and the cycle:
 *   0 write  data: the 74LS374 latch drives the eight data pins (cleared by
 *            nothing: it keeps its value through a reset)
 *   0 read   data: the pins (the latch; no printer drives them)
 *   1 read   status: bit 7 BUSY (inverted: 1 is not busy), bit 6 /ACK (1
 *            idle, 0 during the pulse), bit 5 PAPER END (1 out of paper),
 *            bit 4 SELECT (1 on line), bit 3 /ERROR (1 no error); bits 2-0
 *            are not driven and read 1 (0xDF is the usual idle value of a
 *            ready printer)
 *   2 write  control: a 74LS174 latch, cleared by RESET. Bit 0 STROBE and
 *            bit 1 AUTOFEED and bit 3 SELECT IN go through 7406 inverters
 *            to the open-collector pins, so a 1 pulls the pin low (the
 *            active level); bit 2 /INIT drives its pin directly (a 0 holds
 *            the printer in reset: the BIOS writes 0Ch to let it go); bit 4
 *            is the interrupt enable, bits 7-5 do nothing
 *   2 read   control: bits 3-0 are the pins read back through their
 *            inverters (the written bits), bit 4 the interrupt enable, bits
 *            7-5 read 1
 *   1, 3 write and 3 read: not decoded (a read gives 0xFF)
 * The adapter takes no wait state (only the serial side holds IOCHRDY).
 *
 * Interrupt: with control bit 4 set, a 74LS125 drives IRQ 7 with the inverse
 * of the /ACK pin: the line is high while the printer's ACK pulse is low, so
 * each 1-to-0 transition of ACK is one rising edge on IRQ 7 (the manual: an
 * interrupt on each transition from 1 to 0 of ACK). Output pin
 * MX_LPT_IRQ, host->line, state 1 asserted; the board routes it to IRQ 7.
 *
 * The printer. The chip carries a simple Centronics printer: the byte on the
 * data pins is latched on the leading edge of STROBE (control bit 0 going
 * from 0 to 1) when the printer is on line (select), has no error, is not
 * held in reset (/INIT high, control bit 2) and is not busy; the byte goes
 * to the sink, BUSY goes high at once, and after the busy time BUSY falls
 * and /ACK is pulsed low for the ack time (BUSY and ACK change together, so
 * the interrupt finds the printer ready; a strobe during the ACK pulse is
 * accepted). A strobe that finds the printer busy or off line is lost, a strobe that is held low gives one byte, and
 * /INIT going low aborts a handshake in progress. An off-line printer or
 * one with an error shows BUSY. The default times are 10 microseconds busy
 * and a 5 microsecond ACK pulse; mx_lpt_set_timing changes them (OSC
 * periods; MX_LPT_US converts).
 *
 * Input pins, ops->input, state 1 meaning what the name says (the wire's
 * level is the opposite for ERROR): MX_LPT_IN_SELECT (printer on line,
 * default 1), MX_LPT_IN_PAPER_END (out of paper, default 0), MX_LPT_IN_ERROR
 * (printer error, default 0).
 *
 * Reset (RESET pin): the control latch clears (STROBE and AUTOFEED and
 * SELECT IN high, /INIT low, interrupts disabled), a handshake is dropped.
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_LPT_H
#define MX_LPT_H

#include "hw.h"

#define MX_LPT_IO_BASE 0x378u
#define MX_LPT_IO_SIZE 4u

/* Output pin (host->line) and input pins (ops->input). */
enum { MX_LPT_IRQ };
enum { MX_LPT_IN_SELECT, MX_LPT_IN_PAPER_END, MX_LPT_IN_ERROR };

/* Control register bits. */
enum { MX_LPT_CTL_STROBE = 1, MX_LPT_CTL_AUTOFEED = 2, MX_LPT_CTL_INIT = 4, MX_LPT_CTL_SELIN = 8, MX_LPT_CTL_IRQEN = 16 };
/* Status register bits. */
enum { MX_LPT_ST_ERROR = 0x08, MX_LPT_ST_SELECT = 0x10, MX_LPT_ST_PAPER = 0x20, MX_LPT_ST_ACK = 0x40, MX_LPT_ST_BUSY = 0x80 };

/* OSC periods in `n` microseconds. */
#define MX_LPT_US(n) (((uint64_t)(n) * 157500u) / 11000u)

enum { MX_LPT_IDLE, MX_LPT_BUSY, MX_LPT_ACK };

typedef struct mx_lpt {
    mx_dev dev;
    uint8_t data;                       /* the data latch */
    uint8_t ctl;                        /* the control latch, bits 4-0 */
    uint8_t select, paper_end, error;   /* the printer's inputs */
    uint8_t phase;                      /* MX_LPT_IDLE, BUSY or ACK */
    uint8_t irq_out;
    uint64_t busy_osc, ack_osc, phase_end;
    void (*sink)(void *ctx, uint8_t byte);
    void *sink_ctx;
} mx_lpt;

/* Fills in the port with a printer on line and no sink; no host call is made.
 * The board then calls dev.ops->reset. */
void mx_lpt_init(mx_lpt *p, const mx_hw_host *host, const char *name);
/* The function that receives each printed byte (NULL: bytes are dropped). */
void mx_lpt_set_sink(mx_lpt *p, void (*sink)(void *ctx, uint8_t byte), void *ctx);
/* Busy time and ACK pulse width in OSC periods (at least 1 each). */
void mx_lpt_set_timing(mx_lpt *p, uint64_t busy_osc, uint64_t ack_osc);

#endif /* MX_LPT_H */
