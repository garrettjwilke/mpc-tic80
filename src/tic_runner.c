#include "tic_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <libgen.h>

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

    r->samplerate = (samplerate > 0) ? samplerate : TIC80_SAMPLERATE;
    r->tic = tic80_create(r->samplerate, TIC80_PIXEL_COLOR_BGRA8888);
    if (!r->tic) {
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

int tic_runner_load_buffer(struct tic_runner *r, const void *data, int size, const char *name) {
    if (!r || !data || size <= 0) return -1;

    if (r->cart_data) {
        free(r->cart_data);
        r->cart_data = NULL;
        r->cart_size = 0;
    }

    r->cart_data = malloc(size);
    if (!r->cart_data) return -1;
    memcpy(r->cart_data, data, size);
    r->cart_size = size;

    // Check if this is a plain text lua file (no PNG header and no binary chunk header)
    bool is_png = (size >= 8 && memcmp(data, "\x89PNG", 4) == 0);
    bool is_chunked = false;

    // Check if first byte matches valid chunk type
    uint8_t first_type = ((const uint8_t*)data)[0] & 0x1F;
    if (first_type <= 20 && !is_png) {
        is_chunked = true;
    }

    if (!is_png && !is_chunked) {
        // Synthesize a valid chunked TIC buffer containing CHUNK_DEFAULT + CHUNK_CODE
        // Chunk structure: type:5, bank:3, size:16 (total 4 bytes header)
        int code_size = size;
        int synthetic_size = 4 + 4 + code_size;
        uint8_t *synth = calloc(1, synthetic_size);
        if (synth) {
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
            memcpy(synth + 8, data, code_size);

            tic80_load(r->tic, synth, synthetic_size);
            free(synth);
        } else {
            tic80_load(r->tic, r->cart_data, r->cart_size);
        }
    } else {
        tic80_load(r->tic, r->cart_data, r->cart_size);
    }

    r->is_loaded = true;
    r->is_paused = false;
    r->frame_count = 0;

    extract_cart_title(r, name);
    return 0;
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
    tic80_load(r->tic, r->cart_data, r->cart_size);
    r->frame_count = 0;
}

void tic_runner_set_paused(struct tic_runner *r, bool paused) {
    if (r) r->is_paused = paused;
}

bool tic_runner_is_paused(const struct tic_runner *r) {
    return r ? r->is_paused : false;
}

void tic_runner_tick(struct tic_runner *r, const tic80_input *input) {
    if (!r || !r->tic || !r->is_loaded || r->is_paused) return;

    tic80_input in = {0};
    if (input) in = *input;

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
    if (out_count) *out_count = r->tic->samples.count;
    return r->tic->samples.buffer;
}

void tic_runner_render(struct tic_runner *r, struct ui *u) {
    if (!r || !r->tic || !r->tic->screen || !u || !u->px) return;

    // Viewport is centered at x=40, y=24 (240x136)
    const uint32_t *src_buf = r->tic->screen;
    uint32_t *dst_buf = u->px;

    for (int y = 0; y < TIC80_HEIGHT; y++) {
        // Source row inside TIC80_FULLWIDTH (256) buffer with MARGIN_TOP (4) and MARGIN_LEFT (8)
        const uint32_t *src_row = src_buf + (TIC80_MARGIN_TOP + y) * TIC80_FULLWIDTH + TIC80_MARGIN_LEFT;
        uint32_t *dst_row = dst_buf + (TIC_RUNNER_VP_Y + y) * UI_W + TIC_RUNNER_VP_X;
        memcpy(dst_row, src_row, TIC80_WIDTH * sizeof(uint32_t));
    }
}
