/* The monochrome display coupler as a card of the Junior's board: the display adapter (hw/mda.h) and the printer port
 * of the IBM card it reconstructs (hw/lpt.h at 3BCh) on the board's host and
 * bus, the picture handed to the machine interface (an MX_FRAME_MONO frame of
 * 720x350) and the printer connector to its byte port (machine.h:
 * MX_PORT_LPT_MDA). The adapter is large: it lives on the heap, owned by the
 * card.
 *
 *     display   I/O 3B0h-3BBh, memory B0000h-B7FFFh (the 4 KiB store repeats),
 *               8-bit slot
 *     printer   I/O 3BCh-3BFh, IRQ 7 (the Junior's own printer port at 378h
 *               also drives IRQ 7: the bus ORs them)
 *
 * The glyphs are the Junior's character ROM (2048 bytes, copied) turned into
 * the 8x14 font of the card (mda.h). The board's DIP switches (SW5-6) must say
 * the monochrome coupler is fitted (mx_junior_options.display), or the BIOS
 * never looks at it. A printer is connected and on line when the host says so
 * (a modem call with DSR 1); until then the port reads off line.
 *
 * MezinXT's own code. */
#ifndef MX_MDA_CARD_H
#define MX_MDA_CARD_H

#include "machine.h"
#include "../hw/junior.h"
#include "../hw/lpt.h"
#include "../hw/mda.h"

typedef struct mx_mda_card {
    mx_mda *mda;
    mx_lpt lpt;
    mx_junior *board;
    mx_serial_host host;
    int have_host;
    uint8_t font[MX_MDA_GLYPH_BYTES];
} mx_mda_card;

/* Puts the coupler on the board (windows, route, display) with the Junior's character ROM
 * `glyphs` (MX_MDA_SOURCE_BYTES). NULL when out of memory or the bus is full. */
mx_mda_card *mx_mda_card_attach(mx_junior *j, const uint8_t *glyphs);
/* Frees the card: after the board (mx_junior_destroy), whose bus and display point into it. */
void mx_mda_card_free(mx_mda_card *card);
/* The state at switching on (mode and store cleared); call before mx_junior_reset(j, 1). */
void mx_mda_card_power_on(mx_mda_card *card);
/* The frame period in OSC periods: the CRT controller's when it is programmed to a video frame (40 to
 * 100 Hz), else the standard 49.82 Hz. */
uint64_t mx_mda_card_frame_period(const mx_mda_card *card);
/* The printer connector's host end (copied; NULL: nothing connected): bytes printed go to host->transmit as
 * MX_PORT_LPT_MDA, and the port reads off line until the host reports a printer. */
void mx_mda_card_set_host(mx_mda_card *card, const mx_serial_host *host);
/* The host's modem lines for the printer port: DSR 1 is a printer connected and on line. */
void mx_mda_card_modem(mx_mda_card *card, int dsr);

#endif /* MX_MDA_CARD_H */
