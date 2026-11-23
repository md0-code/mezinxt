/* MezinXT - Motorola MC6845 CRT controller.
 *
 * Written from the MC6845 data sheet. The address register and the
 * register file R0-R17, with their widths and read/write rules:
 *
 *   R0  horizontal total - 1 (characters)         8 bits  write
 *   R1  horizontal displayed (characters)         8 bits  write
 *   R2  horizontal sync position (characters)     8 bits  write
 *   R3  horizontal sync width (characters)        4 bits  write (bits 3-0;
 *       the MC6845's vertical sync width is fixed at 16 raster lines, the
 *       upper nibble is ignored)
 *   R4  vertical total - 1 (character rows)       7 bits  write
 *   R5  vertical total adjust (raster lines)      5 bits  write
 *   R6  vertical displayed (character rows)       7 bits  write
 *   R7  vertical sync position (character rows)   7 bits  write
 *   R8  interlace mode                            2 bits  write (stored; the
 *       interlace modes are not modelled, timing is always non-interlaced)
 *   R9  maximum raster address (lines per row - 1) 5 bits write
 *   R10 cursor start raster (bits 4-0), cursor mode (bits 6-5)  7 bits write
 *   R11 cursor end raster                         5 bits  write
 *   R12 start address high                        6 bits  write
 *   R13 start address low                         8 bits  write
 *   R14 cursor address high                       6 bits  read / write
 *   R15 cursor address low                        8 bits  read / write
 *   R16 light pen address high                    6 bits  read
 *   R17 light pen address low                     8 bits  read
 *
 * The address register is 5 bits and write only: a read returns 0xFF (the
 * chip does not drive the bus). Reading a write-only register returns 0;
 * writing a read-only or a non-existent register (R18-R31) does nothing.
 * A reset (the RESET pin) clears the counters and the light pen strobe
 * latch state but leaves the registers as they are, as on the chip.
 *
 * Cursor modes (R10 bits 6-5): 0 steady, 1 not displayed, 2 blink with a
 * period of 16 fields (8 on, 8 off), 3 blink with a period of 32 fields.
 * The cursor is displayed on the raster lines from the start raster (R10)
 * to the end raster (R11) inclusive, in the cell whose address equals the
 * cursor address; with start above end it covers the lines from start to
 * the bottom of the cell (the chip's comparators are equality compares).
 *
 * Timing. The chip is never clocked: its counters are a function of time.
 * The character clock period is set in OSC periods (mx_mc6845_set_period,
 * the CGA's is 8 for 80 columns and 16 otherwise). The counters are kept
 * as a "beam": their values at one character boundary. Any later time is
 * reached in closed form, so a poll of the status or a read of a counter
 * costs the same however long ago the last one was. A write to R0-R9 first
 * brings the counters up to the current time with the old values and then
 * goes on with the new ones. The counters follow the data sheet's
 * equality compares: a counter that is found above its limit counts on
 * until it wraps (the horizontal counter is 8 bits, the raster counter 5,
 * the row counter 7), so an odd register value gives an odd but
 * deterministic frame.
 *
 * One scan line (a "line") is R0+1 characters; a character row is R9+1
 * lines; a frame is (R4+1) rows plus R5 adjust lines. The raster address
 * counts the adjust lines from 0; the row counter holds R4+1 in them.
 * Display enable is the horizontal counter below R1 and the row counter
 * below R6; HSYNC is the R3 characters from the horizontal counter R2;
 * VSYNC is the 16 lines from the first line of row R7. The memory address
 * is the frame's start address (R12/R13, loaded at the start of each frame)
 * plus R1 per row passed plus the horizontal counter, 14 bits.
 *
 * Light pen: a rising edge of the strobe input (mx_mc6845_lpstb) latches
 * the memory address of the character being scanned into R16/R17. The real
 * chip's latch lags the pen by a few characters; software corrects for it,
 * and the caller picks the `now` it passes.
 *
 * Output pins (host->line): VSYNC, only when mx_mc6845_vsync_line enables
 * it; then the chip keeps one deadline, the next VSYNC edge. Off by
 * default: nothing else uses a deadline.
 *
 * Bus cycles: I/O space, offset bit 0: 0 address register, 1 data register
 * (the board, or the CGA, places the pair).
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_MC6845_H
#define MX_MC6845_H

#include "hw.h"

enum { MX_MC6845_PIN_VSYNC = 0 };
enum { MX_MC6845_CURSOR_STEADY, MX_MC6845_CURSOR_OFF, MX_MC6845_CURSOR_BLINK16, MX_MC6845_CURSOR_BLINK32 };

#define MX_MC6845_MA_MASK 0x3FFFu       /* the memory address is 14 bits */
#define MX_MC6845_REGS 18
#define MX_MC6845_VSYNC_LINES 16u

/* The counters at one character boundary. */
typedef struct mx_mc6845_beam {
    uint64_t t;             /* OSC time of these values */
    uint64_t frame;         /* the field counter: frames since reset */
    uint16_t start;         /* start address latched at the start of this frame */
    uint16_t row_start;     /* memory address of the current row's first character */
    uint8_t h;              /* horizontal counter */
    uint8_t ra;             /* raster address (counts the adjust lines in them) */
    uint8_t row;            /* vertical (character row) counter */
    uint8_t adj;            /* 1 during the vertical total adjust lines */
} mx_mc6845_beam;

typedef struct mx_mc6845 {
    mx_dev dev;
    uint8_t addr;                   /* address register */
    uint8_t reg[MX_MC6845_REGS];    /* R0-R17 */
    uint8_t lpstb;                  /* strobe input level */
    uint8_t vsync_enabled, vsync_level;
    uint64_t period;                /* character clock, OSC periods */
    mx_mc6845_beam beam;
} mx_mc6845;

/* The chip's outputs at one instant. */
typedef struct mx_mc6845_state {
    uint8_t h, ra, row, adj;        /* counters */
    uint16_t ma;                    /* memory address outputs MA0-MA13 */
    uint32_t line;                  /* scan line in the frame, 0-based */
    uint64_t frame;
    int hde, vde, de;               /* horizontal, vertical, display enable */
    int hsync, vsync;
    int cursor;                     /* the CURSOR output */
} mx_mc6845_state;

/* A scan line, for a renderer that steps through a frame line by line.
 * Everything is as it was at the line's first character. */
typedef struct mx_mc6845_line {
    mx_mc6845_beam beam;            /* counters at the first character (h = 0) */
    uint64_t start;                 /* OSC time of the first character */
    uint64_t length;                /* OSC periods to the next line */
    uint32_t index;                 /* scan line in the frame, 0-based: row * (R9+1) + raster */
    uint16_t ma;                    /* memory address of the first character */
    uint8_t row, ra;
    unsigned hdisp;                 /* characters displayed on the line (0 outside the vertical window) */
    int display;                    /* the line is inside the vertical display window */
    int vsync;
    int cursor;                     /* the cursor is on for this line, in the cell at cursor_ma */
    uint16_t cursor_ma;
} mx_mc6845_line;

/* Fills in the chip (registers zero, character period 8, counters at the
 * current host time); the board then calls dev.ops->reset. */
void mx_mc6845_init(mx_mc6845 *c, const mx_hw_host *host, const char *name);
/* The character clock changes at `now` (OSC periods per character, at
 * least 1): the counters are first brought up to `now` at the old period. */
void mx_mc6845_set_period(mx_mc6845 *c, uint64_t now, uint64_t period);
/* A register's stored value (R0-R17; 0 beyond), whatever its read rule. */
uint8_t mx_mc6845_get_reg(const mx_mc6845 *c, unsigned reg);
/* The outputs and counters at `now` (not before the last call's time). */
void mx_mc6845_state_at(mx_mc6845 *c, uint64_t now, mx_mc6845_state *st);
/* The strobe input: a rising edge at `now` latches the light pen registers. */
void mx_mc6845_lpstb(mx_mc6845 *c, uint64_t now, int level);
/* Drives the VSYNC pin through host->line from now on (or stops). */
void mx_mc6845_vsync_line(mx_mc6845 *c, int enable);

/* One scan line / one frame in OSC periods at the present registers and period. */
uint64_t mx_mc6845_line_period(const mx_mc6845 *c);
uint64_t mx_mc6845_frame_period(const mx_mc6845 *c);

/* Line stepping. locate: the line that contains `now`. next: the line after
 * `ln`, with the present registers. refresh: recomputes what the registers
 * decide (display, hdisp, vsync, cursor) from the line's counters, for a
 * line described before a register changed. frame_start: the first line of
 * the frame `ln` is in (the registers are taken as constant over it). */
void mx_mc6845_line_locate(mx_mc6845 *c, uint64_t now, mx_mc6845_line *ln);
void mx_mc6845_line_next(const mx_mc6845 *c, mx_mc6845_line *ln);
void mx_mc6845_line_refresh(const mx_mc6845 *c, mx_mc6845_line *ln);
void mx_mc6845_line_frame_start(const mx_mc6845 *c, mx_mc6845_line *ln);

#endif /* MX_MC6845_H */
