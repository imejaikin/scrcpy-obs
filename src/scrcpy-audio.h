#pragma once

#include <obs-module.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Captures audio from the Android device and feeds it to the OBS source.
 *
 * A second instance of the scrcpy server is started on the device with
 * audio only and raw PCM output (audio_codec=raw, raw_stream=true), which
 * is a plain stream of 48 kHz stereo signed 16-bit samples. It is reached
 * through `adb forward` and read from a thread; the connection is
 * re-established if it drops.
 */
typedef struct scrcpy_audio scrcpy_audio_t;

/* audio_source: a scrcpy audio source name, e.g. "mic", "mic-camcorder", "output". */
scrcpy_audio_t *scrcpy_audio_start(obs_source_t *source, const char *serial, const char *audio_source);

void scrcpy_audio_stop(scrcpy_audio_t *a);

#ifdef __cplusplus
}
#endif
