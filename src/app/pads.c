/* The host's game controllers (pads.h). MezinXT's own code. */
#include "pads.h"

#include <stdlib.h>
#include <string.h>

#define STICK_THRESHOLD 16000
#define REPEAT_FIRST_MS 400
#define REPEAT_NEXT_MS 120

static int known(const mx_pads *p, SDL_JoystickID id) {
    for (unsigned i = 0; i < p->count; ++i)
        if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p->pad[i])) == id) return 1;
    return 0;
}

static int add(mx_pads *p, int device_index) {
    if (p->count >= MX_CONFIG_PADS || !SDL_IsGameController(device_index)) return 0;
    SDL_GameController *c = SDL_GameControllerOpen(device_index);
    if (!c) return 0;
    if (known(p, SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)))) { SDL_GameControllerClose(c); return 0; }
    p->pad[p->count++] = c;
    return 1;
}

int mx_pads_open(mx_pads *p) {
    memset(p, 0, sizeof *p);
    p->held = MX_KEY_NONE;
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)) return 0;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) add(p, i);
    return 1;
}

void mx_pads_close(mx_pads *p) {
    for (unsigned i = 0; i < p->count; ++i) SDL_GameControllerClose(p->pad[i]);
    p->count = 0;
}

mx_pads_result mx_pads_event(mx_pads *p, const SDL_Event *e, mx_tui_event *out) {
    out->key = MX_KEY_NONE;
    out->ch = 0;
    if (e->type == SDL_CONTROLLERDEVICEADDED) return add(p, e->cdevice.which) ? MX_PADS_CHANGED : MX_PADS_NONE;
    if (e->type == SDL_CONTROLLERDEVICEREMOVED) {
        for (unsigned i = 0; i < p->count; ++i) {
            if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p->pad[i])) != e->cdevice.which) continue;
            SDL_GameControllerClose(p->pad[i]);
            memmove(p->pad + i, p->pad + i + 1, (p->count - i - 1) * sizeof p->pad[0]);
            --p->count;
            p->held = MX_KEY_NONE;
            return MX_PADS_CHANGED;
        }
        return MX_PADS_NONE;
    }
    if (e->type != SDL_CONTROLLERBUTTONDOWN) return MX_PADS_NONE;
    switch (e->cbutton.button) {
    case SDL_CONTROLLER_BUTTON_A: out->key = MX_KEY_ENTER; break;
    case SDL_CONTROLLER_BUTTON_B: out->key = MX_KEY_ESCAPE; break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: out->key = MX_KEY_PAGE_UP; break;       /* the value of the selected option goes back */
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: out->key = MX_KEY_PAGE_DOWN; break;    /* ... and on; the d-pad's left and right turn the pages */
    case SDL_CONTROLLER_BUTTON_START:
    case SDL_CONTROLLER_BUTTON_GUIDE: return MX_PADS_SETUP;
    default: return MX_PADS_NONE;       /* the d-pad repeats through mx_pads_repeat */
    }
    return MX_PADS_EVENT;
}

/* The direction a d-pad or stick of any controller holds, MX_KEY_NONE for none. */
static int direction(const mx_pads *p) {
    for (unsigned i = 0; i < p->count; ++i) {
        SDL_GameController *c = p->pad[i];
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP)) return MX_KEY_UP;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) return MX_KEY_DOWN;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) return MX_KEY_LEFT;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) return MX_KEY_RIGHT;
        const int x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (abs(x) >= STICK_THRESHOLD || abs(y) >= STICK_THRESHOLD) {
            if (abs(y) >= abs(x)) return y < 0 ? MX_KEY_UP : MX_KEY_DOWN;
            return x < 0 ? MX_KEY_LEFT : MX_KEY_RIGHT;
        }
    }
    return MX_KEY_NONE;
}

int mx_pads_repeat(mx_pads *p, uint32_t now_ms, mx_tui_event *out) {
    const int d = direction(p);
    out->ch = 0;
    if (d == MX_KEY_NONE) { p->held = MX_KEY_NONE; return 0; }
    if (d != p->held) {
        p->held = d;
        p->next_ms = now_ms + REPEAT_FIRST_MS;
    } else if ((int32_t)(now_ms - p->next_ms) >= 0) p->next_ms = now_ms + REPEAT_NEXT_MS;
    else return 0;
    out->key = (mx_tui_key)d;
    return 1;
}

const char *mx_pads_name(const mx_pads *p, unsigned index) {
    const char *name = index < p->count ? SDL_GameControllerName(p->pad[index]) : NULL;
    return name && *name ? name : "Game controller";
}

void mx_pads_axes(const mx_pads *p, unsigned index, int *x, int *y) {
    *x = *y = 0;
    if (index >= p->count) return;
    *x = SDL_GameControllerGetAxis(p->pad[index], SDL_CONTROLLER_AXIS_LEFTX);
    *y = SDL_GameControllerGetAxis(p->pad[index], SDL_CONTROLLER_AXIS_LEFTY);
}

void mx_pads_joystick(const mx_pads *p, unsigned index, int *x, int *y, int *dpad_x, int *dpad_y, unsigned *buttons) {
    *x = *y = *dpad_x = *dpad_y = 0;
    *buttons = 0;
    if (index >= p->count) return;
    SDL_GameController *c = p->pad[index];
    *x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    *y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    *dpad_x = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) - SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    *dpad_y = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) - SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP);
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A)) *buttons |= 1u;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B)) *buttons |= 2u;
}
