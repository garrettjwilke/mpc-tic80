#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "input.h"
#include "ui.h"

#include "tic_runner.h"
#include "input_map.h"
#include "audio_engine.h"
#include "ui_overlay.h"
#include "config.h"
#include "savestate.h"

#define TICK_NS (1000000000LL / 60)

struct app_state {
    struct tic_runner *runner;
    struct input_map map;
    struct ui_overlay overlay;
    struct audio_engine *audio;
    bool audio_active;

    int64_t next_tick_ns;
    int encoder_accum;
    int pad_latched[16];
    int pad_latch_vel[16];

    int shown_mode;
    int shown_bank;
    int shown_toast;
    int shown_stretch;
    char shown_title[64];

    struct savestate *saves;
    unsigned pad_swallow;
    unsigned pad_grandfather;
    int64_t rec_flash_until_ms;

    // A note-on with no note-off leaves a pad or Minus/Plus held. The UI only
    // acts on the press edge, so a menu still works. A cart reads the level
    // every tick, so that hold walks the player until the control is tapped.
    unsigned pad_settle;
    unsigned button_settle;
    int settle_left;
    int was_playing;
};

static int64_t mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + (int64_t)t.tv_nsec;
}

static void app_audio_fill(void *ctx, int16_t *dst, int frames) {
    struct app_state *app = (struct app_state *)ctx;
    tic_runner_fill_audio(app->runner, dst, frames);
}

static void audio_set_active(struct app_state *app, bool active) {
    if (!app->audio || app->audio_active == active) return;
    audio_engine_set_active(app->audio, active);
    app->audio_active = active;
}

static void clear_game_edges(struct app_state *app) {
    app->encoder_accum = 0;
    app->next_tick_ns = 0;
    memset(app->pad_latched, 0, sizeof app->pad_latched);
}

static int ticks_due(int64_t now, int64_t *next) {
    if (*next <= 0) {
        *next = now + TICK_NS;
        return 1;
    }
    int n = 0;
    while (now >= *next && n < 2) {
        *next += TICK_NS;
        n++;
    }
    if (now >= *next)
        *next = now + TICK_NS;
    return n;
}

static void latch_game_input(struct app_state *app, const struct input *in) {
    app->encoder_accum += in->encoder;
    for (int p = 0; p < 16; p++) {
        if (in->pad_hit[p] >= 0) {
            app->pad_latched[p] = 1;
            app->pad_latch_vel[p] = in->pad_hit[p];
        }
    }
}

static void tick_game(struct app_state *app, const struct input *in, int n) {
    for (int i = 0; i < n; i++) {
        struct input tick_in = *in;
        if (i == 0) {
            tick_in.encoder = app->encoder_accum;
            for (int p = 0; p < 16; p++) {
                if (app->pad_latched[p] && !tick_in.pad_down[p]) {
                    tick_in.pad_down[p] = 1;
                    if (tick_in.pad_vel[p] <= 0)
                        tick_in.pad_vel[p] = app->pad_latch_vel[p];
                }
            }
        } else {
            tick_in.encoder = 0;
        }
        tic80_input tic_in;
        input_map_translate(&app->map, &tick_in, &tic_in);
        tic_runner_tick(app->runner, &tic_in);
    }
    app->encoder_accum = 0;
    memset(app->pad_latched, 0, sizeof app->pad_latched);
}

static const char *cart_title(const struct app_state *app) {
    if (app->runner && app->runner->cart_title[0]) return app->runner->cart_title;
    return "";
}

static int toast_on(const struct app_state *app, int64_t now_ms) {
    return app->overlay.toast_msg[0] && now_ms < app->overlay.toast_until_ms;
}

static unsigned chrome_mask(const struct app_state *app, int64_t now_ms) {
    int bank = input_map_get_bank(&app->map);
    int toast = toast_on(app, now_ms);
    int stretch = app->overlay.stretch ? 1 : 0;
    if (app->shown_mode != (int)UI_MODE_GAMEPLAY
        || app->shown_bank != bank
        || app->shown_toast != toast
        || app->shown_stretch != stretch
        || strcmp(app->shown_title, cart_title(app)) != 0) {
        unsigned mask = UI_OVERLAY_CHROME;
        // The toast sits on the stretched picture. Repaint that picture when
        // the toast comes or goes so the old box does not stay behind.
        if (stretch && app->shown_toast != toast)
            mask |= UI_OVERLAY_VIEWPORT;
        return mask;
    }
    return 0;
}

static void remember_chrome(struct app_state *app, int64_t now_ms) {
    app->shown_mode = UI_MODE_GAMEPLAY;
    app->shown_bank = input_map_get_bank(&app->map);
    app->shown_toast = toast_on(app, now_ms);
    app->shown_stretch = app->overlay.stretch ? 1 : 0;
    snprintf(app->shown_title, sizeof app->shown_title, "%s", cart_title(app));
}

static int wake_for(int64_t now_ns, int64_t next_ns, bool playing) {
    if (!playing) return 16;
    if (next_ns <= now_ns) return 1;
    int64_t ms = (next_ns - now_ns + 999999LL) / 1000000LL;
    if (ms < 1) return 1;
    if (ms > 16) return 16;
    return (int)ms;
}

static int saves_allowed(UiMode mode) {
    return mode == UI_MODE_GAMEPLAY || mode == UI_MODE_FILE_MENU
        || mode == UI_MODE_GAME_MENU || mode == UI_MODE_SETTINGS_MENU;
}

static void filter_pads(struct input *game, unsigned block) {
    for (int p = 0; p < 16; p++) {
        if (!(block & (1u << p))) continue;
        game->pad_hit[p] = -1;
        game->pad_down[p] = 0;
        game->pad_pressure[p] = -1;
        game->pad_release[p] = 0;
        game->pad_vel[p] = 0;
    }
}

// The pad lights change when a cart starts, and that burst is when a false
// note-on tends to land. Anything down in this window stays out of the cart
// and is dropped, so the latch does not survive into play.
enum { SETTLE_FRAMES = 8 };

static void settle_input(struct app_state *app) {
    app->settle_left = SETTLE_FRAMES;
    clear_game_edges(app);
}

static void note_settled(struct app_state *app, const struct input *in) {
    if (app->settle_left > 0) {
        for (int p = 0; p < 16; p++)
            if (in->pad_down[p]) app->pad_settle |= 1u << p;
        app->button_settle |= in->held;
        app->settle_left--;
    }
    for (int p = 0; p < 16; p++)
        if (!in->pad_down[p]) app->pad_settle &= ~(1u << p);
    app->button_settle &= in->held;
}

static void quiet_settled(struct input *game, const struct app_state *app) {
    filter_pads(game, app->pad_settle);
    game->held &= ~app->button_settle;
    game->down &= ~app->button_settle;
    if (app->button_settle & (1u << IN_SHIFT)) game->shift = 0;
    if (app->button_settle & (1u << IN_ENCODER)) game->encoder_press = 0;
}

static int app_frame(struct ui *u, const struct input *in, void *state) {
    struct app_state *app = (struct app_state *)state;
    if (!app || !u || !in) return 0;

    int64_t now_ns = mono_ns();
    u->now_ms = now_ns / 1000000LL;

    if (in->down & (1u << IN_PAD_BANK)) {
        input_map_toggle_bank(&app->map);
        ui_overlay_show_toast(&app->overlay,
            (input_map_get_bank(&app->map) == 0) ? "Pad Bank A" : "Pad Bank B",
            u->now_ms, 1200);
    }

    if (in->down & (1u << IN_PLAY)) {
        tic_runner_set_paused(app->runner, false);
        ui_overlay_set_mode(&app->overlay, UI_MODE_GAMEPLAY);
    }
    if (in->down & (1u << IN_STOP)) {
        tic_runner_set_paused(app->runner, true);
        ui_overlay_set_mode(&app->overlay, UI_MODE_GAME_MENU);
    }
    if (in->down & (1u << IN_ERASE)) {
        audio_set_active(app, false);
        tic_runner_reset(app->runner);
        settle_input(app);
        ui_overlay_show_toast(&app->overlay, "Cartridge Reset", u->now_ms, 1500);
    }

    if (in->main_moved && app->audio)
        audio_engine_set_volume(app->audio, in->main);

    if (app->saves) savestate_bind(app->saves, app->runner);

    unsigned down_pads = 0;
    for (int p = 0; p < 16; p++)
        if (in->pad_down[p]) down_pads |= 1u << p;
    if (in->down & (1u << IN_SAMPLE_SELECT))
        app->pad_grandfather = down_pads;

    int allow = app->saves && app->runner && app->runner->is_loaded
        && saves_allowed(app->overlay.mode);
    int choosing = allow && (in->held & (1u << IN_SAMPLE_SELECT));
    if (choosing) {
        for (int p = 0; p < 16; p++) {
            if (in->pad_hit[p] >= 0) {
                savestate_set_slot(app->saves, p);
                app->pad_swallow |= 1u << p;
            } else if (in->pad_down[p] && !(app->pad_grandfather & (1u << p))) {
                app->pad_swallow |= 1u << p;
            }
        }
    }
    for (int p = 0; p < 16; p++)
        if (!in->pad_down[p]) app->pad_swallow &= ~(1u << p);
    if (!(in->held & (1u << IN_SAMPLE_SELECT)))
        app->pad_grandfather = 0;

    if (allow && (in->down & (1u << IN_SAMPLE_REC))) {
        int slot = savestate_slot(app->saves);
        char msg[64];
        if (in->shift) {
            if (savestate_save(app->saves, app->runner) == 0)
                snprintf(msg, sizeof msg, "Saved slot %d", slot + 1);
            else
                snprintf(msg, sizeof msg, "Can't save this game");
        } else if (!savestate_filled(app->saves, slot)) {
            snprintf(msg, sizeof msg, "Slot %d empty", slot + 1);
        } else if (savestate_load(app->saves, app->runner) == 0) {
            snprintf(msg, sizeof msg, "Loaded slot %d", slot + 1);
            settle_input(app);
        } else {
            snprintf(msg, sizeof msg, "Couldn't load slot");
        }
        ui_overlay_show_toast(&app->overlay, msg, u->now_ms, 1200);
        app->rec_flash_until_ms = u->now_ms + 200;
    }

    struct save_hud hud;
    savestate_hud(app->saves, choosing, &hud);
    struct input game_in = *in;
    filter_pads(&game_in, app->pad_swallow);

    UiMode mode0 = app->overlay.mode;
    if (mode0 != UI_MODE_GAMEPLAY)
        ui_clear(u, 0x080A0E);

    int req = ui_overlay_draw(&app->overlay, u, in, app->runner, &app->map,
                              mode0 == UI_MODE_GAMEPLAY ? UI_OVERLAY_INPUT : 0, &hud);
    if (req) return 1;

    if (mode0 == UI_MODE_GAMEPLAY && app->overlay.mode != UI_MODE_GAMEPLAY) {
        app->shown_mode = -1;
        clear_game_edges(app);
    }

    bool in_game = app->overlay.mode == UI_MODE_GAMEPLAY;
    bool loaded = app->runner && app->runner->is_loaded;
    if (in_game && loaded && tic_runner_is_paused(app->runner))
        tic_runner_set_paused(app->runner, false);

    bool playing = in_game && loaded && !tic_runner_is_paused(app->runner);
    int ticked = 0;
    if (playing) {
        if (!app->was_playing) {
            settle_input(app);
            app->was_playing = 1;
        }
        note_settled(app, in);
        quiet_settled(&game_in, app);
        ui_lift(u, app->button_settle, app->pad_settle);
        latch_game_input(app, &game_in);
        int n = ticks_due(now_ns, &app->next_tick_ns);
        if (n > 0) {
            tick_game(app, &game_in, n);
            ticked = 1;
        }
    } else {
        app->pad_settle = 0;
        app->button_settle = 0;
        app->settle_left = 0;
        app->was_playing = 0;
        clear_game_edges(app);
    }

    if (in_game) {
        unsigned paint = chrome_mask(app, u->now_ms);
        if (ticked || app->shown_mode != (int)UI_MODE_GAMEPLAY || choosing)
            paint |= UI_OVERLAY_VIEWPORT;
        if (choosing)
            paint |= UI_OVERLAY_CHROME;
        if (paint) {
            ui_overlay_draw(&app->overlay, u, in, app->runner, &app->map, paint, &hud);
            if (paint & UI_OVERLAY_CHROME)
                remember_chrome(app, u->now_ms);
        }
    } else {
        app->shown_mode = -1;
    }

    audio_set_active(app, playing);

    struct input led_in = *in;
    filter_pads(&led_in, app->pad_settle);
    for (int p = 0; p < 16; p++) {
        if (choosing) {
            int filled = savestate_filled(app->saves, p);
            int current = savestate_slot(app->saves) == p;
            if (current) ui_led(u, p, 80, 64, 0);
            else if (filled) ui_led(u, p, 0, 48, 64);
            else ui_led(u, p, 8, 8, 12);
        } else {
            uint8_t r = 0, g = 0, b = 0;
            input_map_get_pad_led(&app->map, &led_in, p, &r, &g, &b);
            ui_led(u, p, r, g, b);
        }
    }

    if (app->audio) {
        int l_mask = 0, r_mask = 0;
        audio_engine_get_meter_masks(app->audio, &l_mask, &r_mask);
        ui_meter(u, l_mask, r_mask);
    }

    ui_button_led(u, IN_PLAY, playing ? 2 : 0);
    ui_button_led(u, IN_STOP, playing ? 0 : 2);
    ui_button_led(u, IN_PAD_BANK, (input_map_get_bank(&app->map) == 0) ? 1 : 2);
    ui_button_led(u, IN_ERASE, 0);
    ui_button_led(u, IN_SAMPLE_SELECT, choosing ? 2 : 0);
    ui_button_led(u, IN_SAMPLE_REC, u->now_ms < app->rec_flash_until_ms ? 2 : 0);

    ui_wake(u, wake_for(now_ns, app->next_tick_ns, playing));
    return 0;
}

int main(int argc, char **argv) {
    struct app_state app;
    memset(&app, 0, sizeof(app));
    app.shown_mode = -1;

    config_set_exe_path(argc > 0 ? argv[0] : NULL);
    input_map_init(&app.map);
    ui_overlay_init(&app.overlay);

    struct app_settings settings;
    if (config_load_settings(&settings) == 0) {
        app.overlay.stretch_default = settings.stretch;
        app.overlay.stretch = settings.stretch;
        snprintf(app.overlay.default_dir, sizeof app.overlay.default_dir, "%s", settings.default_dir);
    }

    app.saves = savestate_create();

    app.runner = tic_runner_create(44100);
    if (!app.runner) {
        fprintf(stderr, "Failed to create TIC-80 runner\n");
        return 1;
    }

    app.audio = audio_engine_create(44100, 512);
    if (app.audio) {
        audio_engine_set_source(app.audio, app_audio_fill, &app);
        audio_engine_start(app.audio);
    }

    const char *initial_cart = NULL;
    int force_stretch = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stretch") == 0)
            force_stretch = 1;
        else if (!initial_cart && argv[i][0])
            initial_cart = argv[i];
    }
    if (initial_cart && access(initial_cart, F_OK) == 0) {
        tic_runner_load_file(app.runner, initial_cart);
        config_load_for_cart(&app.map, initial_cart);
        app.overlay.stretch = app.overlay.stretch_default;
        if (force_stretch) app.overlay.stretch = true;
        ui_overlay_set_mode(&app.overlay, UI_MODE_GAMEPLAY);
    } else {
        config_load_default(&app.map);
        if (force_stretch) app.overlay.stretch = true;
        ui_overlay_set_mode(&app.overlay, UI_MODE_FILEBROWSER);
    }

    int rc = app_run(app_frame, &app);

    if (app.audio) {
        audio_engine_destroy(app.audio);
        app.audio = NULL;
    }

    if (app.runner) {
        tic_runner_destroy(app.runner);
        app.runner = NULL;
    }
    savestate_destroy(app.saves);

    return rc;
}
