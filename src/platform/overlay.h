/* Short messages: the text and how long it stays on the status line. */

#ifndef MX_OVERLAY_H
#define MX_OVERLAY_H

#include <stdint.h>

typedef struct mx_osd {
    char text[128];
    uint32_t until_ms;
} mx_osd;

/* Shows a message for a couple of seconds from now_ms. */
void mx_osd_show(mx_osd *o, uint32_t now_ms, const char *fmt, ...);

#endif /* MX_OVERLAY_H */
