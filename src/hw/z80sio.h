/* MezinXT - Zilog Z80-SIO dual serial channel. Written from the Z80-SIO data sheet and matched to the Junior's
 * adapter as `junior_serial.sv` specifies it.
 *
 * What is modelled: both channels in the asynchronous modes, in full. Write
 * registers 0-7 are accepted, 0-5 act (WR0 pointer and the commands null,
 * reset external/status interrupts, channel reset, enable interrupt on next
 * receive character, reset Tx interrupt pending, error reset, return from
 * interrupt; WR1 external/Tx/Rx interrupt enables, status affects vector and
 * the three receive interrupt modes; WR2 the vector; WR3 receiver enable,
 * auto enables and 5/6/7/8 bits; WR4 parity, stop bits and the x1/x16/x32/x64
 * clock; WR5 DTR, RTS, send break, Tx enable and 5/6/7/8 bits). Read
 * registers 0-2. The interrupt logic with its internal priority chain
 * (receive A, transmit A, external A, receive B, transmit B, external B),
 * the interrupt-under-service state, vectors with status-affects-vector,
 * the 3-character receive FIFO with parity, framing and overrun, the
 * transmit buffer and shift register, break send and detect, DCD, CTS and
 * SYNC as inputs with their external/status latching, auto enables, RTS
 * (held until the transmitter is empty) and DTR.
 *
 * Not modelled: the synchronous modes (monosync, bisync, SDLC): their register
 * writes are accepted and stored, but WR4 bits 5-4 do not change the
 * character engine (a channel whose WR4 stop-bit field is 00 still runs
 * asynchronously with one stop bit, as the Junior's RTL does), the CRC
 * generator and checker, hunt phase, residue codes (RR1 bits 3-1 read 0),
 * end of frame (RR1 bit 7), Tx underrun/EOM (RR0 bit 6), send abort, WAIT/READY.
 *
 * Interface to the board
 *   Bus (ops->read, ops->write; I/O space, offset bits: 0 = control (1) or
 *   data (0), 1 = channel B (1) or A (0)): 0 A data, 1 A control, 2 B data,
 *   3 B control. A control read of a register other than RR0 follows a
 *   pointer write to WR0 and the pointer returns to 0 after every control
 *   access. RR2 reads the vector from either channel's control port.
 *
 *   Output pins (host->line, state 1 asserted):
 *     MX_Z80SIO_OUT_INT      INT, an interrupt is requested
 *     MX_Z80SIO_OUT_RTS(ch)  RTSA/RTSB
 *     MX_Z80SIO_OUT_DTR(ch)  DTRA/DTRB
 *   Input pins (ops->input, state 1 asserted, that is the pin low on the wire):
 *     MX_Z80SIO_IN_CTS(ch), MX_Z80SIO_IN_DCD(ch), MX_Z80SIO_IN_SYNC(ch)
 *   They start deasserted. Each is a status bit of RR0 (CTS 5, SYNC 4, DCD 3)
 *   whose change is an external/status interrupt, and CTS and DCD gate the
 *   transmitter and receiver while WR3 bit 5 (auto enables) is set.
 *
 *   Interrupt acknowledge: mx_z80sio_inta() returns the vector for the
 *   highest-priority interrupt and marks it under service (the Junior reads
 *   the acknowledge ports 258h-25Bh for it); mx_z80sio_vector() is the same
 *   without the marking.
 *
 *   Clocks: the receive and transmit clock inputs are not edge-by-edge pins
 *   but periodic signals described by a rate (mx_z80sio_set_clock): the edge
 *   k of a clock is at OSC time ceil((phase + k * num) / den) and a stopped
 *   clock has num 0. The board (or the card around the chip) sets it from
 *   whatever generates the clock. The x1/x16/x32/x64 divisor of WR4 then
 *   gives the bit time. A character starts on the first clock edge after the
 *   data was written (or the previous character ended) and each bit lasts
 *   the divisor in clock edges (the stop bit 1, 1.5 or 2 bits). The receiver
 *   detects a start bit at the first clock edge, samples the middle of the
 *   start bit (a transient is rejected), and then each bit in its middle,
 *   exactly as the divisor says; a framing error delays the search for the
 *   next start bit by half a bit time. All of it is event driven through
 *   the host's schedule; nothing runs per clock.
 *
 *   The serial lines (TxD, RxD) are carried as characters on the wire, so a
 *   whole character is one host call instead of ten pin changes: the
 *   transmitter hands each character frame to the link's tx_frame at the
 *   time of its start bit, and a character is fed to a receiver with
 *   mx_z80sio_rx_frame(). A frame is the exact line levels (start bit,
 *   data, parity, stop bit) with its start time and bit time, so the
 *   receiver samples the real waveform: a mismatch of character length,
 *   parity or baud rate between the two ends gives wrong data and errors
 *   as it would on a wire. The link's tx_break reports the transmitter's
 *   TxD held low (WR5 bit 4), mx_z80sio_rx_break() holds RxD low: the
 *   receiver loads one null character with a framing error, sets the
 *   Break bit and interrupts (external/status) at the start and again at the
 *   end. rx_ready tells the board that the receiver can take a frame
 *   (enabled, clocked, idle) so that a byte end with flow control can pull
 *   its next byte at the receiver's pace.
 *
 * Choices where the data sheet leaves room: the receiver
 * keeps the first characters on overrun when `overrun_keeps` is set (the
 * adapter's RTL: the new character is lost) and otherwise follows the data
 * sheet (the new character replaces the last one in the FIFO); RR1 reports
 * parity and overrun as latched until an error reset and framing for the
 * character at the head of the FIFO; the Tx interrupt comes when the
 * transmit buffer becomes empty (not on enabling); a break sets Break/Abort
 * (RR0 bit 7) at its start; "return from interrupt" is accepted on either
 * channel (the Junior's RTL does; the data sheet says channel A).
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_Z80SIO_H
#define MX_Z80SIO_H

#include "hw.h"

enum { MX_Z80SIO_A = 0, MX_Z80SIO_B = 1 };

/* Output pins (host->line) and input pins (ops->input). */
#define MX_Z80SIO_OUT_INT     0
#define MX_Z80SIO_OUT_RTS(ch) (1 + (ch))
#define MX_Z80SIO_OUT_DTR(ch) (3 + (ch))
#define MX_Z80SIO_IN_CTS(ch)  (0 + (ch))
#define MX_Z80SIO_IN_DCD(ch)  (2 + (ch))
#define MX_Z80SIO_IN_SYNC(ch) (4 + (ch))

/* RR0 bits. */
enum {
    MX_Z80SIO_RR0_RXAVAIL = 0x01, MX_Z80SIO_RR0_INTPEND = 0x02, MX_Z80SIO_RR0_TXEMPTY = 0x04, MX_Z80SIO_RR0_DCD = 0x08,
    MX_Z80SIO_RR0_SYNC = 0x10, MX_Z80SIO_RR0_CTS = 0x20, MX_Z80SIO_RR0_UNDERRUN = 0x40, MX_Z80SIO_RR0_BREAK = 0x80
};
/* RR1 bits. */
enum { MX_Z80SIO_RR1_ALLSENT = 0x01, MX_Z80SIO_RR1_PARITY = 0x10, MX_Z80SIO_RR1_OVERRUN = 0x20, MX_Z80SIO_RR1_FRAMING = 0x40 };

/* A clock input: edge k at OSC time ceil((phase + k * num) / den); num 0 stops it. */
typedef struct mx_z80sio_clock {
    uint64_t num, den, phase;
} mx_z80sio_clock;

/* Time of edge k, and the index of the first edge at or after OSC time t
 * (after t when `strict`). */
static inline uint64_t mx_z80sio_edge_time(const mx_z80sio_clock *c, uint64_t k) {
    return (c->phase + k * c->num + c->den - 1u) / c->den;
}
static inline uint64_t mx_z80sio_edge_after(const mx_z80sio_clock *c, uint64_t t, int strict) {
    const uint64_t r = strict ? t + 1u : t;
    if (r == 0) return 0;
    const uint64_t limit = (r - 1u) * c->den;
    if (c->phase > limit) return 0;
    return (limit - c->phase) / c->num + 1u;
}

/* One character on a line: the levels from the start bit on (bit 0 of
 * `bits` is the start bit, 0), padded by the idle level (1) after `length`
 * bits; `start` is the OSC time of the start bit's leading edge and a bit
 * lasts bit_num / bit_den OSC periods. `data` and `nbits` are the data bits
 * of a transmitted character, right-justified, for a byte end. `brk` marks
 * the frame of a held break (all zero). */
typedef struct mx_z80sio_frame {
    uint16_t bits;
    uint8_t length;
    uint8_t data, nbits;
    uint8_t brk;
    uint64_t start;
    uint64_t bit_num, bit_den;
} mx_z80sio_frame;

/* What the chip tells the board about the lines (all optional). */
typedef struct mx_z80sio_link {
    void *ctx;
    /* A character starts on TxD of `channel`. */
    void (*tx_frame)(void *ctx, unsigned channel, const mx_z80sio_frame *frame);
    /* TxD is held low (on 1) or released. */
    void (*tx_break)(void *ctx, unsigned channel, int on);
    /* The receiver of `channel` can take a frame now. */
    void (*rx_ready)(void *ctx, unsigned channel);
} mx_z80sio_link;

typedef struct mx_z80sio_chan {
    uint8_t wr[8];
    uint8_t ptr;
    uint8_t in_cts, in_dcd, in_sync;            /* input pins, 1 asserted */
    uint8_t ext_live, ext_snap, ext_latched, ext_ip;
    uint8_t tx_ip, rx_armed;
    /* receiver */
    uint8_t fifo[3], fifo_err[3], head, used;
    uint8_t overrun, parity_latch;
    uint8_t rx_state;                           /* 0 idle, 1 receiving a frame, 2 recovering after a framing error */
    uint8_t rx_hold, rx_in_break, rx_ready_reported;
    uint8_t rx_d, rx_nb, rx_par;                /* divisor, data bits and parity of the frame in progress */
    mx_z80sio_frame rx_frame;
    mx_z80sio_clock rx_snap;
    uint64_t rx_k0;
    uint64_t rx_done_at, rx_free_at;
    /* transmitter */
    uint8_t tx_buf, tx_full, tx_active, tx_break_on;
    uint64_t tx_start_at, tx_end_at;
    mx_z80sio_clock rx_clk, tx_clk;
    uint8_t rts_out, dtr_out;
} mx_z80sio_chan;

typedef struct mx_z80sio {
    mx_dev dev;
    mx_z80sio_chan ch[2];
    uint8_t in_service;                         /* bit j: level j under service (priority order, 0 highest) */
    uint8_t int_out;
    uint8_t overrun_keeps;                      /* 1: an overrun loses the new character (the RTL); 0: it replaces the last one */
    mx_z80sio_link link;
} mx_z80sio;

/* Sets the chip up on `host` (default data sheet overrun behaviour, no link,
 * clocks stopped) and resets it. No host call is made; the board then calls
 * dev.ops->reset. */
void mx_z80sio_init(mx_z80sio *s, const mx_hw_host *host, const char *name);
void mx_z80sio_set_link(mx_z80sio *s, const mx_z80sio_link *link);
/* Sets the receive clock (`which` bit 0), the transmit clock (bit 1) or both
 * of a channel. NULL stops it. A character in flight keeps its timing. */
void mx_z80sio_set_clock(mx_z80sio *s, unsigned channel, unsigned which, const mx_z80sio_clock *clock);

/* The receiver of a channel can take a frame (enabled, clocked, not busy). */
int mx_z80sio_rx_ready(const mx_z80sio *s, unsigned channel);
/* Feeds a frame to the receiver; 0 when it is not ready (the frame is lost
 * on a wire, a byte end keeps its byte). The frame's start time is taken as
 * `now` when it lies in the past. */
int mx_z80sio_rx_frame(mx_z80sio *s, unsigned channel, const mx_z80sio_frame *frame);
/* Builds the well-formed frame of `value` in the channel's receive format
 * (data bits, parity, one stop bit) at the receive clock's bit time. */
void mx_z80sio_make_frame(const mx_z80sio *s, unsigned channel, uint8_t value, uint64_t start, mx_z80sio_frame *frame);
/* RxD held low (on 1) or released. */
void mx_z80sio_rx_break(mx_z80sio *s, unsigned channel, int on);

/* The vector as an acknowledge would give it now, and the acknowledge itself. */
uint8_t mx_z80sio_vector(const mx_z80sio *s);
uint8_t mx_z80sio_inta(mx_z80sio *s);
/* Characters in a channel's receive FIFO, for flow control. */
unsigned mx_z80sio_rx_count(const mx_z80sio *s, unsigned channel);

#endif /* MX_Z80SIO_H */
