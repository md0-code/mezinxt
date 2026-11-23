/* The monochrome display coupler as a card of the Junior's board
 * (mda_card.h). MezinXT's own code. */
#include "mda_card.h"

#include <stdlib.h>
#include <string.h>

/* The standard frame: 98 characters of 9 dots by 370 lines of the card's own crystal. */
#define STANDARD_PERIOD 287421u
#define MIN_PERIOD (MX_OSC_NUM / MX_OSC_DEN / 100u)     /* 100 Hz */
#define MAX_PERIOD (MX_OSC_NUM / MX_OSC_DEN / 40u)      /* 40 Hz */

static int card_frame(void *ctx, mx_frame *f) {
    mx_mda_card *card = ctx;
    uint64_t serial = 0;
    f->pixels = mx_mda_frame(card->mda, &serial);
    f->width = MX_MDA_FRAME_W;
    f->height = MX_MDA_FRAME_H;
    f->pitch = MX_MDA_FRAME_W;
    f->aspect_w = 4;
    f->aspect_h = 3;
    f->serial = serial;
    f->format = MX_FRAME_MONO;
    return 1;
}

static void print_byte(void *ctx, uint8_t byte) {
    mx_mda_card *c = ctx;
    if (c->have_host && c->host.transmit) c->host.transmit(c->host.ctx, MX_PORT_LPT_MDA, byte);
}

static void card_init(mx_mda_card *card) {
    mx_mda_init(card->mda, mx_junior_host(card->board), "mda", card->font);
    card->mda->dev.ops->reset(&card->mda->dev);
}

mx_mda_card *mx_mda_card_attach(mx_junior *j, const uint8_t *glyphs) {
    mx_mda_card *card = calloc(1, sizeof *card);
    if (card) card->mda = malloc(sizeof *card->mda);
    if (!card || !card->mda) {
        free(card);
        return NULL;
    }
    mx_bus *bus = mx_junior_bus(j);
    /* The three windows, the route and both chips' places, or nothing is mapped. */
    if (bus->windows + 3u > MX_BUS_WINDOWS || bus->routes + 1u > MX_BUS_ROUTES || bus->chips + 2u > MX_BUS_CHIPS) {
        mx_mda_card_free(card);
        return NULL;
    }
    card->board = j;
    mx_mda_font_build(card->font, glyphs);
    card_init(card);
    const mx_hw_host *host = mx_junior_host(j);
    mx_lpt_init(&card->lpt, host, "lpt-mda");
    mx_lpt_set_sink(&card->lpt, print_byte, card);
    card->lpt.dev.ops->reset(&card->lpt.dev);
    card->lpt.dev.ops->input(&card->lpt.dev, MX_LPT_IN_SELECT, 0);      /* nothing on the connector yet */
    mx_bus_map_io(bus, &card->mda->dev, MX_MDA_IO_BASE, MX_MDA_IO_BASE + MX_MDA_IO_SIZE - 1u, 0);
    mx_bus_map_mem(bus, &card->mda->dev, MX_MDA_MEM_BASE, MX_MDA_MEM_BASE + MX_MDA_MEM_WINDOW - 1u, 0);
    mx_bus_map_io(bus, &card->lpt.dev, MX_MDA_PRINTER_BASE, MX_MDA_PRINTER_BASE + MX_LPT_IO_SIZE - 1u, 0);
    mx_bus_route(bus, &card->lpt.dev, MX_LPT_IRQ, MX_ROUTE_IRQ, 7);
    const mx_junior_display display = {card, card_frame};
    mx_junior_set_display(j, &display);
    return card;
}

void mx_mda_card_free(mx_mda_card *card) {
    if (!card) return;
    free(card->mda);
    free(card);
}

void mx_mda_card_power_on(mx_mda_card *card) { card_init(card); }

uint64_t mx_mda_card_frame_period(const mx_mda_card *card) {
    const uint64_t p = mx_mda_frame_period(card->mda);
    return p >= MIN_PERIOD && p <= MAX_PERIOD ? p : STANDARD_PERIOD;
}

void mx_mda_card_set_host(mx_mda_card *card, const mx_serial_host *host) {
    memset(&card->host, 0, sizeof card->host);
    card->have_host = host != NULL;
    if (host) card->host = *host;
    card->lpt.dev.ops->input(&card->lpt.dev, MX_LPT_IN_SELECT, 0);
}

void mx_mda_card_modem(mx_mda_card *card, int dsr) { card->lpt.dev.ops->input(&card->lpt.dev, MX_LPT_IN_SELECT, dsr != 0); }
