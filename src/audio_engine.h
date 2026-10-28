#pragma once
#include <stdint.h>
#include <stdbool.h>

struct audio_engine;

struct audio_engine* audio_engine_create(int rate, int period_frames);
void audio_engine_destroy(struct audio_engine *ae);

bool audio_engine_start(struct audio_engine *ae);
void audio_engine_stop(struct audio_engine *ae);
bool audio_engine_is_running(const struct audio_engine *ae);

// Pushes stereo 16-bit interleaved PCM samples (count is number of int16_t words, i.e. 2 * frames)
void audio_engine_push_samples(struct audio_engine *ae, const int16_t *samples, int word_count);

void audio_engine_set_volume(struct audio_engine *ae, int cc_vol); // 0..127
int audio_engine_get_volume(const struct audio_engine *ae);

// Returns 5-bit meter masks for Left and Right channels to feed surface_meter()
void audio_engine_get_meter_masks(struct audio_engine *ae, int *left_mask, int *right_mask);
