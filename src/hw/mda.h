/* MezinXT - the monochrome display coupler (703.300.003).
 * The catalogue lists the coupler but its interface is not documented, so this
 * is a reconstruction: the IBM Monochrome Display and Printer Adapter's design
 * as publicly documented (an MC6845 (mc6845.h) with a 9-dot character clock, a
 * 4 KiB text store, a mode control register and a status register), written
 * from the adapter's technical reference behaviour. The printer port of that
 * card (3BCh) is the Junior's own lpt chip (hw/lpt.h), placed by the board's
 * card (core/mda_card.h).
 *
 * Board interface (hw.h). One mx_dev with two windows, both given as offsets
 * inside the window:
 *   memory space  B0000h-B7FFFh (MX_MDA_MEM_*): the 4 KiB store, repeated
 *                 eight times as on the IBM card (address lines A12-A14 are
 *                 not decoded). Accesses never wait and never cause snow.
 *   I/O space     3B0h-3BBh (MX_MDA_IO_*):
 *     3B0,2,4,6   MC6845 address register   |  3B1,3,5,7  MC6845 data
 *                 (the pair repeats over 3B0h-3B7h as on the IBM card)
 *     3B8h        mode control, write only
 *     3BAh        status, read only
 *     3B9h, 3BBh  unused
 *   A read of a write-only or unused port returns 0xFF (open bus). No
 *   interrupts, no DMA, no deadlines: the picture is rendered lazily from the
 *   CRTC's counters.
 *
 * Mode control 3B8h: bit 0 high resolution (the 9-dot character clock; the
 * card shows nothing while it is clear), bit 3 video enable (clear: a blank
 * screen), bit 5 blink enable (attribute bit 7 blinks; clear: ignored).
 * Status 3BAh: bit 0 horizontal sync (the CRTC's HSYNC), bit 3 the video
 * signal (1 while the beam is on a lit dot, so it follows the screen's
 * contents dot by dot), bits 7-4 read 1, the others 0. A program tests the card by
 * waiting for the video bit to rise and fall (BIOS POST).
 *
 * Timing. The card has its own crystal, 16.257 MHz, which is the dot clock
 * (a character is 9 dots), not derived from the board's oscillator: the dot
 * clock is OSC * 59609/52500 exactly (MX_MDA_DOT_NUM / MX_MDA_DOT_DEN), and
 * the CRTC runs in dot-clock periods (its character period is 9). The
 * standard register set of the IBM BIOS (R0 = 61h: 98 characters, R4 = 19h and
 * R5 = 6: 26 rows of 14 lines and 6 lines of adjust = 370 lines) makes a line
 * 54.25 us (18.432 kHz) and a frame 49.82 Hz.
 *
 * The picture. The frame is MX_MDA_FRAME_W x MX_MDA_FRAME_H bytes, one byte per
 * dot: 0 dark, 1 normal intensity, 2 intense (the monochrome monitor's two
 * levels; core/machine.h's MX_FRAME_MONO). The top left of the frame is the
 * CRTC's first character of its first row; what the programmed window does not
 * cover (right of R1 characters, below R6 rows) and the whole screen while
 * video is disabled is dark (the card has no border). The last complete frame
 * is returned.
 *
 * Text, 80 x 25 cells of 9 x 14 dots. The cell's character byte is at the even
 * address, the attribute at the odd one, the CRTC's memory address MA0-MA11
 * over A1-A12. The character generator is MezinXT's own 8 x 14 font derived
 * from the Junior's character ROM (mx_mda_font_build); the ninth dot is blank,
 * except for the characters C0h-DFh, where it repeats the eighth. Attribute:
 *   background bits 6-4 all set (70h, 78h, F0h, F8h): reverse video, the
 *     dots of the character dark on a lit cell; any other background is dark
 *   foreground bits 2-0: 0 shows nothing (00h and 08h are invisible), 1
 *     underlines scan line 12 of the cell (all nine dots), any other value is
 *     the normal character; a reverse cell takes the underline too
 *   bit 3 intense: the lit dots (the character, or a reverse cell's whole
 *     background) at the higher level
 *   bit 7 with blink enabled: the character (and underline) disappears for 16
 *     fields and shows for 16 (a reverse cell then shows its plain
 *     background)
 * The cursor (shape and address from the CRTC; the card blinks it 8 fields on
 * and 8 off) lights the cell's nine dots on the cursor's scan lines whatever
 * the attribute, at the attribute's intensity.
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_MDA_H
#define MX_MDA_H

#include "hw.h"
#include "mc6845.h"

#define MX_MDA_MEM_BASE 0xB0000u
#define MX_MDA_MEM_WINDOW 0x8000u       /* B0000h-B7FFFh */
#define MX_MDA_VRAM_SIZE 0x1000u        /* the 4 KiB store, repeated through the window */
#define MX_MDA_IO_BASE 0x3B0u
#define MX_MDA_IO_SIZE 12u              /* 3B0h-3BBh; the printer port at 3BCh is lpt.h's */
#define MX_MDA_PRINTER_BASE 0x3BCu
#define MX_MDA_FRAME_W 720u
#define MX_MDA_FRAME_H 350u
#define MX_MDA_CELL_W 9u
#define MX_MDA_CELL_H 14u
#define MX_MDA_SOURCE_BYTES 2048u       /* the Junior's character ROM: 256 glyphs of 8 rows */
#define MX_MDA_GLYPH_BYTES (256u * MX_MDA_CELL_H)   /* the derived font: 256 glyphs of 14 rows */

/* The dot clock as a ratio of the board's oscillator: OSC * NUM / DEN = 16.257 MHz. */
#define MX_MDA_DOT_NUM 59609u
#define MX_MDA_DOT_DEN 52500u

/* The frame's levels. */
enum { MX_MDA_DARK = 0, MX_MDA_NORMAL = 1, MX_MDA_INTENSE = 2 };

typedef struct mx_mda {
    mx_dev dev;
    mx_hw_host dots;                    /* the CRTC's host: time in dot-clock periods */
    mx_mc6845 crtc;
    uint8_t mode;
    uint8_t glyph[MX_MDA_GLYPH_BYTES];
    uint8_t vram[MX_MDA_VRAM_SIZE];
    /* renderer: the next scanline to draw, the frame being drawn and the frame last completed */
    mx_mc6845_line line;
    int line_valid, draw_valid;
    uint64_t draw_frame, last_frame;
    uint32_t last_index;
    uint32_t rows_done;
    uint32_t front;
    uint64_t serial;
    uint8_t frames[2][MX_MDA_FRAME_W * MX_MDA_FRAME_H];
} mx_mda;

/* The font of the card from the Junior's character ROM (`glyphs8`,
 * MX_MDA_SOURCE_BYTES): the 8 dots of each row are kept, the 8 rows are placed
 * in the 14 lines of the cell by mx_mda_font_row. `out` is MX_MDA_GLYPH_BYTES,
 * the 8 dots of line n of character c at out[c * 14 + n], leftmost dot in bit 7.
 * No other font is used. */
void mx_mda_font_build(uint8_t *out, const uint8_t *glyphs8);
/* Which row (0-7) of the character ROM's glyph line `line` (0-13) of the cell shows,
 * or -1 for a blank line. Text: line 0 blank (the leading above the character), each of
 * the rows 0, 2, 4, 6 and 7 on two lines and each of 1, 3 and 5 on one (lines 1-13), so
 * the capitals stand on line 11, the descender row on 12-13 and the underline (line 12)
 * sits right under the baseline. The line-drawing and block characters B0h-DFh take
 * every line (the row nearest to its centre: 0 0 1 2 2 3 3 4 4 5 6 6 7 7), so that
 * vertical strokes and blocks join from cell to cell. */
int mx_mda_font_row(unsigned ch, unsigned line);

/* Fills in the card (power-on state: mode 0, store cleared) with a copy of `glyphs`
 * (MX_MDA_GLYPH_BYTES, from mx_mda_font_build; NULL: all blank); the struct is
 * large, allocate it, and do not move it. The board then calls dev.ops->reset. */
void mx_mda_init(mx_mda *m, const mx_hw_host *host, const char *name, const uint8_t *glyphs);
/* Draws what is due up to the host's `now` and returns the last complete frame
 * (MX_MDA_FRAME_W x MX_MDA_FRAME_H, pitch MX_MDA_FRAME_W). *serial (may be NULL) changes
 * whenever the frame's pixels do. */
const uint8_t *mx_mda_frame(mx_mda *m, uint64_t *serial);
/* The present frame period in OSC periods (287421 in the standard mode: 49.82 Hz). */
uint64_t mx_mda_frame_period(const mx_mda *m);
/* OSC periods to dot-clock periods (rounded down) and back (rounded up). */
uint64_t mx_mda_osc_to_dots(uint64_t osc);
uint64_t mx_mda_dots_to_osc(uint64_t dots);

#endif /* MX_MDA_H */
