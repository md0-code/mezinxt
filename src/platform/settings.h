/* The presenter's settings.
 *
 * MezinXT reads its settings from mezinxt.yaml (src/core/config.c); the
 * shell fills this structure from the `video` section and hands it to the
 * presenter, which needs nothing else. */

#ifndef MX_SETTINGS_H
#define MX_SETTINGS_H

#include <stdbool.h>

#define MX_PATH_MAX 1024

typedef enum mx_filter { MX_FILTER_NEAREST, MX_FILTER_LINEAR, MX_FILTER_SHARP } mx_filter;
typedef enum mx_aspect { MX_ASPECT_ORIGINAL, MX_ASPECT_SQUARE } mx_aspect;

typedef struct mx_settings {
    bool fullscreen;            /* borderless, on the desktop */
    int window_scale;           /* the window at start: the picture times this */
    int integer_scale;          /* 0: fill the window; N: whole multiples up to N */
    int aspect;                 /* mx_aspect */
    int filter;                 /* mx_filter */
    bool crt;
    bool vsync;
} mx_settings;

#endif /* MX_SETTINGS_H */
