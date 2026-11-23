/* The PC/XT asynchronous adapter as cards of the Junior's board
 * (pcserial_card.h). MezinXT's own code. */
#include "pcserial_card.h"

#include <stdlib.h>
#include <string.h>

static unsigned port_of(unsigned unit) { return MX_PORT_COM1 + unit; }

static void end_tx(void *ctx, unsigned unit, uint8_t data, unsigned bits) {
    mx_pcserial_card *c = ctx;
    (void)bits;
    if (c->have_host && c->host.transmit) c->host.transmit(c->host.ctx, port_of(unit), data);
}

static int end_rx(void *ctx, unsigned unit, uint8_t *byte) {
    mx_pcserial_card *c = ctx;
    if (!c->rx_count[unit]) return 0;
    *byte = c->rx[unit][c->rx_head[unit]];
    c->rx_head[unit] = (c->rx_head[unit] + 1u) % MX_PCSERIAL_RX;
    c->rx_count[unit]--;
    return 1;
}

static void end_modem(void *ctx, unsigned unit, int rts, int dtr) {
    mx_pcserial_card *c = ctx;
    if (c->have_host && c->host.outputs) c->host.outputs(c->host.ctx, port_of(unit), dtr, rts);
}

mx_pcserial_card *mx_pcserial_card_attach(mx_junior *j, unsigned count) {
    mx_bus *bus = mx_junior_bus(j);
    if (count < 1 || count > 2) return NULL;
    /* All windows, all routes and all chips' places, or nothing is mapped. */
    if (bus->windows + count > MX_BUS_WINDOWS || bus->routes + count > MX_BUS_ROUTES || bus->chips + count > MX_BUS_CHIPS) return NULL;
    mx_pcserial_card *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->count = count;
    c->mouse_unit = -1;
    mx_sermouse_init(&c->mouse);
    const mx_hw_host *host = mx_junior_host(j);
    static const uint32_t BASE[2] = {MX_NS8250_COM1_BASE, MX_NS8250_COM2_BASE};
    static const int IRQ[2] = {4, 3};
    for (unsigned u = 0; u < count; ++u) {
        mx_ns8250_init(&c->uart[u], host, u ? "com2" : "com1", u);
        c->uart[u].dev.ops->reset(&c->uart[u].dev);
        mx_bus_map_io(bus, &c->uart[u].dev, BASE[u], BASE[u] + MX_NS8250_IO_SIZE - 1u, 0);
        mx_bus_route(bus, &c->uart[u].dev, MX_NS8250_IRQ, MX_ROUTE_IRQ, IRQ[u]);
    }
    return c;
}

void mx_pcserial_card_free(mx_pcserial_card *card) { free(card); }

void mx_pcserial_card_set_host(mx_pcserial_card *c, const mx_serial_host *host) {
    memset(&c->host, 0, sizeof c->host);
    c->have_host = host != NULL;
    if (host) c->host = *host;
    c->mouse_unit = -1;
    mx_sermouse_init(&c->mouse);
    for (unsigned u = 0; u < c->count; ++u) {
        mx_ns8250 *uart = &c->uart[u];
        c->rx_head[u] = c->rx_count[u] = 0;
        const unsigned port = port_of(u);
        const int mouse = host && host->mouse && host->mouse(host->ctx, port);
        mx_ns8250_set_plug(uart, host && host->loopback && host->loopback(host->ctx, port));
        mx_ns8250_set_modem(uart, 0, 0, 0, 0);
        if (mouse) {
            c->mouse_unit = (int)u;
            mx_sermouse_attach(&c->mouse, uart);
            mx_sermouse_modem(&c->mouse, uart->o_dtr, uart->o_rts);     /* the lines may be up already */
        } else {
            mx_ns8250_end end;
            memset(&end, 0, sizeof end);
            if (host) {
                end.ctx = c;
                end.tx = end_tx;
                end.rx = end_rx;
                end.modem = end_modem;
            }
            mx_ns8250_set_end(uart, host ? &end : NULL);
        }
    }
}

size_t mx_pcserial_card_receive(mx_pcserial_card *c, unsigned port, const uint8_t *data, size_t count) {
    if (port < MX_PORT_COM1 || port >= MX_PORT_COM1 + c->count) return count;
    const unsigned u = port - MX_PORT_COM1;
    if (c->mouse_unit == (int)u) return count;
    size_t taken = 0;
    while (taken < count && c->rx_count[u] < MX_PCSERIAL_RX) {
        c->rx[u][(c->rx_head[u] + c->rx_count[u]) % MX_PCSERIAL_RX] = data[taken++];
        c->rx_count[u]++;
    }
    if (taken) mx_ns8250_rx_notify(&c->uart[u]);
    return taken;
}

void mx_pcserial_card_modem(mx_pcserial_card *c, unsigned port, int dcd, int cts, int dsr, int ri) {
    if (port < MX_PORT_COM1 || port >= MX_PORT_COM1 + c->count) return;
    const unsigned u = port - MX_PORT_COM1;
    if (c->mouse_unit == (int)u) return;                /* a mouse has no modem lines to answer with */
    mx_ns8250_set_modem(&c->uart[u], cts != 0, dsr != 0, ri != 0, dcd != 0);
}

void mx_pcserial_card_mouse(mx_pcserial_card *c, int dx, int dy, unsigned buttons) {
    if (c->mouse_unit >= 0) mx_sermouse_motion(&c->mouse, dx, dy, buttons);
}
