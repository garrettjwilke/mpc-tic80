#pragma once
#include <stdint.h>
#include <stdbool.h>

struct audio_engine;

// Fills interleaved stereo int16 PCM. `frames` is the number of stereo frames.
// Called on the audio thread, never across a Lua tick.
typedef void (*audio_fill_fn)(void *ctx, int16_t *dst, int frames);

struct audio_engine* audio_engine_create(int rate, int period_frames);
void audio_engine_destroy(struct audio_engine *ae);

bool audio_engine_start(struct audio_engine *ae);
void audio_engine_stop(struct audio_engine *ae);
bool audio_engine_is_running(const struct audio_engine *ae);

void audio_engine_set_source(struct audio_engine *ae, audio_fill_fn fn, void *ctx);

// When inactive, the thread writes silence and does not call the source.
void audio_engine_set_active(struct audio_engine *ae, bool active);

void audio_engine_set_volume(struct audio_engine *ae, int cc_vol); // 0..127
int audio_engine_get_volume(const struct audio_engine *ae);

// Returns 5-bit meter masks for Left and Right channels to feed surface_meter()
void audio_engine_get_meter_masks(struct audio_engine *ae, int *left_mask, int *right_mask);
