/* The CGA as a card of the Junior's board (cga_card.h). MezinXT's own code. */
#include "cga_card.h"
#include "machine.h"

#include <stdlib.h>
#include <string.h>

/* The standard frame: 912 dots by 262 lines of the crystal (59.92 Hz). */
#define STANDARD_PERIOD (912u * 262u)
#define MIN_PERIOD (MX_OSC_NUM / MX_OSC_DEN / 100u)     /* 100 Hz */
#define MAX_PERIOD (MX_OSC_NUM / MX_OSC_DEN / 40u)      /* 40 Hz */

static int card_frame(void *ctx, mx_frame *f) {
    mx_cga_card *card = ctx;
    uint64_t serial = 0;
    f->pixels = mx_cga_frame(card->cga, &serial);
    f->width = MX_CGA_FRAME_W;
    f->height = MX_CGA_FRAME_H;
    f->pitch = MX_CGA_FRAME_W;
    f->aspect_w = 4;
    f->aspect_h = 3;
    f->serial = serial;
    f->format = MX_FRAME_RGBI;
    return 1;
}

static void card_init(mx_cga_card *card) {
    mx_cga_init(card->cga, mx_junior_host(card->board), "cga", card->glyph);
    mx_cga_set_snow(card->cga, card->snow);
    card->cga->dev.ops->reset(&card->cga->dev);
}

mx_cga_card *mx_cga_card_attach(mx_junior *j, const uint8_t *glyphs) {
    mx_cga_card *card = calloc(1, sizeof *card);
    if (card) card->cga = malloc(sizeof *card->cga);
    if (!card || !card->cga) {
        free(card);
        return NULL;
    }
    mx_bus *bus = mx_junior_bus(j);
    /* Both windows and the chip's place, or nothing is mapped. */
    if (bus->windows + 2u > MX_BUS_WINDOWS || bus->chips >= MX_BUS_CHIPS) {
        mx_cga_card_free(card);
        return NULL;
    }
    card->board = j;
    card->snow = 1;
    if (glyphs) memcpy(card->glyph, glyphs, sizeof card->glyph);
    card_init(card);
    mx_bus_map_io(bus, &card->cga->dev, MX_CGA_IO_BASE, MX_CGA_IO_BASE + MX_CGA_IO_SIZE - 1u, 0);
    /* A 16-bit store (BHE, DGL/DGH) that holds the CPU through I/O CH RDY. */
    mx_bus_map_mem(bus, &card->cga->dev, MX_CGA_MEM_BASE, MX_CGA_MEM_BASE + MX_CGA_MEM_SIZE - 1u, MX_BUS_WORD | MX_BUS_IOCHRDY);
    const mx_junior_display display = {card, card_frame};
    mx_junior_set_display(j, &display);
    return card;
}

void mx_cga_card_free(mx_cga_card *card) {
    if (!card) return;
    free(card->cga);
    free(card);
}

void mx_cga_card_power_on(mx_cga_card *card) { card_init(card); }

void mx_cga_card_set_snow(mx_cga_card *card, int on) {
    card->snow = on != 0;
    mx_cga_set_snow(card->cga, card->snow);
}

void mx_cga_card_pointer(mx_cga_card *card, int x, int y, int inside, unsigned buttons) {
    mx_cga_pen_place(card->cga, x, y, inside);
    card->cga->dev.ops->input(&card->cga->dev, MX_CGA_PIN_PEN_SWITCH, inside && (buttons & 1u));
}

uint64_t mx_cga_card_frame_period(const mx_cga_card *card) {
    const uint64_t p = mx_cga_frame_period(card->cga);
    return p >= MIN_PERIOD && p <= MAX_PERIOD ? p : STANDARD_PERIOD;
}
