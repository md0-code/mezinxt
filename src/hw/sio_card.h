/* MezinXT - the Junior's asynchronous serial adapter (703.910.050, schematic
 * sheets 17-19; manual 3.4): a Z80-SIO, a three-channel 8253 for the baud
 * rates and the bus interface around them, as this project's
 * `junior_serial.sv` specifies it. One mx_dev (hw.h) that owns the SIO
 * (z80sio.h) and the 8253 (i8253.h) as plain structs behind an internal host
 * and talks to the board only through its own host callbacks.
 *
 * Board interface: one I/O window of 16 bytes at 250h (the alternative strap
 * is 260h: the board maps the window where it wants), MX_SIO_CARD_IO_BASE and
 * MX_SIO_CARD_IO_SIZE, offsets inside it:
 *   0, 1   SIO channel A data, control      2, 3   channel B data, control
 *   4-7    the baud 8253 (counter 0 for channel A, counter 1 for channel B,
 *          counter 2 free, control word)
 *   8-11   interrupt acknowledge: a read serves the SIO's vector for the
 *          highest-priority pending interrupt and marks it under service
 *          (the manual: reading it in the interrupt routine is necessary,
 *          it releases the SIO); writes are ignored
 *   12-15  modem status: bit 0 is DSR of channel A, bits 7-1 read 0
 * The adapter holds every cycle for one wait state of 210 ns (the manual:
 * "stare de asteptare de 210 ns la fiecare transfer"), returned by ops->wait
 * as MX_SIO_CARD_WAIT_OSC OSC periods (3 = 209.5 ns); the board's own wait
 * state for I/O cycles comes on top.
 *
 * Pins. Outputs, host->line, state 1 asserted:
 *   MX_SIO_CARD_IRQ      the SIO's INT; the strap routes it to IRQ 4 (default)
 *                        or IRQ 3
 *   MX_SIO_CARD_RTS(ch), MX_SIO_CARD_DTR(ch)   the modem outputs
 * Inputs, ops->input, state 1 asserted (the signal's active level, not the
 * wire's; the Junior's 1488/1489 drivers invert):
 *   MX_SIO_CARD_CTS(ch), MX_SIO_CARD_DCD(ch)   to the SIO
 *   MX_SIO_CARD_DSR(ch)  channel A's is the status port's bit 0; B's is wired
 *                        to nothing readable
 * They start deasserted. The board forwards them from the byte ends or sets
 * them with mx_sio_card_set_modem().
 *
 * The baud clock: the 18.432 MHz crystal divided by 10 by U5 (a 7490), 1.8432
 * MHz, feeds the three 8253 counters, and OUT0 and OUT1 are channel A's and
 * B's transmit and receive clocks (RxC and TxC tied together). The 8253 is
 * the real chip of i8253.h clocked at 21875/2816 OSC periods per edge (OSC =
 * 157.5/11 MHz, so 1.8432 MHz exactly: 14318181.8 * 2816 / 21875 =
 * 1843200). The card turns each counter's programming into the SIO's clock:
 * a counter in mode 3 or 2 with a count of N >= 2 gives one SIO clock edge
 * per N 8253 clocks, phased on the 8253's own clock grid from the moment the
 * count was loaded; other modes give no clock. Count 12 and the x16 divisor
 * give 9600 baud (1.8432 MHz / 12 / 16), as the BIOS programs it.
 *
 * Byte ends. Each channel has an optional end (mx_sio_end): what is behind
 * the connector, in terms of bytes: a callback for each character the guest
 * sends (`tx`, called when the character's start bit begins, with its data
 * bits and their number), a callback that hands over the next received byte
 * if there is one (`rx`: flow control: the card asks only when the
 * receiver is enabled, free and, with flow control on, the SIO's FIFO has
 * room, so a byte the guest cannot take yet waits in the end; the end calls
 * mx_sio_card_rx_notify() when it has a byte after having had none), the
 * break line (`tx_break`) and the modem outputs (`modem`: RTS and DTR).
 * A received byte is framed in the channel's receive format (data bits,
 * parity, one stop bit) at the channel's baud rate, so it takes a
 * character time to arrive, as on a wire. Break received:
 * mx_sio_card_rx_break(). A channel can also be put in loopback
 * (mx_sio_card_set_loopback): its TxD goes to its own RxD, bit for bit in
 * time (the guest's own format, so a mismatch gives errors), and nothing
 * reaches the end.
 *
 * Reset: the SIO and the 8253 are reset, the baud clocks stop, the modem
 * inputs and the byte ends are kept.
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_SIO_CARD_H
#define MX_SIO_CARD_H

#include "hw.h"
#include "i8253.h"
#include "z80sio.h"

#define MX_SIO_CARD_IO_BASE 0x250u
#define MX_SIO_CARD_IO_SIZE 16u
#define MX_SIO_CARD_WAIT_OSC 3u

#define MX_SIO_CARD_IRQ     0
#define MX_SIO_CARD_RTS(ch) (1 + (ch))
#define MX_SIO_CARD_DTR(ch) (3 + (ch))
#define MX_SIO_CARD_CTS(ch) (0 + (ch))
#define MX_SIO_CARD_DCD(ch) (2 + (ch))
#define MX_SIO_CARD_DSR(ch) (4 + (ch))

/* What is behind one channel's connector; every callback is optional. */
typedef struct mx_sio_end {
    void *ctx;
    /* The guest's character: its `bits` (5-8) data bits, right-justified. */
    void (*tx)(void *ctx, unsigned channel, uint8_t data, unsigned bits);
    /* Offers the next received byte: 1 and *byte, or 0 for none. */
    int (*rx)(void *ctx, unsigned channel, uint8_t *byte);
    /* The guest holds TxD low (on 1) or lets it go. */
    void (*tx_break)(void *ctx, unsigned channel, int on);
    /* RTS or DTR changed (1 asserted). */
    void (*modem)(void *ctx, unsigned channel, int rts, int dtr);
} mx_sio_end;

typedef struct mx_sio_card {
    mx_dev dev;
    mx_z80sio sio;
    mx_i8253 pit;
    mx_hw_host sio_host, pit_host;      /* what the chips see */
    mx_sio_end end[2];
    uint8_t loop[2];                    /* TxD to RxD */
    uint8_t flow;                       /* 1 (default): bytes are pulled only while the FIFO has room */
    uint8_t live;                       /* the card has been reset: the host's time is valid */
    uint8_t dsr[2];                     /* DSR inputs */
    uint8_t rts[2], dtr[2];
    uint8_t pumping[2];
    uint8_t pit_run[2];                 /* counter n gives the SIO a clock */
    uint32_t pit_n[2];                  /* its count */
    uint64_t pit_k0[2];                 /* the 8253 clock edge its cycles start on */
} mx_sio_card;

/* Fills in the adapter (no byte ends, no loopback, flow control on); no host
 * call is made. The board then calls dev.ops->reset. */
void mx_sio_card_init(mx_sio_card *c, const mx_hw_host *host, const char *name);
/* Connects channel 0 (A) or 1 (B) to a byte end (NULL: nothing behind it). */
void mx_sio_card_set_end(mx_sio_card *c, unsigned channel, const mx_sio_end *end);
/* The channel's TxD to its own RxD. */
void mx_sio_card_set_loopback(mx_sio_card *c, unsigned channel, int on);
/* Pull received bytes only while the SIO's FIFO has room (default), or
 * whenever the receiver is free (the end's bytes then overrun like a wire's). */
void mx_sio_card_set_flow_control(mx_sio_card *c, int on);
/* The end has a byte to give: the card collects it when the receiver is free. */
void mx_sio_card_rx_notify(mx_sio_card *c, unsigned channel);
/* RxD held low (break received) or released. */
void mx_sio_card_rx_break(mx_sio_card *c, unsigned channel, int on);
/* The modem inputs of a channel, 1 asserted. */
void mx_sio_card_set_modem(mx_sio_card *c, unsigned channel, int cts, int dcd, int dsr);

#endif /* MX_SIO_CARD_H */
