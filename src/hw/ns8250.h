/* MezinXT - the National Semiconductor INS8250 asynchronous communications
 * element, as the IBM PC/XT's Asynchronous Communications Adapter uses it
 * (a reconstruction: the Junior's catalogue has no such board, its BIOS 3.0
 * and 5.0 probe for one). One mx_dev (hw.h) per UART, written from the
 * INS8250 data sheet; it talks to the board only through its host callbacks.
 *
 * Window: eight registers, offsets 0-7 (3F8h for COM1, 2F8h for COM2):
 *   0  RBR (read) / THR (write), the divisor latch's low byte when DLAB is set
 *   1  IER, the divisor latch's high byte when DLAB is set
 *   2  IIR (read only)           3  LCR (bit 7 is DLAB)
 *   4  MCR                       5  LSR
 *   6  MSR                       7  nothing
 * The original 8250 has no scratch register (the 8250A and the 16450 added
 * it): offset 7 is not decoded, reads 0FFh and ignores writes, which is how
 * programs that test for one tell this chip from its successors. Writes to
 * LSR and MSR (the data sheet's factory test) are ignored. There is no FIFO.
 *
 * The clock is the IBM adapter's 1.8432 MHz crystal, taken as 21875/2816 OSC
 * periods an edge (the same ratio as the Junior's SIO adapter): a bit lasts
 * 16 x the divisor of them. A divisor of 0 stops the baud generator, so
 * nothing is sent or received until one is loaded (as on the chip; the
 * divisor latch is not touched by a master reset and starts as 0).
 *
 * Interrupts. The IIR reports, in this order of priority, receiver line
 * status (OE, PE, FE, BI; cleared by reading the LSR), received data
 * available (cleared by reading the RBR), transmitter holding register empty
 * (set when the THR's byte goes into the shift register, cleared by a THR
 * write or by an IIR read that reports it; also set when the IER enables it
 * while the THR is empty) and modem status (cleared by reading the MSR).
 * The line MX_NS8250_IRQ is the INTRPT pin as the IBM adapter wires it: gated
 * by OUT2, a tri-state buffer on the card, and OUT2 is held inactive in
 * loopback mode, so there is no interrupt on the bus then.
 *
 * Time. The transmitter takes a character time per character (start bit,
 * 5-8 data bits, parity, 1, 1.5 or 2 stop bits) from the moment its byte
 * leaves the THR; a character is handed to the byte end when its start bit
 * begins. A received character lands in the RBR at the middle of its stop
 * bit, the line staying busy until the whole frame has gone by. There is one
 * RBR: a character that arrives while DR is set replaces it and sets OE.
 *
 * Byte end (mx_ns8250_end, the same shape as the SIO adapter's mx_sio_end, so
 * the same host ends serve either chip): what is behind the connector, in
 * terms of bytes. `tx` gets each character the guest sends, `rx` offers the
 * next received byte (the chip asks only when the receiver is idle and, with
 * flow control on, DR is clear; the end calls mx_ns8250_rx_notify() when it
 * has a byte after having had none), `tx_break` the break the guest holds on
 * TxD and `modem` the RTS and DTR pins (with the byte end given as unit).
 * Loopback mode (MCR bit 4) joins TxD to the receiver inside the chip and the
 * modem outputs to the modem inputs (RTS to CTS, DTR to DSR, OUT1 to RI, OUT2
 * to DCD) and drives every output pin inactive; a loopback plug on the
 * connector (mx_ns8250_set_plug) does the first of these outside the chip.
 *
 * Pins. Outputs, host->line, 1 asserted: MX_NS8250_IRQ (gated as above),
 * MX_NS8250_RTS, DTR, OUT1 and OUT2. Inputs, ops->input, 1 asserted (the
 * signal's active level, not the wire's): MX_NS8250_CTS, DSR, RI, DCD; they
 * start deasserted and are kept by a reset.
 *
 * Portable C11. MezinXT's own code, from the INS8250 data sheet. */
#ifndef MX_NS8250_H
#define MX_NS8250_H

#include "hw.h"

#define MX_NS8250_IO_SIZE   8u
#define MX_NS8250_COM1_BASE 0x3F8u      /* IRQ 4 */
#define MX_NS8250_COM2_BASE 0x2F8u      /* IRQ 3 */
/* OSC periods per edge of the 1.8432 MHz clock, as a ratio. */
#define MX_NS8250_CLOCK_NUM 21875u
#define MX_NS8250_CLOCK_DEN 2816u

#define MX_NS8250_IRQ  0
#define MX_NS8250_RTS  1
#define MX_NS8250_DTR  2
#define MX_NS8250_OUT1 3
#define MX_NS8250_OUT2 4

#define MX_NS8250_CTS 0
#define MX_NS8250_DSR 1
#define MX_NS8250_RI  2
#define MX_NS8250_DCD 3

/* What is behind the connector; every callback is optional. */
typedef struct mx_ns8250_end {
    void *ctx;
    /* The guest's character: its `bits` (5-8) data bits, right-justified. */
    void (*tx)(void *ctx, unsigned unit, uint8_t data, unsigned bits);
    /* Offers the next received byte: 1 and *byte, or 0 for none. */
    int (*rx)(void *ctx, unsigned unit, uint8_t *byte);
    /* The guest holds TxD low (on 1) or lets it go. */
    void (*tx_break)(void *ctx, unsigned unit, int on);
    /* RTS or DTR changed (1 asserted). */
    void (*modem)(void *ctx, unsigned unit, int rts, int dtr);
} mx_ns8250_end;

typedef struct mx_ns8250 {
    mx_dev dev;
    unsigned unit;                      /* told to the byte end's callbacks */
    mx_ns8250_end end;
    uint8_t flow;                       /* 1 (default): bytes are pulled only while DR is clear */
    uint8_t plug;                       /* loopback plug on the connector */
    uint8_t ier, lcr, mcr, rbr, thr;
    uint16_t divisor;
    uint8_t dr, oe, pe, fe, bi;         /* the LSR's flags */
    uint8_t thr_full, thre_int;
    uint8_t pins;                       /* modem inputs: bit 0 CTS, 1 DSR, 2 RI, 3 DCD */
    uint8_t msr_state, msr_delta;       /* the MSR's bits 7-4 (as bits 3-0) and bits 3-0 */
    uint8_t o_irq, o_rts, o_dtr, o_out1, o_out2;
    uint8_t brk_end, brk_rx;
    uint8_t tx_busy, tsr;
    uint64_t tx_start, tx_end;
    uint8_t rx_busy, rx_landed, rx_data, rx_bad_parity, rx_bad_stop;
    uint64_t rx_start, rx_ready, rx_end;
    uint8_t brk_in, brk_seen;
    uint64_t brk_start, brk_at;
    uint8_t pumping;
} mx_ns8250;

/* Fills in the chip (no byte end, flow control on); no host call is made.
 * The board then calls dev.ops->reset. */
void mx_ns8250_init(mx_ns8250 *u, const mx_hw_host *host, const char *name, unsigned unit);
/* Sets what is behind the connector (NULL: nothing). */
void mx_ns8250_set_end(mx_ns8250 *u, const mx_ns8250_end *end);
/* A loopback plug on the connector: TxD to RxD, nothing reaches the byte end. */
void mx_ns8250_set_plug(mx_ns8250 *u, int on);
/* Pull received bytes only while DR is clear (default), or whenever the
 * receiver is idle (a byte then overruns the RBR as on a wire). */
void mx_ns8250_set_flow_control(mx_ns8250 *u, int on);
/* The end has a byte to give: the chip collects it when the receiver is idle. */
void mx_ns8250_rx_notify(mx_ns8250 *u);
/* A character on RxD, starting now, with a parity bit or stop bit that is not
 * as the receiver expects when asked. 0 when the receiver is busy, a break
 * is on the line or the chip is in loopback mode. */
int mx_ns8250_rx(mx_ns8250 *u, uint8_t byte, int bad_parity, int bad_stop);
/* RxD held low (break) or released. */
void mx_ns8250_rx_break(mx_ns8250 *u, int on);
/* The modem inputs, 1 asserted. */
void mx_ns8250_set_modem(mx_ns8250 *u, int cts, int dsr, int ri, int dcd);
/* The line settings the guest programmed, for ends that care (a mouse):
 * the divisor latch and the line control register. */
unsigned mx_ns8250_divisor(const mx_ns8250 *u);
unsigned mx_ns8250_lcr(const mx_ns8250 *u);
/* The INTRPT pin before the OUT2 gate. */
int mx_ns8250_intrpt(const mx_ns8250 *u);

#endif /* MX_NS8250_H */
