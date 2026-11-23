/* Host ends of the Junior's byte ports (ports.sio_a, ports.sio_b, ports.lpt
 * in mezinxt.yaml): a host serial device, a local pipe, a TCP connection or
 * server, an output file, or a loopback plug (which the machine wires inside
 * the channel). Bytes the guest sends are queued by the machine's transmit
 * callback and written out by mx_host_serial_pump, which also offers the
 * bytes that arrived to the machine and passes on the modem inputs; none of
 * it blocks. The OS-specific parts are the host layer's streams (src/host). */
#ifndef MX_HOST_SERIAL_H
#define MX_HOST_SERIAL_H

#include "../core/config.h"
#include "../core/machine.h"
#include "../host/host.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define MX_SERIAL_BUFFER 4096

typedef struct mx_serial_end {
    uint32_t type;              /* MX_END_NONE ... (config.h) */
    mx_host_stream *stream;     /* serial device or pipe */
    FILE *file;                 /* output */
    int dtr, rts;               /* the guest's handshake outputs */
    int dcd, cts, dsr, ri;      /* the modem inputs last passed on (-1 none yet) */
    uint8_t out[MX_SERIAL_BUFFER];
    size_t out_count;
    uint8_t in[MX_SERIAL_BUFFER];
    size_t in_count;
} mx_serial_end;

typedef struct mx_host_serial {
    mx_serial_end port[MX_PORT_COUNT];   /* by machine port (machine.h MX_PORT_*) */
} mx_host_serial;

/* Opens the configured ends; 0 with a message when one cannot open. */
int mx_host_serial_open(mx_host_serial *serial, const mx_config *config, char *error, size_t capacity);
/* The machine's callbacks for these ends (mx_machine_set_serial). */
mx_serial_host mx_host_serial_callbacks(mx_host_serial *serial);
/* Writes what the guest sent, reads what arrived and gives it to the
 * machine, updates the modem inputs. After each run slice. */
void mx_host_serial_pump(mx_host_serial *serial, mx_machine *machine);
/* The next pump passes every port's modem inputs on again (a new machine
 * has not heard them). */
void mx_host_serial_resend(mx_host_serial *serial);
void mx_host_serial_close(mx_host_serial *serial);

/* The pieces of the pump without a machine (tests): flush the queued
 * output; read what arrived into the port's input buffer. */
void mx_host_serial_flush(mx_serial_end *end);
void mx_host_serial_read(mx_serial_end *end);

#endif
