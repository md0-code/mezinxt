/* A host joystick's position as the game control adapter sees it (phase 6 and 7a): the calibration of a game controller's stick
 * (controllers.joystickN: dead_zone, range, centre_x, centre_y) and the
 * resistance of the joystick's potentiometer for a position. No SDL: the
 * shell reads the host's axes and calls these.
 *
 * Positions run from -32768 (left, up) to 32767 (right, down), as
 * mx_machine_joystick has them; the stick's rest is 0. MezinXT's own code. */
#ifndef MX_STICK_H
#define MX_STICK_H

#include "config.h"

#include <stdint.h>

/* The raw position of a controller's stick (-32768 to 32767 on each axis) through
 * the calibration of `config` into the position handed to the machine: the
 * centre taken as rest, a dead zone around it that counts as rest, then a
 * straight rise to full deflection at `range` percent of the travel. */
void mx_stick_map(const mx_stick_config *config, int raw_x, int raw_y, int *x, int *y);

/* The pot's resistance for a position: 0 ohms at -32768, 100 kilohms at 32767,
 * half way (about 50 kilohms) at rest. */
uint32_t mx_stick_ohms(int position);

#endif /* MX_STICK_H */
