#include "tic_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <libgen.h>
#include <pthread.h>

#include "cart.h"
#include "tic.h"
#include "api.h"

static u64 sys_counter(void *data) {
    (void)data;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

static u64 sys_freq(void *data) {
    (void)data;
    return 1000000000ULL;
}

static void on_trace(const char *text, u8 color) {
    (void)color;
    fprintf(stderr, "[TIC-80 TRACE] %s\n", text ? text : "");
}

static void on_error(const char *info) {
    fprintf(stderr, "[TIC-80 ERROR] %s\n", info ? info : "");
}

static void on_exit(void) {
    fprintf(stderr, "[TIC-80] cart requested exit\n");
}

struct tic_runner* tic_runner_create(int samplerate) {
    struct tic_runner *r = calloc(1, sizeof(*r));
    if (!r) return NULL;

    pthread_mutex_init(&r->sound_lock, NULL);
    r->samplerate = (samplerate > 0) ? samplerate : TIC80_SAMPLERATE;
    r->tic = tic80_create(r->samplerate, TIC80_PIXEL_COLOR_BGRA8888);
    if (!r->tic) {
        pthread_mutex_destroy(&r->sound_lock);
        free(r);
        return NULL;
    }

    r->tic->callback.trace = on_trace;
    r->tic->callback.error = on_error;
    r->tic->callback.exit = on_exit;

    snprintf(r->cart_title, sizeof(r->cart_title), "TIC-80");
    return r;
}

void tic_runner_destroy(struct tic_runner *r) {
    if (!r) return;
    if (r->tic) {
        tic80_delete(r->tic);
        r->tic = NULL;
    }
    if (r->cart_data) {
        free(r->cart_data);
        r->cart_data = NULL;
    }
    pthread_mutex_destroy(&r->sound_lock);
    free(r);
}

static void sanitize_title(char *dst, size_t dst_sz, const char *src) {
    if (!src || !dst || dst_sz == 0) return;
    while (*src && isspace((unsigned char)*src)) src++;
    size_t i = 0;
    while (*src && *src != '\r' && *src != '\n' && i < dst_sz - 1) {
        dst[i++] = *src++;
    }
    while (i > 0 && isspace((unsigned char)dst[i - 1])) {
        i--;
    }
    dst[i] = '\0';
}

static void extract_cart_title(struct tic_runner *r, const char *filename) {
    if (!r) return;

    // Default to filename without extension
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", filename ? filename : "cart");
    char *base = basename(tmp);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    snprintf(r->cart_title, sizeof(r->cart_title), "%s", base);

    // Try reading title: tag from cart code
    if (r->tic) {
        tic_mem *mem = (tic_mem*)r->tic;
        const char *code = mem->cart.code.data;
        if (code && code[0]) {
            const char *tag = strstr(code, "title:");
            if (tag) {
                tag += 6;
                sanitize_title(r->cart_title, sizeof(r->cart_title), tag);
            }
        }
    }
}

// A source file is not a cart. Chunk type lives in the low 5 bits, and most
// ASCII bytes look like a type, so walk the headers instead of checking one byte.
static bool looks_like_cart_chunks(const uint8_t *data, int size) {
    if (!data || size < 4) return false;
    int off = 0;
    while (off + 4 <= size) {
        unsigned type = data[off] & 0x1F;
        if (type > 20) return false;
        unsigned raw = (unsigned)data[off + 1] | ((unsigned)data[off + 2] << 8);
        int payload = (raw == 0 && (type == 5 || type == 19)) ? 65536 : (int)raw;
        if (off > size - 4 - payload) return false;
        off += 4 + payload;
    }
    return off == size;
}

// Keeps the exact image passed to tic80_load so reset replays that image.
static int commit_image(struct tic_runner *r, const void *image, int image_size, const char *name) {
    uint8_t *copy = malloc((size_t)image_size);
    if (!copy) return -1;
    memcpy(copy, image, (size_t)image_size);

    pthread_mutex_lock(&r->sound_lock);
    free(r->cart_data);
    r->cart_data = copy;
    r->cart_size = image_size;
    tic80_load(r->tic, r->cart_data, r->cart_size);
    r->is_loaded = true;
    r->is_paused = false;
    r->frame_count = 0;
    r->sound_pos = 0;
    r->sound_left = 0;
    pthread_mutex_unlock(&r->sound_lock);

    extract_cart_title(r, name);
    return 0;
}

int tic_runner_load_buffer(struct tic_runner *r, const void *data, int size, const char *name) {
    if (!r || !data || size <= 0) return -1;

    bool is_png = (size >= 8 && memcmp(data, "\x89PNG", 4) == 0);
    bool is_chunked = !is_png && looks_like_cart_chunks((const uint8_t *)data, size);

    if (!is_png && !is_chunked) {
        // Synthesize a valid chunked TIC buffer containing CHUNK_DEFAULT + CHUNK_CODE
        // Chunk structure: type:5, bank:3, size:16 (total 4 bytes header)
        int code_size = size;
        int synthetic_size = 4 + 4 + code_size;
        uint8_t *synth = calloc(1, (size_t)synthetic_size);
        if (!synth) return -1;

        // Chunk 1: CHUNK_DEFAULT (17)
        synth[0] = 17; // CHUNK_DEFAULT
        synth[1] = 0;
        synth[2] = 0;
        synth[3] = 0;

        // Chunk 2: CHUNK_CODE (5)
        synth[4] = 5;  // CHUNK_CODE
        synth[5] = 0;
        synth[6] = (uint8_t)(code_size & 0xFF);
        synth[7] = (uint8_t)((code_size >> 8) & 0xFF);
        memcpy(synth + 8, data, (size_t)code_size);

        int rc = commit_image(r, synth, synthetic_size, name);
        free(synth);
        return rc;
    }

    return commit_image(r, data, size, name);
}

int tic_runner_load_file(struct tic_runner *r, const char *path) {
    if (!r || !path) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "tic_runner: failed to open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > (16 * 1024 * 1024)) {
        fclose(f);
        return -1;
    }

    void *buf = malloc(size);
    if (!buf) {
        fclose(f);
        return -1;
    }

    if (fread(buf, 1, size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    snprintf(r->cart_path, sizeof(r->cart_path), "%s", path);
    int rc = tic_runner_load_buffer(r, buf, (int)size, path);
    free(buf);
    return rc;
}

void tic_runner_reset(struct tic_runner *r) {
    if (!r || !r->is_loaded || !r->cart_data || r->cart_size <= 0) return;
    pthread_mutex_lock(&r->sound_lock);
    tic80_load(r->tic, r->cart_data, r->cart_size);
    r->frame_count = 0;
    r->sound_pos = 0;
    r->sound_left = 0;
    pthread_mutex_unlock(&r->sound_lock);
}

void tic_runner_set_paused(struct tic_runner *r, bool paused) {
    if (r) r->is_paused = paused;
}

bool tic_runner_is_paused(const struct tic_runner *r) {
    return r ? r->is_paused : false;
}

// Studio copies this into RAM before every tick. Carts that say "press Z"
// call btnp(4); Z is button A only because this table says so.
static void apply_keyboard_map(tic80 *tic) {
    tic_mem *mem = (tic_mem *)tic;
    if (!mem->ram) return;

    static const u8 keys[TIC_BUTTONS] = {
        tic_key_up,
        tic_key_down,
        tic_key_left,
        tic_key_right,
        tic_key_z, // A
        tic_key_x, // B
        tic_key_a, // X
        tic_key_s, // Y
    };
    memset(mem->ram->mapping.data, 0, sizeof mem->ram->mapping.data);
    memcpy(mem->ram->mapping.data, keys, sizeof keys);
}

void tic_runner_tick(struct tic_runner *r, const tic80_input *input) {
    if (!r || !r->tic || !r->is_loaded || r->is_paused) return;

    tic80_input in = {0};
    if (input) in = *input;

    apply_keyboard_map(r->tic);
    tic80_tick(r->tic, in, sys_counter, sys_freq);
    r->frame_count++;
}

void tic_runner_sound(struct tic_runner *r) {
    if (!r || !r->tic || !r->is_loaded || r->is_paused) return;
    tic80_sound(r->tic);
}

const int16_t* tic_runner_get_samples(struct tic_runner *r, int *out_count) {
    if (!r || !r->tic) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    // count is already the number of int16 words (channels included), not frames.
    if (out_count) *out_count = r->tic->samples.count;
    return r->tic->samples.buffer;
}

void tic_runner_fill_audio(struct tic_runner *r, int16_t *dst, int frames) {
    if (!dst || frames <= 0) return;
    if (!r || !r->tic) {
        memset(dst, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }

    pthread_mutex_lock(&r->sound_lock);
    if (!r->is_loaded || !r->tic->samples.buffer) {
        memset(dst, 0, (size_t)frames * 2 * sizeof(int16_t));
        pthread_mutex_unlock(&r->sound_lock);
        return;
    }

    int filled = 0;
    while (filled < frames) {
        if (r->sound_left <= 0) {
            tic80_sound(r->tic);
            r->sound_pos = 0;
            r->sound_left = r->tic->samples.count / TIC80_SAMPLE_CHANNELS;
            if (r->sound_left <= 0) break;
        }
        int n = frames - filled;
        if (n > r->sound_left) n = r->sound_left;
        memcpy(dst + filled * 2,
               r->tic->samples.buffer + r->sound_pos * 2,
               (size_t)n * 2 * sizeof(int16_t));
        r->sound_pos += n;
        r->sound_left -= n;
        filled += n;
    }
    if (filled < frames)
        memset(dst + filled * 2, 0, (size_t)(frames - filled) * 2 * sizeof(int16_t));
    pthread_mutex_unlock(&r->sound_lock);
}

static const uint32_t *tic_screen_row(const struct tic_runner *r, int y) {
    return r->tic->screen + (TIC80_MARGIN_TOP + y) * TIC80_FULLWIDTH + TIC80_MARGIN_LEFT;
}

static void render_pixel(const struct tic_runner *r, uint32_t *dst) {
    for (int y = 0; y < TIC80_HEIGHT; y++) {
        const uint32_t *src_row = tic_screen_row(r, y);
        uint32_t *dst_row = dst + (TIC_RUNNER_VP_Y + y) * UI_W + TIC_RUNNER_VP_X;
        memcpy(dst_row, src_row, TIC80_WIDTH * sizeof(uint32_t));
    }
}

// Nearest neighbor. 240 source columns become 320, so every third column is doubled.
static void render_stretch(const struct tic_runner *r, uint32_t *dst) {
    int src_x[TIC_RUNNER_STRETCH_W];
    for (int x = 0; x < TIC_RUNNER_STRETCH_W; x++)
        src_x[x] = x * TIC80_WIDTH / TIC_RUNNER_STRETCH_W;

    for (int y = 0; y < TIC_RUNNER_STRETCH_H; y++) {
        int sy = y * TIC80_HEIGHT / TIC_RUNNER_STRETCH_H;
        const uint32_t *src_row = tic_screen_row(r, sy);
        uint32_t *dst_row = dst + (TIC_RUNNER_STRETCH_Y + y) * UI_W + TIC_RUNNER_STRETCH_X;
        for (int x = 0; x < TIC_RUNNER_STRETCH_W; x++)
            dst_row[x] = src_row[src_x[x]];
    }
}

void tic_runner_render(struct tic_runner *r, struct ui *u, bool stretch) {
    if (!r || !r->tic || !r->tic->screen || !u || !u->px) return;
    if (stretch)
        render_stretch(r, u->px);
    else
        render_pixel(r, u->px);
}
