/* The host's game controllers (SDL's GameController API) as the setup
 * screen's input and as the list of controllers the Controllers page
 * offers: d-pad and left stick move (left and right turn the pages), A is Enter, B is Escape,
 * the shoulder buttons change the value of the selected option, Start and the Guide button open and
 * close the setup screen; on the running machine a controller assigned to a joystick of the game control
 * adapter gives its stick or d-pad and its A and B buttons.
 * MezinXT's own code. */
#ifndef MX_PADS_H
#define MX_PADS_H

#include "../core/config.h"
#include "tui.h"

#include <SDL.h>
#include <stdint.h>

typedef struct mx_pads {
    SDL_GameController *pad[MX_CONFIG_PADS];    /* in the order they were connected */
    unsigned count;
    int held;                                   /* the direction held: a mx_tui_key, MX_KEY_NONE for none */
    uint32_t next_ms;                           /* when a held direction repeats */
} mx_pads;

typedef enum mx_pads_result {
    MX_PADS_NONE,
    MX_PADS_CHANGED,            /* a controller came or went */
    MX_PADS_EVENT,              /* the event for the setup screen is set */
    MX_PADS_SETUP               /* open or close the setup screen */
} mx_pads_result;

/* Starts the controller subsystem and opens those present; 0 when SDL has none to offer. */
int mx_pads_open(mx_pads *p);
void mx_pads_close(mx_pads *p);
mx_pads_result mx_pads_event(mx_pads *p, const SDL_Event *e, mx_tui_event *out);
/* A direction held on a d-pad or stick repeats like a key: after 400 ms,
 * then every 120. 1 when `out` is set. */
int mx_pads_repeat(mx_pads *p, uint32_t now_ms, mx_tui_event *out);
const char *mx_pads_name(const mx_pads *p, unsigned index);
/* The first stick of a controller, -32768 to 32767. */
void mx_pads_axes(const mx_pads *p, unsigned index, int *x, int *y);
/* What a controller gives the game control adapter's joystick: its first stick as it
 * is (the caller calibrates it), the d-pad as -1, 0 or 1 on each axis (up and left
 * are -1) and the A and B buttons as bits 0 and 1. */
void mx_pads_joystick(const mx_pads *p, unsigned index, int *x, int *y, int *dpad_x, int *dpad_y, unsigned *buttons);

#endif /* MX_PADS_H */
