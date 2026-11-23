/* MezinXT - the PC speaker.
 *
 * The XT's speaker is driven by a gate: the 8253's channel 2 output (OUT2)
 * ANDed with port B bit 1 of the 8255 (SPKR DATA). Port B bit 0 gates the
 * timer's channel 2 (GATE2), which is the 8253's input, not the speaker's:
 * with the timer gated off its OUT2 stays high in mode 3, and bit 1 then
 * drives the cone by itself (the "direct" speaker of sampled sound). The
 * board routes OUT2 to MX_SPEAKER_TIMER and port B bit 1 to MX_SPEAKER_DATA.
 *
 * The chip has no registers and no deadline. Every change of the drive
 * (pin changes that change timer AND data) is stamped with the host's
 * now() and becomes a band-limited step in the output: a windowed-sinc step
 * response (Blackman window, 32 taps, -6 dB at 17 kHz at 48 kHz) summed at
 * the edge's exact position between two samples, so a 2 kHz square wave
 * and a 600 kHz timer carrier alike make no aliasing: whatever is above the
 * filter is simply gone. The output is the cone's motion: the signal is
 * 0 or MX_SPEAKER_AMPLITUDE high-passed at about 30 Hz (a speaker ignores
 * DC), so a held level decays to silence and each change clicks as it does
 * on the real machine.
 *
 * Output format = machine.h's sound: interleaved stereo 16-bit frames (both
 * channels equal) at MX_SPEAKER_RATE, exactly derived from guest time:
 * 176 / 52500 frames per OSC period. Frame n is the signal at OSC time
 * n * 52500 / 176. The board's mx_machine_audio_pull calls mx_speaker_render
 * with the machine's time.
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_SPEAKER_H
#define MX_SPEAKER_H

#include "hw.h"

#include <stddef.h>

/* Input pins (ops->input): the 8253's OUT2 and 8255 port B bit 1. */
enum { MX_SPEAKER_TIMER, MX_SPEAKER_DATA };

#define MX_SPEAKER_RATE 48000u
/* Output frames per OSC period, as a ratio (= RATE / (OSC_NUM / OSC_DEN)). */
#define MX_SPEAKER_NUM 176u
#define MX_SPEAKER_DEN 52500u
_Static_assert((unsigned long long)MX_SPEAKER_RATE * MX_OSC_DEN * MX_SPEAKER_DEN ==
               (unsigned long long)MX_SPEAKER_NUM * MX_OSC_NUM, "speaker sample ratio");

/* The band-limiting kernel reaches this many frames either side of an edge,
 * so a frame is final (and rendered) only this many frames after the last
 * time the board has told the speaker about: output lags guest time by this
 * (0.33 ms). */
#define MX_SPEAKER_LATENCY 16u
/* The high level of the drive, in output units (a symmetrical square wave
 * comes out at about half of it either way, as the stub's beep does). */
#define MX_SPEAKER_AMPLITUDE 12000

#define MX_SPEAKER_RING 8192u               /* frames kept between the shell's pulls (power of two) */
#define MX_SPEAKER_STEP_RES 64u             /* step table entries per frame */
#define MX_SPEAKER_TABLE (2u * MX_SPEAKER_LATENCY * MX_SPEAKER_STEP_RES + 1u)

typedef struct mx_speaker {
    mx_dev dev;
    uint8_t timer, data, level;     /* the input pins and the drive (timer AND data) */
    uint64_t done;                  /* the next frame to be produced */
    int32_t naive;                  /* the unfiltered level (0 or 1) at frame `done` */
    double hp_x, hp_y;              /* high-pass state: the last input and output */
    int16_t step[MX_SPEAKER_RING];  /* net level change that takes effect at a frame (the unfiltered square wave) */
    double acc[MX_SPEAKER_RING];    /* band-limiting corrections summed for a frame, in levels */
    double table[MX_SPEAKER_TABLE]; /* the band-limited step response over 2 * LATENCY frames */
} mx_speaker;

/* The index of the next frame mx_speaker_render will produce on the frame clock above (what the drive sound,
 * drivesnd.h, follows so that its frames are the speaker's). */
static inline uint64_t mx_speaker_position(const mx_speaker *s) { return s->done; }
/* Sets the speaker up on `host` and resets it. */
void mx_speaker_init(mx_speaker *s, const mx_hw_host *host);
/* Produces up to `frames` stereo frames, in order, from where the last call
 * stopped: only frames final at guest time `until` (OSC periods; the board's
 * current time), so fewer than asked when the machine has not run that far.
 * Frames the shell never took are dropped: silence beyond one second, and
 * sound beyond the last MX_SPEAKER_RING frames (0.17 s) when new edges
 * arrive, so the shell must pull at least that often (a frame loop does).
 * Returns the number of frames written (2 int16_t each). */
size_t mx_speaker_render(mx_speaker *s, uint64_t until, int16_t *stereo, size_t frames);

#endif /* MX_SPEAKER_H */
