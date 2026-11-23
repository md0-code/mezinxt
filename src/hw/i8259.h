/* MezinXT - Intel 8259A programmable interrupt controller.
 *
 * One 8259A as on the PC/XT and the Junior: eight interrupt request inputs,
 * the INT output to the 8086's INTR, a single acknowledge call. Written from
 * the 8259A data sheet.
 *
 * Pins (hw.h): the board drives IR0-IR7 through ops->input, pin numbers 0-7,
 * state 1 = the line is high; with edge triggering a request is the rising
 * edge (latched in the IRR until acknowledged), with level triggering the
 * IRR follows the line. The chip drives INT through host->line, pin
 * MX_I8259_INT, state 1 = INT high (the CPU's INTR asserted).
 *
 * Bus cycles: I/O space, offset bit 0 is A0 (the Junior decodes 20h-21h);
 * the other offset bits are ignored. Reads: A0 = 0 the IRR or ISR as OCW3
 * selected (or the poll word), A0 = 1 the IMR. Writes: ICW1-4 and OCW1-3.
 *
 * Interrupt acknowledge: when the CPU runs its INTA cycles the board calls
 * mx_i8259_inta once and puts the returned byte on the data bus (the
 * vector). The two INTA pulses of the real chip are one step here: the ISR
 * bit is set (unless AEOI), an edge-triggered IRR bit cleared and INT
 * released. With no request left (a request withdrawn or masked after INT
 * rose) the vector of IR7 is returned and nothing is set: the spurious
 * interrupt.
 *
 * Not modelled: the MCS-80 vector sequence (the vector byte is always the
 * 8086 one), the cascade lines and the slave side (ICW3 is accepted and kept;
 * a cascaded board would arbitrate in its own code), the buffered-mode pin
 * SP/EN (ICW4 is kept).
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_I8259_H
#define MX_I8259_H

#include "hw.h"

/* Input pins (ops->input). */
enum { MX_I8259_IR0, MX_I8259_IR1, MX_I8259_IR2, MX_I8259_IR3, MX_I8259_IR4, MX_I8259_IR5, MX_I8259_IR6, MX_I8259_IR7 };
/* Output pin (host->line). */
enum { MX_I8259_INT = 0 };

typedef struct mx_i8259 {
    mx_dev dev;
    uint8_t irr, isr, imr;
    uint8_t lines;          /* the input pins' levels */
    uint8_t icw1, icw2, icw3, icw4;
    uint8_t icw_next;       /* 0 initialised, else the ICW (2, 3 or 4) the next write at A0 = 1 is */
    uint8_t lowest;         /* the lowest-priority level: 7 after ICW1 (IR0 highest) */
    uint8_t read_isr;       /* OCW3: a read at A0 = 0 returns the ISR (else the IRR) */
    uint8_t poll;           /* OCW3 poll command given: the next read at A0 = 0 is the poll word */
    uint8_t special_mask;   /* special mask mode */
    uint8_t auto_rotate;    /* rotate on automatic EOI */
    uint8_t int_out;        /* the INT output's level */
} mx_i8259;

/* Sets the chip up on `host` and resets it. The board then wires the pins. */
void mx_i8259_init(mx_i8259 *c, const mx_hw_host *host);
/* The CPU's interrupt acknowledge: the vector byte for the data bus. */
uint8_t mx_i8259_inta(mx_i8259 *c);
/* The INT output's level (what the last host->line said). */
int mx_i8259_int(const mx_i8259 *c);

#endif /* MX_I8259_H */
