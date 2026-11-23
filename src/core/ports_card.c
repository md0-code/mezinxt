/* The serial and parallel adapter as a card of the Junior's board
 * (ports_card.h). MezinXT's own code. */
#include "ports_card.h"

#include <stdlib.h>
#include <string.h>

static unsigned port_of(unsigned channel) { return MX_PORT_SIO_A + channel; }

static void end_tx(void *ctx, unsigned channel, uint8_t data, unsigned bits) {
    mx_ports_card *c = ctx;
    (void)bits;
    if (c->have_host && c->host.transmit) c->host.transmit(c->host.ctx, port_of(channel), data);
}

static int end_rx(void *ctx, unsigned channel, uint8_t *byte) {
    mx_ports_card *c = ctx;
    if (!c->rx_count[channel]) return 0;
    *byte = c->rx[channel][c->rx_head[channel]];
    c->rx_head[channel] = (c->rx_head[channel] + 1u) % MX_PORTS_RX;
    c->rx_count[channel]--;
    return 1;
}

static void end_modem(void *ctx, unsigned channel, int rts, int dtr) {
    mx_ports_card *c = ctx;
    if (c->have_host && c->host.outputs) c->host.outputs(c->host.ctx, port_of(channel), dtr, rts);
}

static void print_byte(void *ctx, uint8_t byte) {
    mx_ports_card *c = ctx;
    if (c->have_host && c->host.transmit) c->host.transmit(c->host.ctx, MX_PORT_LPT, byte);
}

mx_ports_card *mx_ports_card_attach(mx_junior *j) {
    mx_bus *bus = mx_junior_bus(j);
    /* Both windows, both routes and both chips' places, or nothing is mapped. */
    if (bus->windows + 2u > MX_BUS_WINDOWS || bus->routes + 2u > MX_BUS_ROUTES || bus->chips + 2u > MX_BUS_CHIPS) return NULL;
    mx_ports_card *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->board = j;
    const mx_hw_host *host = mx_junior_host(j);
    mx_sio_card_init(&c->sio, host, "sio");
    c->sio.dev.ops->reset(&c->sio.dev);
    for (unsigned ch = 0; ch < 2; ++ch) {
        const mx_sio_end end = {c, end_tx, end_rx, NULL, end_modem};
        mx_sio_card_set_end(&c->sio, ch, &end);
    }
    mx_lpt_init(&c->lpt, host, "lpt");
    mx_lpt_set_sink(&c->lpt, print_byte, c);
    c->lpt.dev.ops->reset(&c->lpt.dev);
    c->lpt.dev.ops->input(&c->lpt.dev, MX_LPT_IN_SELECT, 0);        /* nothing on the connector yet */
    mx_bus_map_io(bus, &c->sio.dev, MX_SIO_CARD_IO_BASE, MX_SIO_CARD_IO_BASE + MX_SIO_CARD_IO_SIZE - 1u, 0);
    mx_bus_route(bus, &c->sio.dev, MX_SIO_CARD_IRQ, MX_ROUTE_IRQ, 4);
    mx_bus_map_io(bus, &c->lpt.dev, MX_LPT_IO_BASE, MX_LPT_IO_BASE + MX_LPT_IO_SIZE - 1u, 0);
    mx_bus_route(bus, &c->lpt.dev, MX_LPT_IRQ, MX_ROUTE_IRQ, 7);
    return c;
}

void mx_ports_card_free(mx_ports_card *card) { free(card); }

void mx_ports_card_set_host(mx_ports_card *c, const mx_serial_host *host) {
    memset(&c->host, 0, sizeof c->host);
    c->have_host = host != NULL;
    if (host) c->host = *host;
    for (unsigned ch = 0; ch < 2; ++ch) {
        c->rx_head[ch] = c->rx_count[ch] = 0;
        mx_sio_card_set_loopback(&c->sio, ch, host && host->loopback && host->loopback(host->ctx, port_of(ch)));
        mx_sio_card_set_modem(&c->sio, ch, 0, 0, 0);
    }
    c->lpt.dev.ops->input(&c->lpt.dev, MX_LPT_IN_SELECT, 0);
}

size_t mx_ports_card_receive(mx_ports_card *c, unsigned port, const uint8_t *data, size_t count) {
    if (port != MX_PORT_SIO_A && port != MX_PORT_SIO_B) return count;
    const unsigned ch = port - MX_PORT_SIO_A;
    size_t taken = 0;
    while (taken < count && c->rx_count[ch] < MX_PORTS_RX) {
        c->rx[ch][(c->rx_head[ch] + c->rx_count[ch]) % MX_PORTS_RX] = data[taken++];
        c->rx_count[ch]++;
    }
    if (taken) mx_sio_card_rx_notify(&c->sio, ch);
    return taken;
}

void mx_ports_card_modem(mx_ports_card *c, unsigned port, int dcd, int cts, int dsr, int ri) {
    (void)ri;
    if (port == MX_PORT_SIO_A || port == MX_PORT_SIO_B) mx_sio_card_set_modem(&c->sio, port - MX_PORT_SIO_A, cts != 0, dcd != 0, dsr != 0);
    else if (port == MX_PORT_LPT) c->lpt.dev.ops->input(&c->lpt.dev, MX_LPT_IN_SELECT, dsr != 0);
}
