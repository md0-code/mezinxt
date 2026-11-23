/* Host sound output: the machine's samples, pulled after each run slice,
 * converted to the output device's rate with a ratio that follows the
 * device's own clock (audiosync.h: the queue neither grows nor runs dry) and
 * queued at the volume asked for. */
#ifndef MX_HOST_AUDIO_H
#define MX_HOST_AUDIO_H

#include "../core/machine.h"
#include "audiosync.h"

#include <SDL.h>
#include <stdio.h>

typedef struct mx_host_audio {
    SDL_AudioDeviceID device;
    SDL_AudioSpec output;
    mx_audiosync sync;
    uint32_t rate;              /* the machine's rate the sync was set up for */
    int flowing;                /* samples have been queued since the last discard */
} mx_host_audio;

/* 0 when no output device opens; pumping then discards the samples. */
int mx_host_audio_open(mx_host_audio *audio);
/* Moves the machine's samples to the device at `volume` percent (0 mutes);
 * with `discard` (the machine runs faster than real time, or is not
 * running) they are taken and dropped. 0 on an SDL failure. */
int mx_host_audio_pump(mx_host_audio *audio, mx_machine *machine, unsigned volume, int discard);
/* The same while the machine runs free (faster than real time): the machine's frames are dropped, and the drive sound's
 * summary (machine.h: mx_machine_audio_free) is queued at the pace the device consumes it, so the drives are heard
 * working at the speed of the clock. 0 on an SDL failure. */
int mx_host_audio_pump_free(mx_host_audio *audio, mx_machine *machine, unsigned volume);
void mx_host_audio_close(mx_host_audio *audio);

/* The machine's sound recorded to a WAV file instead (16-bit stereo PCM at
 * the machine's rate): mx_wav_pump after each run slice. */
typedef struct mx_wav {
    FILE *file;
    uint32_t rate, frames;
    int peak;                   /* loudest sample so far */
} mx_wav;
int mx_wav_open(mx_wav *wav, const char *path);
void mx_wav_pump(mx_wav *wav, mx_machine *machine);
/* Completes the header; 0 when the file could not be written. */
int mx_wav_close(mx_wav *wav);

#endif /* MX_HOST_AUDIO_H */
