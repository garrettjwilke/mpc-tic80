#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

#include "tic80.h"
#include "ui.h"

#define TIC_RUNNER_VP_X  40
#define TIC_RUNNER_VP_Y  24
#define TIC_RUNNER_VP_W  TIC80_WIDTH   // 240
#define TIC_RUNNER_VP_H  TIC80_HEIGHT  // 136

struct tic_runner {
    tic80 *tic;
    char cart_path[512];
    char cart_title[64];
    bool is_loaded;
    bool is_paused;
    uint8_t *cart_data;
    int cart_size;
    int samplerate;
    int fps;
    uint64_t frame_count;

    // Serializes tic80_sound / tic80_load with the audio thread.
    // tic80_tick stays outside this lock; TIC-80's sound ring covers that split.
    pthread_mutex_t sound_lock;
    int sound_pos;  // stereo frames already consumed from samples.buffer
    int sound_left; // stereo frames still waiting in samples.buffer
};

struct tic_runner* tic_runner_create(int samplerate);
void tic_runner_destroy(struct tic_runner *r);

int tic_runner_load_file(struct tic_runner *r, const char *path);
int tic_runner_load_buffer(struct tic_runner *r, const void *data, int size, const char *name);
void tic_runner_reset(struct tic_runner *r);
void tic_runner_set_paused(struct tic_runner *r, bool paused);
bool tic_runner_is_paused(const struct tic_runner *r);

void tic_runner_tick(struct tic_runner *r, const tic80_input *input);
void tic_runner_sound(struct tic_runner *r);
const int16_t* tic_runner_get_samples(struct tic_runner *r, int *out_count);

// Pulls `frames` stereo frames of PCM. Called on the audio thread.
void tic_runner_fill_audio(struct tic_runner *r, int16_t *dst, int frames);

void tic_runner_render(struct tic_runner *r, struct ui *u);
