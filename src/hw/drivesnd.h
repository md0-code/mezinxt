/* MezinXT - the sound of the disk drives, synthesised.
 *
 * The real machine's drives were audible: the diskette drive's spindle motor, the stepper clicking on every
 * track step (a recalibrate is a rapid burst, a seek a short run), the head-load solenoid's tock, and the
 * Winchester's spindle whine and the clicks and buzz of its seeks. This source makes all of it from nothing but the
 * drives' own mechanical events (hw.h: mx_drive_hook), so there is no recording of any drive in MezinXT:
 * every sound is a few damped resonances, a burst of noise and a slow oscillator, summed.
 *
 *  - A step is a click: a damped resonance near 2.2 kHz (the carriage's strike), a duller one near 560 Hz
 *    (its mass), a short ring near 4.3 kHz and a crack of noise, each step a little different (pitch and
 *    level drawn from a fixed pseudo-random sequence, so that a burst of steps is a run of clicks and not a
 *    buzz); a step outwards is a little lower, and a step against the end stop is heavier.
 *  - The head load is a softer tock (330 Hz and 1.1 kHz with a low thump), the unload an even softer one,
 *    a hold time after the last command that used the head (the controller's HUT).
 *  - The diskette motor is a hum of 120 Hz and its first harmonics, a band of low noise, a little hiss and the
 *    rotation's 5 Hz flutter; it comes up and runs down smoothly, its pitch following its speed (a 140 ms rise, a
 *    500 ms run-down).
 *  - The Winchester is modelled after measurements of recordings of a 1987 Miniscribe 8425 (3600 rpm, 20 MB, a
 *    stepper actuator); it is synthesised, no sample of them is used. Running, it is
 *    a whine, not a hum: 28 tones from 320 Hz to 3.6 kHz at their measured levels (the strongest the rotation's 28th
 *    harmonic, 1680 Hz, with the 23rd to 30th around it at their measured phases, so that the sound flutters at the 60 Hz
 *    of the rotation as the drive's does; then 773 Hz, 2836 Hz and 3117 Hz), over noise shaped by five resonant bands to
 *    the measured spectrum (the highest, near 13 kHz, the motor driver's). Started (a disk attached), the motor brings it
 *    to speed in 2.5 s along the measured curve, every tone rising with the speed and the sound growing louder, the
 *    driver's band three times as loud while it accelerates; then the stepper is energised (a 1120 Hz tone with its
 *    harmonics, louder for a moment, broken, then steady for 0.7 s) and the drive makes its own seek to track 0, as it
 *    did at power-on. Stopped (the disk detached), the motor is switched off and the drive runs down in about two
 *    seconds along the measured curves of speed and level. A seek is what the recording of a one-track seek showed:
 *    the step's click (resonances near 2.0 and 2.7 kHz), a 2.5 kHz tone of the stepper's drive for 30 ms that falls to a
 *    weak rattle, and 61 ms after the step a sharp click with resonances from 1.0 to 4.7 kHz at their measured decays,
 *    the loudest sound the drive makes. A longer seek is a run of steps first (the drive buffers the controller's step
 *    pulses and ramps its stepper from 500 to 5000 steps a second at 150000 steps/s^2, so a 615-cylinder stroke takes
 *    150 ms and a third of it 68 ms, the drive's average access), each step a soft knock through the body's resonances,
 *    which run together into a rising and falling buzz; a seek time from the coupler longer than that stretches the run.
 *    A recalibrate is the same run, ending at track 0 (hw.h: MX_DRIVE_SEEK_HOME). The Winchester has its own
 *    pseudo-random sequence, so that its sound never changes a diskette drive's.
 *  Diskette drive n sits a little to the left (even n) or right (odd) of the middle; the fixed disks are
 *  in the middle. Levels are well below the speaker's: a step peaks at about 4500 of 32768 at volume 100 (the
 *  speaker's tone is about 6000; steps that overlap and the motor reach 7000), a motor runs at an RMS of
 *  about 650, the Winchester's spindle at about 170, and its seek's click is, as in the recording, 17 times the
 *  spindle's RMS, about 2900; the default volume is 50.
 *
 * Timing. Events come in stamped with guest time (OSC periods, hw.h) from the chips, in time order (the
 * source keeps a short sorted queue), and are played at the output frame of their time: the same frame
 * clock as the speaker's (speaker.h, 176 / 52500 frames per OSC period). mx_drivesnd_mix adds the sound
 * to the speaker's frames, for the frames the speaker just produced, so the two stay in step however
 * the shell pulls. The chips do no per-sample work; a source that is disabled keeps no hook on the chips.
 *
 * Free running. A machine run at full host speed (drives.*.fast, or speed unthrottled) makes guest
 * time pass faster than the clock, and a drive's sound spread over its guest time would be gone in an
 * instant. mx_drivesnd_free instead plays a summary at the pace of the clock: the motors run as they run,
 * and the steps and seeks that happened come out as a stream of clicks at the rate of an ordinary
 * seek (6 ms a step) and the Winchester's seeks at twice their pace, one after the other with a pause between,
 * as many as happened up to a backlog of 40 steps and 8 waiting seeks, so the user hears the drive work in proportion to
 * what it does.
 *
 * Output: the speaker's format, interleaved 16-bit stereo at MX_DRIVESND_RATE. Deterministic: the same
 * events and the same frames give the same samples, whatever the size of the pulls.
 *
 * Portable C11, no C library math (the sines and decays are summed from series). MezinXT's own code. */
#ifndef MX_DRIVESND_H
#define MX_DRIVESND_H

#include "hw.h"
#include "speaker.h"

#define MX_DRIVESND_RATE MX_SPEAKER_RATE
#define MX_DRIVESND_QUEUE 256u          /* events kept between pulls (the oldest are played as a summary beyond it) */
#define MX_DRIVESND_RES 48u             /* damped resonances sounding at once */
#define MX_DRIVESND_BURSTS 16u          /* noise bursts sounding at once */
#define MX_DRIVESND_TABLE 1024u         /* the oscillator's sine table */
#define MX_DRIVESND_JOBS 8u             /* Winchester seeks waiting their turn in the free-running summary */
#define MX_DRIVESND_HARDS (MX_DRIVE_COUNT - MX_DRIVE_HARD0)
#define MX_DRIVESND_HD_TONES 28u        /* the Winchester spindle's tones */
#define MX_DRIVESND_HD_BANDS 5u         /* ... and its noise bands */
#define MX_DRIVESND_HD_BODY 5u          /* the resonances its steps ring */
#define MX_DRIVESND_HD_LOOP 16384u      /* its tones summed over ten turns of the disk, a table */

typedef struct mx_drivesnd_item {
    uint64_t t;
    uint32_t a, b;
    uint8_t drive, kind, dead;
} mx_drivesnd_item;

typedef struct mx_drivesnd_res {        /* a damped resonance: x(n) = A r^n cos(n w) as a rotating phasor */
    double x, y, c, s, r, left, right;
    uint8_t live;
} mx_drivesnd_res;

typedef struct mx_drivesnd_burst {      /* a decaying burst of low-passed noise */
    double env, decay, lp, k, left, right;
    uint8_t live, hard;                 /* hard: the noise is the Winchester's own sequence */
} mx_drivesnd_burst;

typedef struct mx_drivesnd_spin {       /* a diskette motor: its speed fraction (0 to 1) and its oscillators */
    double level, target, lp1, lp2, lp3;
    uint32_t phase[2];
} mx_drivesnd_spin;

typedef struct mx_drivesnd_seq {        /* a run of clicks: `left` more, `gap` frames apart, the next at clock `next` */
    uint32_t left, gap;
    uint64_t next;
    uint8_t in;                         /* floppy steps: towards the spindle */
} mx_drivesnd_seq;

typedef struct mx_drivesnd_hd {         /* a Winchester's spindle */
    uint8_t state;                      /* off, coming up, running, running down */
    uint8_t pon;                        /* coming up: the drive's own seek is still to come */
    unsigned cur;                       /* the hold tone's envelope: its segment */
    uint32_t t;                         /* frames since the state began */
    double speed, level, hf;            /* speed and level fractions, the motor driver's band */
    uint32_t loop, loop_inc, hold;      /* the tones' loop: its phase and increment; the hold tone's phase */
    double n1[MX_DRIVESND_HD_BANDS], n2[MX_DRIVESND_HD_BANDS];  /* the noise bands' filters */
} mx_drivesnd_hd;

typedef struct mx_drivesnd_job {        /* one Winchester seek: `steps` step pulses */
    uint32_t steps;
    double level, stretch, pace;        /* loudness; the step intervals' scale; the time after the last step runs at `pace` */
} mx_drivesnd_job;

typedef struct mx_drivesnd_buzz {       /* a Winchester's seek sound: the job running, the ones waiting (summary only) */
    mx_drivesnd_job job, q[MX_DRIVESND_JOBS];
    uint32_t qn, wait;                  /* jobs waiting, the pause before the next */
    uint8_t phase;                      /* 0 idle, 1 stepping, 2 after the last step (the tone and the click) */
    uint8_t settled;                    /* phase 2: the settling click has sounded */
    uint8_t body;                       /* the body's resonances ring */
    uint32_t step, t, tone;             /* steps made; frames since the last; the tone's phase */
    double next;                        /* frames to the next step */
    double bx[MX_DRIVESND_HD_BODY], by[MX_DRIVESND_HD_BODY];   /* the body's resonances (phasors) */
} mx_drivesnd_buzz;

typedef struct mx_drivesnd {
    int enabled;
    double gain;                        /* volume / 100 */
    uint64_t done;                      /* the next frame of the speaker's clock to be produced */
    uint64_t clock;                     /* frames synthesised so far (the time the click runs and rate limits are in) */
    uint32_t rng, rng_hd;               /* the pseudo-random sequences: the diskette drives' and the Winchester's */
    mx_drivesnd_item q[MX_DRIVESND_QUEUE];
    unsigned qh, qn;                    /* the queue's first and one past its last, sorted by time */
    uint64_t next_frame;                /* the first queued event's frame; UINT64_MAX when none */
    uint64_t hold_t[MX_DRIVE_COUNT];    /* the pending head unload's time, MX_NEVER when none */
    uint8_t loaded;                     /* bit n: drive n's head is loaded (as far as the events say) */
    uint64_t tock_next[MX_DRIVE_COUNT]; /* summary mode: the clock before which no further tock sounds */
    mx_drivesnd_spin spin[MX_DRIVE_COUNT];
    mx_drivesnd_seq seq[MX_DRIVE_COUNT];
    mx_drivesnd_hd hd[MX_DRIVESND_HARDS];
    mx_drivesnd_buzz buzz[MX_DRIVESND_HARDS];
    uint8_t spin_mask, seq_mask, buzz_mask;     /* the drives whose spindle, click run or seek sound is active (bit n: drive n) */
    mx_drivesnd_res res[MX_DRIVESND_RES];
    mx_drivesnd_burst burst[MX_DRIVESND_BURSTS];
    unsigned res_next, burst_next, nres, nburst;
    double atk, rel;                    /* the diskette motor's smoothing per sample */
    double nb[MX_DRIVESND_HD_BANDS][3]; /* the Winchester's noise bands: gain, a1, a2 */
    double bc[MX_DRIVESND_HD_BODY], bs[MX_DRIVESND_HD_BODY], br[MX_DRIVESND_HD_BODY];   /* its body's resonances: rotation, decay */
    double table[MX_DRIVESND_TABLE + 1u];
    float hd_loop[MX_DRIVESND_HD_LOOP + 1u];
} mx_drivesnd;

void mx_drivesnd_init(mx_drivesnd *s);
/* Switches the sound on or off and sets its volume (0 to 100 percent; 0 is off). A change of the state
 * clears everything that sounds or waits. Chips need a hook only while it is enabled. */
void mx_drivesnd_set(mx_drivesnd *s, int enabled, unsigned volume);
static inline int mx_drivesnd_enabled(const mx_drivesnd *s) { return s->enabled; }
/* The hook for the chips (hw.h: mx_drive_hook); `ctx` is the source. Ignored while disabled. */
void mx_drivesnd_event(void *ctx, unsigned drive, unsigned event, uint64_t t, uint32_t a, uint32_t b);
/* Adds the sound of `frames` frames to the stereo samples there, which are the speaker's frames from index
 * `first` of its frame clock on (speaker.h). Frames before the source's position are not made again (the
 * source goes on from where it is), a gap is skipped. */
void mx_drivesnd_mix(mx_drivesnd *s, uint64_t first, int16_t *stereo, size_t frames);
/* Free running: writes `frames` frames of the summary at the pace of the clock, after taking in every event
 * up to guest time `until`; the source's frame clock moves to `until`. */
size_t mx_drivesnd_free(mx_drivesnd *s, uint64_t until, int16_t *stereo, size_t frames);
/* Something sounds or is waiting to (tests, and the idle shortcut). */
int mx_drivesnd_busy(const mx_drivesnd *s);

#endif /* MX_DRIVESND_H */
