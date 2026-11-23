/* A host joystick's position for the game control adapter (stick.h).
 * MezinXT's own code. */
#include "stick.h"
#include "../hw/gameport.h"

/* One axis, in hundredths of a percent of the travel. */
static int axis(int raw, int centre_percent, int dead_percent, int range_percent) {
    int64_t p = (int64_t)raw * 10000 / 32767;
    if (p < -10000) p = -10000;
    const int64_t d = p - (int64_t)centre_percent * 100, dead = (int64_t)dead_percent * 100, range = (int64_t)range_percent * 100;
    const int64_t a = d < 0 ? -d : d;
    if (a <= dead) return 0;
    int64_t out = range > dead ? (a - dead) * 10000 / (range - dead) : 10000;
    if (out > 10000) out = 10000;
    return d < 0 ? (int)(-out * 32768 / 10000) : (int)(out * 32767 / 10000);
}

void mx_stick_map(const mx_stick_config *c, int raw_x, int raw_y, int *x, int *y) {
    *x = axis(raw_x, c->centre_x, c->dead_zone, c->range);
    *y = axis(raw_y, c->centre_y, c->dead_zone, c->range);
}

uint32_t mx_stick_ohms(int position) {
    if (position < -32768) position = -32768;
    if (position > 32767) position = 32767;
    return (uint32_t)(((int64_t)position + 32768) * MX_GAMEPORT_MAX_OHMS / 65535);
}
