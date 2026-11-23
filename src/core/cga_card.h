/* The CGA as a card of the Junior's board: the
 * adapter (hw/cga.h) on the board's host and bus, its picture handed to the
 * machine interface. The adapter is large: it lives on the heap, owned by the
 * card.
 *
 *     I/O 3D0h-3DFh, memory B8000h-BFFFFh (32 KiB, no mirror), a 16-bit
 *     store (a word cycle of the 8086 is one cycle) held through I/O CH RDY.
 *
 * The glyphs are the Junior's character ROM (2048 bytes, copied). The board's
 * DIP switches (SW5-6) must say a CGA is fitted (mx_junior_options.display),
 * or the BIOS never looks at it.
 *
 * MezinXT's own code. */
#ifndef MX_CGA_CARD_H
#define MX_CGA_CARD_H

#include "../hw/cga.h"
#include "../hw/junior.h"

typedef struct mx_cga_card {
    mx_cga *cga;
    mx_junior *board;
    int snow;                           /* the snow shown (hw/cga.h): kept over a power cycle */
    uint8_t glyph[MX_CGA_GLYPH_BYTES];
} mx_cga_card;

/* Puts a CGA on the board (windows, display) with `glyphs`
 * (MX_CGA_GLYPH_BYTES). NULL when out of memory or the bus is full. */
mx_cga_card *mx_cga_card_attach(mx_junior *j, const uint8_t *glyphs);
/* Frees the card: after the board (mx_junior_destroy), whose bus and display
 * point into it. */
void mx_cga_card_free(mx_cga_card *card);
/* The state at switching on (mode, colour and store cleared); call before
 * mx_junior_reset(j, 1). */
void mx_cga_card_power_on(mx_cga_card *card);
/* The CGA's snow (hw/cga.h) on or off, at once; a card starts with it on, and a power cycle keeps the setting. */
void mx_cga_card_set_snow(mx_cga_card *card, int on);
/* The light pen (hw/cga.h): the host pointer at pixel (x, y) of the frame, `inside` 0 when it is off
 * the picture, bit 0 of `buttons` the pen's switch. */
void mx_cga_card_pointer(mx_cga_card *card, int x, int y, int inside, unsigned buttons);
/* The frame period in OSC periods: the CRT controller's when it is
 * programmed to a video frame (40 to 100 Hz), else the standard 59.92 Hz
 * (a controller not yet programmed would give a period of a few clocks). */
uint64_t mx_cga_card_frame_period(const mx_cga_card *card);

#endif /* MX_CGA_CARD_H */
