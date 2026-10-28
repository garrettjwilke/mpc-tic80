#include "audio_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <fcntl.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sound/asound.h>
#endif

#define RING_BUF_SIZE 32768 // 16k stereo pairs

struct audio_engine {
    int rate;
    int period_frames;
    int buffer_frames;
    int pcm_fd;
    int card_num;

    pthread_t thread;
    pthread_mutex_t ring_mutex;
    bool running;

    int volume_cc; // 0..127
    float left_envelope;
    float right_envelope;
    int left_redline_hold;
    int right_redline_hold;

    int left_meter_mask;
    int right_meter_mask;

    int16_t ring[RING_BUF_SIZE];
    int ring_read;
    int ring_write;
};

#ifdef __linux__
static int card_by_path(const char* by_path) {
    char link[64];
    ssize_t n = readlink(by_path, link, sizeof(link) - 1);
    if (n < 0) return -1;
    link[n] = 0;
    const char* c = strstr(link, "controlC");
    return c ? atoi(c + 8) : -1;
}

static void hw_mask(struct snd_pcm_hw_params* p, int n, unsigned bit) {
    struct snd_mask* m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m->bits, 0, sizeof(m->bits));
    m->bits[bit >> 5] = 1u << (bit & 31);
}

static void hw_int(struct snd_pcm_hw_params* p, int n, unsigned v) {
    struct snd_interval* i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = i->max = v;
    i->integer = 1;
}

static int pcm_params(int fd, unsigned period, unsigned periods, int rate) {
    struct snd_pcm_hw_params hw;
    memset(&hw, 0, sizeof(hw));
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
        memset(hw.masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits, 0xff, sizeof(hw.masks[0].bits));
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++)
        hw.intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = ~0u;
    hw.rmask = ~0u;
    hw.info = ~0u;
    hw_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hw_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S32_LE);
    hw_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    hw_int(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    hw_int(&hw, SNDRV_PCM_HW_PARAM_RATE, rate);
    hw_int(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period);
    hw_int(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, period * periods);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0 || ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0)
        return -1;
    return 0;
}

static int ctl_bool(int fd, int iface, const char* name, int write, int on) {
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof(v));
    v.id.iface = iface;
    strncpy((char*)v.id.name, name, sizeof(v.id.name) - 1);
    if (!write) {
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0) return -1;
        return v.value.integer.value[0] ? 1 : 0;
    }
    v.value.integer.value[0] = on ? 1 : 0;
    return ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
}

static void speaker_on(int card) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/snd/controlC%d", card);
    int fd = open(path, O_RDWR);
    if (fd < 0) return;
    int hp = ctl_bool(fd, SNDRV_CTL_ELEM_IFACE_CARD, "Headphones Jack", 0, 0);
    int line = ctl_bool(fd, SNDRV_CTL_ELEM_IFACE_CARD, "Line Out Jack", 0, 0);
    if (hp <= 0 && line <= 0)
        ctl_bool(fd, SNDRV_CTL_ELEM_IFACE_MIXER, "Builtin Speaker Switch", 1, 1);
    close(fd);
}
#endif

static void* audio_thread_func(void *arg) {
    struct audio_engine *ae = (struct audio_engine*)arg;

#ifdef __linux__
    struct sched_param sp = {.sched_priority = 40};
    mlockall(MCL_CURRENT | MCL_FUTURE);
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
#endif

    int frames = ae->period_frames;
    int32_t *alsa_buf = calloc((size_t)frames * 2, sizeof(int32_t));
    int16_t *mix_buf = calloc((size_t)frames * 2, sizeof(int16_t));

    while (ae->running) {
        // Read available samples from ring buffer
        pthread_mutex_lock(&ae->ring_mutex);
        int available = (ae->ring_write - ae->ring_read + RING_BUF_SIZE) % RING_BUF_SIZE;
        int needed_words = frames * 2;
        int to_copy = (available < needed_words) ? available : needed_words;

        for (int i = 0; i < to_copy; i++) {
            mix_buf[i] = ae->ring[(ae->ring_read + i) % RING_BUF_SIZE];
        }
        ae->ring_read = (ae->ring_read + to_copy) % RING_BUF_SIZE;
        pthread_mutex_unlock(&ae->ring_mutex);

        // Fill remaining with silence if underrun
        if (to_copy < needed_words) {
            memset(mix_buf + to_copy, 0, (needed_words - to_copy) * sizeof(int16_t));
        }

        // Peak detection & envelope
        int max_l = 0, max_r = 0;
        float gain = ((float)ae->volume_cc / 127.0f) * ((float)ae->volume_cc / 127.0f);

        for (int i = 0; i < frames; i++) {
            int16_t sl = mix_buf[i * 2];
            int16_t sr = mix_buf[i * 2 + 1];

            if (abs(sl) > max_l) max_l = abs(sl);
            if (abs(sr) > max_r) max_r = abs(sr);

            int32_t out_l = (int32_t)(sl * gain);
            int32_t out_r = (int32_t)(sr * gain);

            alsa_buf[i * 2]     = out_l << 16;
            alsa_buf[i * 2 + 1] = out_r << 16;
        }

        // Smooth VU envelope
        float peak_l = (float)max_l / 32767.0f;
        float peak_r = (float)max_r / 32767.0f;

        if (peak_l > ae->left_envelope) ae->left_envelope = peak_l;
        else ae->left_envelope *= 0.88f;

        if (peak_r > ae->right_envelope) ae->right_envelope = peak_r;
        else ae->right_envelope *= 0.88f;

        // 5-segment LED masks
        static const float THRESH[4] = { 0.05f, 0.15f, 0.30f, 0.60f };
        int l_mask = 0, r_mask = 0;
        for (int b = 0; b < 4; b++) {
            if (ae->left_envelope >= THRESH[b]) l_mask |= (1 << b);
            if (ae->right_envelope >= THRESH[b]) r_mask |= (1 << b);
        }

        if (ae->left_envelope >= 0.90f) ae->left_redline_hold = 30;
        else if (ae->left_redline_hold > 0) ae->left_redline_hold--;
        if (ae->left_redline_hold > 0) l_mask |= (1 << 4);

        if (ae->right_envelope >= 0.90f) ae->right_redline_hold = 30;
        else if (ae->right_redline_hold > 0) ae->right_redline_hold--;
        if (ae->right_redline_hold > 0) r_mask |= (1 << 4);

        ae->left_meter_mask = l_mask;
        ae->right_meter_mask = r_mask;

#ifdef __linux__
        if (ae->pcm_fd >= 0) {
            struct snd_xferi x = {.buf = alsa_buf, .frames = (snd_pcm_uframes_t)frames};
            if (ioctl(ae->pcm_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
                ioctl(ae->pcm_fd, SNDRV_PCM_IOCTL_PREPARE);
            }
        } else {
            usleep((useconds_t)((frames * 1000000) / ae->rate));
        }
#else
        // Host simulation delay
        usleep((useconds_t)((frames * 1000000) / ae->rate));
#endif
    }

    free(alsa_buf);
    free(mix_buf);
    return NULL;
}

struct audio_engine* audio_engine_create(int rate, int period_frames) {
    struct audio_engine *ae = calloc(1, sizeof(*ae));
    if (!ae) return NULL;

    ae->rate = (rate > 0) ? rate : 44100;
    ae->period_frames = (period_frames > 0) ? period_frames : 128;
    ae->buffer_frames = ae->period_frames * 3;
    ae->pcm_fd = -1;
    ae->volume_cc = 100;
    pthread_mutex_init(&ae->ring_mutex, NULL);

#ifdef __linux__
    ae->card_num = card_by_path("/dev/snd/by-path/platform-sound");
    if (ae->card_num < 0) ae->card_num = 0;

    char path[32];
    snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0p", ae->card_num);
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL);
        if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

        if (pcm_params(fd, (unsigned)ae->period_frames, 3, ae->rate) == 0) {
            speaker_on(ae->card_num);
            ae->pcm_fd = fd;
        } else {
            close(fd);
        }
    }
#endif

    return ae;
}

void audio_engine_destroy(struct audio_engine *ae) {
    if (!ae) return;
    audio_engine_stop(ae);

#ifdef __linux__
    if (ae->pcm_fd >= 0) {
        close(ae->pcm_fd);
        ae->pcm_fd = -1;
    }
#endif

    pthread_mutex_destroy(&ae->ring_mutex);
    free(ae);
}

bool audio_engine_start(struct audio_engine *ae) {
    if (!ae || ae->running) return false;
    ae->running = true;
    if (pthread_create(&ae->thread, NULL, audio_thread_func, ae) != 0) {
        ae->running = false;
        return false;
    }
    return true;
}

void audio_engine_stop(struct audio_engine *ae) {
    if (!ae || !ae->running) return;
    ae->running = false;
    pthread_join(ae->thread, NULL);
}

bool audio_engine_is_running(const struct audio_engine *ae) {
    return ae ? ae->running : false;
}

void audio_engine_push_samples(struct audio_engine *ae, const int16_t *samples, int word_count) {
    if (!ae || !samples || word_count <= 0) return;

    pthread_mutex_lock(&ae->ring_mutex);
    for (int i = 0; i < word_count; i++) {
        ae->ring[ae->ring_write] = samples[i];
        ae->ring_write = (ae->ring_write + 1) % RING_BUF_SIZE;
        if (ae->ring_write == ae->ring_read) {
            // Drop oldest on overflow
            ae->ring_read = (ae->ring_read + 1) % RING_BUF_SIZE;
        }
    }
    pthread_mutex_unlock(&ae->ring_mutex);
}

void audio_engine_set_volume(struct audio_engine *ae, int cc_vol) {
    if (!ae) return;
    if (cc_vol < 0) cc_vol = 0;
    if (cc_vol > 127) cc_vol = 127;
    ae->volume_cc = cc_vol;
}

int audio_engine_get_volume(const struct audio_engine *ae) {
    return ae ? ae->volume_cc : 100;
}

void audio_engine_get_meter_masks(struct audio_engine *ae, int *left_mask, int *right_mask) {
    if (left_mask)  *left_mask = ae ? ae->left_meter_mask : 0;
    if (right_mask) *right_mask = ae ? ae->right_meter_mask : 0;
}
