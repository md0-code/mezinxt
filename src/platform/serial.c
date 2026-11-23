/* Host ends of the byte ports (serial.h). The OS-specific calls are the host layer's streams. */
#include "serial.h"

#include <string.h>

/* The configuration's ports and the machine's. */
static const unsigned CONFIGURED[MX_CONFIG_PORTS] = {MX_PORT_SIO_A, MX_PORT_SIO_B, MX_PORT_LPT, MX_PORT_COM1, MX_PORT_COM2, MX_PORT_LPT_MDA};

static int open_end(mx_serial_end *e, const mx_port_config *c, char *error, size_t capacity) {
    memset(e, 0, sizeof *e);
    e->type = c->type;
    e->dcd = e->cts = e->dsr = e->ri = -1;
    if (c->type == MX_END_OUTPUT) {
        e->file = fopen(c->path, "ab");
        if (!e->file) snprintf(error, capacity, "Cannot open the port output %s", c->path);
        return e->file != NULL;
    }
    if (c->type == MX_END_SERIAL || c->type == MX_END_PIPE || c->type == MX_END_TCP || c->type == MX_END_TCP_LISTEN) {
        const int kind = c->type == MX_END_SERIAL ? MX_STREAM_SERIAL : c->type == MX_END_PIPE ? MX_STREAM_PIPE
                       : c->type == MX_END_TCP ? MX_STREAM_TCP_CLIENT : MX_STREAM_TCP_LISTEN;
        e->stream = mx_host_stream_open(kind, c->path, error, capacity);
        return e->stream != NULL;
    }
    return 1;      /* none, a loopback plug or the mouse: the machine wires them (mx_serial_host.loopback, .mouse) */
}

int mx_host_serial_open(mx_host_serial *s, const mx_config *config, char *error, size_t capacity) {
    memset(s, 0, sizeof *s);
    for (unsigned p = 0; p < MX_PORT_COUNT; p++) s->port[p].type = MX_END_NONE;
    for (unsigned i = 0; i < MX_CONFIG_PORTS; i++) {
        /* An 8250 port that is not on the board has nothing to connect (ports.pc_serial). */
        if ((CONFIGURED[i] == MX_PORT_COM1 && config->pc_serial < MX_PCSERIAL_ONE) ||
            (CONFIGURED[i] == MX_PORT_COM2 && config->pc_serial < MX_PCSERIAL_TWO)) continue;
        /* The monochrome coupler's printer port is on the coupler: not there, nothing to connect (machine.display). */
        if (CONFIGURED[i] == MX_PORT_LPT_MDA && config->display != MX_DISPLAY_MDA) continue;
        if (!open_end(&s->port[CONFIGURED[i]], &config->ports[i], error, capacity)) {
            mx_host_serial_close(s);
            return 0;
        }
    }
    return 1;
}

static void transmit(void *ctx, unsigned port, uint8_t byte) {
    mx_host_serial *s = ctx;
    if (port >= MX_PORT_COUNT) return;
    mx_serial_end *e = &s->port[port];
    if (e->type == MX_END_NONE || e->type == MX_END_LOOPBACK || e->type == MX_END_MOUSE) return;
    if (e->out_count == sizeof e->out) mx_host_serial_flush(e);
    if (e->out_count < sizeof e->out) e->out[e->out_count++] = byte;      /* else the line overran: lost */
}

static void outputs(void *ctx, unsigned port, int dtr, int rts) {
    mx_host_serial *s = ctx;
    if (port >= MX_PORT_COUNT) return;
    mx_serial_end *e = &s->port[port];
    e->dtr = dtr;
    e->rts = rts;
    if (e->stream) mx_host_stream_outputs(e->stream, dtr, rts);
}

/* A loopback plug is wired by the machine, inside the guest's channel. */
static int loopback(void *ctx, unsigned port) {
    const mx_host_serial *s = ctx;
    return port < MX_PORT_COUNT && s->port[port].type == MX_END_LOOPBACK;
}

/* The serial mouse is run by the machine from the host mouse: the end passes no bytes. */
static int mouse(void *ctx, unsigned port) {
    const mx_host_serial *s = ctx;
    return port < MX_PORT_COUNT && s->port[port].type == MX_END_MOUSE;
}

mx_serial_host mx_host_serial_callbacks(mx_host_serial *s) {
    mx_serial_host h = {s, transmit, outputs, loopback, mouse};
    return h;
}

void mx_host_serial_resend(mx_host_serial *s) {
    for (unsigned p = 0; p < MX_PORT_COUNT; p++) s->port[p].dcd = s->port[p].cts = s->port[p].dsr = s->port[p].ri = -1;
}

void mx_host_serial_flush(mx_serial_end *e) {
    if (!e->out_count) return;
    if (e->file) {
        fwrite(e->out, 1, e->out_count, e->file);
        fflush(e->file);
        e->out_count = 0;
        return;
    }
    if (!e->stream) { e->out_count = 0; return; }
    const long wrote = mx_host_stream_write(e->stream, e->out, e->out_count);
    if (wrote < 0) { e->out_count = 0; return; }      /* the other end went away */
    memmove(e->out, e->out + wrote, e->out_count - (size_t)wrote);
    e->out_count -= (size_t)wrote;
}

void mx_host_serial_read(mx_serial_end *e) {
    if (!e->stream) return;
    const size_t room = sizeof e->in - e->in_count;
    if (!room) return;
    const long got = mx_host_stream_read(e->stream, e->in + e->in_count, room);
    if (got > 0) e->in_count += (size_t)got;
}

/* The modem inputs: the device's lines; a pipe's are its connection; an
 * output file is always ready. */
static void modem(mx_serial_end *e, unsigned port, mx_machine *m) {
    int dcd = 1, cts = 1, dsr = 1, ri = 0;
    if (e->stream && !mx_host_stream_modem(e->stream, &dcd, &cts, &dsr, &ri)) return;
    if (dcd != e->dcd || cts != e->cts || dsr != e->dsr || ri != e->ri) {
        e->dcd = dcd;
        e->cts = cts;
        e->dsr = dsr;
        e->ri = ri;
        mx_machine_serial_modem(m, port, dcd, cts, dsr, ri);
    }
}

void mx_host_serial_pump(mx_host_serial *s, mx_machine *m) {
    for (unsigned p = 0; p < MX_PORT_COUNT; p++) {
        mx_serial_end *e = &s->port[p];
        if (e->type == MX_END_NONE || e->type == MX_END_MOUSE) continue;
        mx_host_serial_flush(e);
        mx_host_serial_read(e);
        if (e->in_count) {
            const size_t took = mx_machine_serial_receive(m, p, e->in, e->in_count);
            memmove(e->in, e->in + took, e->in_count - took);
            e->in_count -= took;
        }
        modem(e, p, m);
    }
}

void mx_host_serial_close(mx_host_serial *s) {
    for (unsigned p = 0; p < MX_PORT_COUNT; p++) {
        mx_serial_end *e = &s->port[p];
        if (e->out_count) mx_host_serial_flush(e);
        if (e->file) fclose(e->file);
        if (e->stream) mx_host_stream_close(e->stream);
        memset(e, 0, sizeof *e);
    }
}
