/* The analog video outputs of the Junior's CGA card, from schematic sheet 10 (COLOR GRAPHICS ADAPTOR 3/3) and the
 * card's parts list (sheet 12): the levels each monitor receives for the card's 16 RGBI colours.
 *
 * The card has three outputs:
 *   J1, nine pins: the digital R, G, B, I, HSYNC and VSYNC from an LS244. The RGBI monitor (mx_rgbi_color) converts
 *     them; nothing here.
 *   J3, J4, J5 (RCA) and J6 (sync): "RGB without intensity line". Each jack is a 2N2222 emitter follower whose
 *     base sums a colour line and the intensity line through two equal 470 ohm resistors: three levels a channel
 *     (neither line high, one, both).
 *   J2 (RCA): composite video. The four latched lines and the composite sync reach the base of one follower
 *     through 1K2, 560, 12K, 3K7 and 180 ohm: brightness only, there is no colour subcarrier on the card.
 *
 * The levels are computed from the components with the assumptions in video_out.c (typical 74LS244 output
 * levels, the transistor's base-emitter drop) and normalised: black 0, the top level 255. Indexes are the CGA's
 * RGBI indexes (bit 0 blue, 1 green, 2 red, 3 intensity). MezinXT's own. */
#ifndef MX_VIDEO_OUT_H
#define MX_VIDEO_OUT_H

#include <stdint.h>

/* J3-J5: the level (0-255) of one jack when `lines_on` (0, 1 or 2) of the two lines on its base are high. */
unsigned mx_cga_rgb_level(unsigned lines_on);
/* The colour of an RGBI index on the RGB monitor without intensity steps (0x00RRGGBB): each channel is the
 * level of its colour line and the intensity line together. Colours 7 and 8 come out alike, colour 6 is dark yellow. */
uint32_t mx_cga_rgb_color(unsigned index);
/* The grey level (0-255) of an RGBI index on the composite output J2. */
unsigned mx_cga_composite_level(unsigned index);

/* --- the composite monitor's video bandwidth ------------------------------------------------------------------
 * A monochrome composite monitor passes a limited band of the brightness signal, so neighbouring dots run into each
 * other along a scanline (vertical detail is untouched: the scanlines are separate signals). The model, per line:
 *   - the picture is 640 dots at the CGA's 14.31818 MHz dot clock (69.84 ns a dot) in every mode (the 320-wide modes
 *     are already doubled in the frame), each dot held at its composite level for its whole period;
 *   - the monitor's video amplifier is a low-pass filter with a Gaussian impulse response and one parameter, its -3 dB
 *     bandwidth B: |H(f)| = exp(-2 pi^2 s^2 f^2) with s = sqrt(ln 2) / (2 pi B), which is 1/sqrt(2) at f = B, a 10-90 %
 *     rise of 0.34 / B and no overshoot or ringing;
 *   - the displayed value of a dot is the mean of that filtered signal over the dot's period.
 * A dot's output is then a weighted sum of the dots around it, the weights those of the held dot's response sampled a
 * dot apart (the closed form is in video_out.c); they add up to 1, so a flat field is unchanged. The edge dots of a line
 * have the edge dot's own level beyond them. The RGBI and RGB monitors, and the monochrome coupler's picture, have
 * no such stage: nothing here applies to them.
 *
 * MX_COMPOSITE_BANDWIDTH_HZ is an ESTIMATE: no source documents the bandwidth of the monitor the Junior had.
 * 80-column text has to stay legible (its one-dot strokes alternate at 7.16 MHz, half the dot clock), and period
 * monochrome composite monitors specified 15 to 18 MHz with a 50 ns rise time: 8 MHz gives a 42 ns rise. */
#define MX_CGA_DOT_HZ (157500000.0 / 11.0)
#define MX_COMPOSITE_BANDWIDTH_HZ 8.0e6
/* The widest reach of the response, dots to each side (a bandwidth below about 1.5 MHz is cut to it). */
#define MX_COMPOSITE_RADIUS_MAX 8

typedef struct mx_composite_filter {
    unsigned radius;                                        /* the response reaches this many dots to each side */
    int32_t tap[2 * MX_COMPOSITE_RADIUS_MAX + 1];           /* tap[radius + m]: the weight of the dot m places away, 16.16 fixed point, the sum 65536 */
    uint8_t level[16];                                      /* the composite level of each RGBI index (mx_cga_composite_level) */
} mx_composite_filter;

/* The filter of a -3 dB bandwidth in Hz (0, or any value that gives no spread, leaves the picture as it is). */
void mx_composite_filter_design(mx_composite_filter *filter, double bandwidth_hz);
/* The filter of MX_COMPOSITE_BANDWIDTH_HZ, designed on first use. */
const mx_composite_filter *mx_composite_filter_default(void);
/* The filter that leaves every dot at its level (no bandwidth limit). */
const mx_composite_filter *mx_composite_filter_none(void);
/* One scanline of the composite monitor: `count` dots of RGBI indices as 0xFFgggggg through `filter` (a grey: all three
 * channels the filtered level). One pass, integer arithmetic. */
void mx_cga_composite_row(const mx_composite_filter *filter, const uint8_t *indices, unsigned count, uint32_t *argb);

#endif
