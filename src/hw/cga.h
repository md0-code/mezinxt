/* MezinXT - the FEPER JUNIOR-XT's colour/graphics adapter. The IBM CGA's design as the Junior's schematic (sheets 8-10)
 * has it: an MC6845 (mc6845.h) on a 14.318 MHz dot clock, a mode register,
 * a colour-select register, a status register and a video store.
 *
 * Board interface (hw.h). One mx_dev with two windows, both given as
 * offsets inside the window:
 *   memory space  B8000h-BFFFFh (MX_CGA_MEM_*): the video store, a full
 *                 32 KiB with no 16 KiB mirror (the Junior keeps CPU A14
 *                 and CRTC MA13 wired; the IBM CGA's 16 KiB repeats). A
 *                 16-bit store: an 8086 word cycle is one cycle (the
 *                 window takes words whole). The CPU's accesses wait
 *                 for the card's CPU slot (below); in 80-column text
 *                 that slot is taken from the picture (snow, below).
 *   I/O space     3D0h-3DFh (MX_CGA_IO_*):
 *     3D0,2,4,6   MC6845 address register   |  3D1,3,5,7  MC6845 data
 *                 (the pair repeats over 3D0h-3D7h as on the IBM CGA)
 *     3D8h        mode register, write only
 *     3D9h        colour select, write only
 *     3DAh        status, read only
 *     3DBh        write: clear the light pen latch
 *     3DCh        write: set the light pen latch (preset)
 *     3DDh-3DFh   unused
 *   A read of a write-only or unused port returns 0xFF (open bus).
 * Input pins (ops->input): MX_CGA_PIN_PEN a rising edge is the pen seeing
 * the beam (it sets the latch, like a write to 3DCh); MX_CGA_PIN_PEN_SWITCH
 * state 1 is the pen's switch closed. No output pins. The picture is
 * rendered lazily from the CRTC's counters; the only deadline is the light
 * pen's, below.
 *
 * A light pen placed on the picture (mx_cga_pen_place) sees the beam pass its
 * spot once a frame, at the time the beam is on that pixel, and raises the
 * strobe then, provided the spot is lit: the pen is a light sensor, so a
 * black spot gives no pulse. The latch then holds the address of the
 * character the beam was on (the real latch lags the pen by a few
 * characters; software corrects for it) until the program clears it with a
 * write to 3DBh. A write to 3DCh sets the latch with no pen at all.
 *
 * Mode register 3D8h: bit 0 80-column text (character clock OSC/8, else
 * OSC/16), bit 1 graphics, bit 2 black and white (selects the cyan/red/white
 * palette in 320x200 graphics), bit 3 video enable, bit 4 640x200 graphics
 * (with bit 1), bit 5 blink enable (attribute bit 7 blinks instead of being
 * a bright background).
 * Colour select 3D9h: bits 3-0 border colour and, in 320x200, the background;
 * in 640x200 the foreground (the border there is black); bit 4 intensity of
 * the 320x200 foreground colours; bit 5 palette (0 green/red/brown, 1
 * cyan/magenta/white). With video disabled the screen shows the border.
 * Status 3DAh: bit 0 display disabled (the CRTC's display enable inverted:
 * 1 in the blanking and outside the programmed window), bit 1 light pen
 * latch set, bit 2 pen switch open (1 = open), bit 3 vertical retrace (the
 * CRTC's VSYNC, 16 lines), bits 7-4 read 1.
 *
 * Video store addressing, from the CRTC's memory address MA and raster
 * address RA (byte address, 15 bits): text, MA13-MA0 over A14-A1 (char at
 * the even byte, attribute at the odd); graphics, MA13 on A14, RA0 on A13,
 * MA11-MA0 on A12-A1 (so even lines at 0000h, odd at 2000h; MA12 is not
 * used); A0 is the byte of the pair. Pages above 16 KiB are reached with
 * MA13 (start address 2000h words).
 *
 * The picture. The frame is MX_CGA_FRAME_W x MX_CGA_FRAME_H bytes, one
 * byte per pixel, an RGBI index 0-15 (src/core/machine.h: bit 0 blue, 1
 * green, 2 red, 3 intensity; brown stays index 6, it is the monitor's
 * business). One pixel is one OSC period wide: an 80-column cell is 8
 * pixels, a 40-column cell and a graphics cell 16 (so a 320x200 pixel is
 * 2 wide). The frame's top left is the CRTC's first character of its first
 * row; the region outside the programmed window (right of R1 characters,
 * below R6 rows) is the border colour. The frame is built scanline by
 * scanline from the CRTC's counters: a scanline is drawn when its
 * displayed part has been scanned, from the registers and the store as
 * they are then, and every write that matters draws what is due first, so
 * a mid-frame change (a colour, a palette, a mode, a CRTC register) shows
 * from the line it happened on. The last complete frame is the one
 * returned. The character generator is the Junior's 256 glyphs of 8 rows
 * (the address of the glyph row is RA modulo 8), copied at init.
 *
 * Wait states (schematic sheet 9). The
 * card's timing chain is a twisted ring of eight stages clocked by OSC
 * (74LS164 U16), cleared by RESET: 16 states, two RAM cycles of 8 OSC
 * periods each (RAS in states 1-4 and 9-12). A memory access pulls I/O CH
 * RDY low from its command; the select is sampled at the entry to state 8
 * (U36, clock Q8), the CPU gets the RAM cycle of states 9-12 and the
 * release follows the end of its RAS (U36, clock RAS2). ops->wait gives, from
 * the command's leading edge `now`, the time the board can sample that
 * release (the window is MX_BUS_IOCHRDY): the select needs 2 OSC periods to
 * reach the sampling flip-flop, the release 7 after the sample; at OSC time
 * t the chain is in state (t - reset - 1) mod 16. The same in every mode;
 * no wait on the I/O registers. A write reaches the store at the CPU slot's
 * CAS, 2 periods after the sample, and is drawn as of then.
 *
 * Snow (sheets 8-10). In 80-column text (mode
 * bit 0, HRES) both RAM cycles of the chain fetch a character for the
 * picture, and the CPU's cycle takes the second (states 9-12) from it; the
 * character and attribute latches still latch the RAM data bus at the end
 * of that cycle (CCLATCH, entry to state 12). So the cell the CRTC
 * addressed for it (the character whose address the CRTC latches at the
 * entry to state 5, 3 periods before the sample) shows instead, on that
 * one scanline: for a read, the character and attribute stored at the
 * CPU's word address; for a write, the CPU's data in the half (or halves)
 * written and, in the other half, what that half of the card's data bus
 * last carried (no driver holds it then, and nothing on the net pulls it:
 * it keeps its level for the microsecond or so that matters; the bus's
 * two halves are tracked as the last byte each carried in a memory or I/O
 * cycle, `dbus`). Only for cycles that asked ops->wait (the CPU's: the slot
 * is known); none in 40 columns and graphics (slot B is the CPU's alone),
 * with video disabled, or in the blanking and the border (the latches are
 * cleared, the border colour shown). One cell for each such cycle: a cycle
 * is one slot, and the CPU's next one is two chain cycles (32 periods)
 * later at the soonest, so no cell is taken twice.
 *
 * The snow can be turned off (mx_cga_set_snow; on at power-on): not original,
 * the CPU's cycles then change the picture in no way. The wait states stay.
 *
 * Text cells: the cursor from the CRTC (cursor shape and mode in R10/R11,
 * address in R14/R15) is blinked by the card as well, 8 fields on and 8
 * off; blinking characters (mode bit 5) are 16 on and 16 off. A cursor
 * cell shows the foreground colour on its cursor lines.
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_CGA_H
#define MX_CGA_H

#include "hw.h"
#include "mc6845.h"

#define MX_CGA_MEM_BASE 0xB8000u
#define MX_CGA_MEM_SIZE 0x8000u
#define MX_CGA_IO_BASE  0x3D0u
#define MX_CGA_IO_SIZE  0x10u
#define MX_CGA_FRAME_W  640u
#define MX_CGA_FRAME_H  200u
#define MX_CGA_GLYPH_BYTES 2048u        /* 256 characters of 8 rows */

enum { MX_CGA_PIN_PEN = 0, MX_CGA_PIN_PEN_SWITCH = 1 };

/* The timing chain (OSC periods): its cycle, the state whose entry samples
 * a CPU select, the select's way from the command to the sampling
 * flip-flop, the sample to the release the board can see, the sample to
 * the store's write. */
enum {
    MX_CGA_CHAIN = 16, MX_CGA_SAMPLE_STATE = 8, MX_CGA_SELECT_OSC = 2, MX_CGA_RELEASE_OSC = 7, MX_CGA_WRITE_OSC = 2
};
/* Snow: the CRTC address latch for the CPU's slot (RAS2 at the entry to
 * state 5) is this many periods before the sample; at most this many
 * cells of one scanline (the CPU's slots are at least 32 periods apart,
 * two chain cycles; R1 up to 255). */
enum { MX_CGA_MA_LATCH_OSC = 3, MX_CGA_SNOW_MAX = 128 };

typedef struct mx_cga_snow {
    uint8_t h;                          /* the character of the line */
    uint8_t ch, at;                     /* what the latches took */
} mx_cga_snow;

typedef struct mx_cga {
    mx_dev dev;
    mx_mc6845 crtc;
    uint8_t mode, colour;
    uint8_t lp_latch, pen_switch;
    uint8_t pen_inside;                 /* the pen is on the picture, at (pen_x, pen_y) */
    uint16_t pen_x, pen_y;
    uint8_t glyph[MX_CGA_GLYPH_BYTES];
    uint8_t vram[MX_CGA_MEM_SIZE];
    uint64_t chain_reset;               /* OSC time RESET last cleared the timing chain */
    uint64_t slot;                      /* the slot's CAS (the store write) of the cycle last asked to wait */
    int slot_valid, slot_write;
    /* snow on the line starting at snow_line (the one being scanned) */
    uint64_t snow_line;
    unsigned snow_n;
    mx_cga_snow snow[MX_CGA_SNOW_MAX];
    uint64_t snow_cells;                /* cells drawn with snow (a count for the checks) */
    int snow_on;                        /* the snow is shown (mx_cga_set_snow) */
    uint8_t dbus[2];                    /* the card's data bus D0-7 and D8-15: the last byte each carried (FFh before any) */
    /* renderer: the next scanline to draw, the frame being drawn and the
     * frame last completed */
    mx_mc6845_line line;
    int line_valid, draw_valid;
    uint64_t draw_frame, last_frame;
    uint32_t last_index;                /* the scanline drawn last */
    uint32_t rows_done;
    uint32_t front;
    uint64_t serial;
    uint8_t frames[2][MX_CGA_FRAME_W * MX_CGA_FRAME_H];
} mx_cga;

/* Fills in the adapter (power-on state: mode and colour 0, store cleared)
 * with a copy of `glyphs`, MX_CGA_GLYPH_BYTES of the Junior's character
 * generator (NULL: all blank); the struct is large, allocate it. The board
 * then calls dev.ops->reset to bring the CRTC in step. */
void mx_cga_init(mx_cga *c, const mx_hw_host *host, const char *name, const uint8_t *glyphs);
/* The pen at pixel (x, y) of the frame (MX_CGA_FRAME_W x MX_CGA_FRAME_H), on the
 * picture (`inside` 1) or not (0: no pen, no strobes). */
void mx_cga_pen_place(mx_cga *c, int x, int y, int inside);
/* The snow (above) on or off. A new adapter has it on. Off, the CPU's cycles take no slot from the picture and
 * the renderer has nothing to do for them; what is noted for the line being scanned is dropped. Applies at
 * once. */
void mx_cga_set_snow(mx_cga *c, int on);
/* Draws what is due up to the host's `now` and returns the last complete
 * frame (MX_CGA_FRAME_W x MX_CGA_FRAME_H, pitch MX_CGA_FRAME_W). *serial
 * (may be NULL) changes whenever the frame's pixels do. */
const uint8_t *mx_cga_frame(mx_cga *c, uint64_t *serial);
/* The present frame period in OSC periods (238944 in the standard modes:
 * 59.92 Hz). */
uint64_t mx_cga_frame_period(const mx_cga *c);

#endif /* MX_CGA_H */
