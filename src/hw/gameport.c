/* MezinXT - the game control adapter (gameport.h). Written from the IBM
 * Game Control Adapter's documented behaviour (the one-shot timing formula,
 * the port bits). MezinXT's own code. */
#include "gameport.h"

#include <string.h>

/* The pulse is (24200 + 11 R) ns; an OSC period is 11000/157.5 ns = 4400/63
 * ns, so the pulse is (24200 + 11 R) * 63 / 4400 periods. */
uint64_t mx_gameport_pulse(uint32_t ohms) {
    if (ohms == MX_GAMEPORT_OPEN) return MX_NEVER;
    if (ohms > MX_GAMEPORT_MAX_OHMS) ohms = MX_GAMEPORT_MAX_OHMS;
    return ((24200u + 11ull * ohms) * 63u + 2200u) / 4400u;
}

static uint32_t gp_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    mx_gameport *g = dev;
    (void)space; (void)offset; (void)size;
    uint32_t v = 0xF0u & ~((uint32_t)g->buttons << 4);
    if (g->fired) {
        const uint64_t now = mx_dev_now(&g->dev), elapsed = now - g->trigger;
        for (unsigned i = 0; i < MX_GAMEPORT_AXES; ++i)
            if (g->width[i] == MX_NEVER || elapsed < g->width[i]) v |= 1u << i;
    }
    return v;
}

static void gp_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_gameport *g = dev;
    (void)space; (void)offset; (void)size; (void)value;
    g->trigger = mx_dev_now(&g->dev);
    g->fired = 1;
}

static void gp_event(void *dev, uint64_t now) { (void)dev; (void)now; }       /* no deadlines */

static void gp_reset(void *dev) {
    mx_gameport *g = dev;
    g->fired = 0;                       /* the one-shots are idle after a reset; the joysticks stay plugged in */
}

static const mx_dev_ops GAMEPORT_OPS = {gp_read, gp_write, gp_event, gp_reset, NULL, NULL, NULL};

void mx_gameport_init(mx_gameport *g, const mx_hw_host *host, const char *name) {
    memset(g, 0, sizeof *g);
    g->dev.ops = &GAMEPORT_OPS;
    g->dev.host = host;
    g->dev.name = name ? name : "gameport";
    for (unsigned i = 0; i < MX_GAMEPORT_AXES; ++i) {
        g->ohms[i] = MX_GAMEPORT_OPEN;
        g->width[i] = MX_NEVER;
    }
}

void mx_gameport_set_axis(mx_gameport *g, unsigned axis, uint32_t ohms) {
    if (axis >= MX_GAMEPORT_AXES) return;
    if (ohms != MX_GAMEPORT_OPEN && ohms > MX_GAMEPORT_MAX_OHMS) ohms = MX_GAMEPORT_MAX_OHMS;
    g->ohms[axis] = ohms;
    g->width[axis] = mx_gameport_pulse(ohms);
}

void mx_gameport_set_buttons(mx_gameport *g, unsigned mask) { g->buttons = (uint8_t)(mask & 15u); }
