#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "app.h"
#include "input.h"
#include "ui.h"

#include "tic_runner.h"
#include "input_map.h"
#include "audio_engine.h"
#include "ui_overlay.h"
#include "config.h"

struct app_state {
    struct tic_runner *runner;
    struct input_map map;
    struct ui_overlay overlay;
    struct audio_engine *audio;
    bool quit;
};

static int app_frame(struct ui *u, const struct input *in, void *state) {
    struct app_state *app = (struct app_state*)state;
    if (!app || !u || !in) return 0;

    // Shift + Encoder click cleanly exits to the boot menu
    if (input_quit(in)) {
        return 1;
    }

    // Hardware PAD BANK button toggles Bank A / Bank B (giving 32 pads)
    if (in->down & (1u << IN_PAD_BANK)) {
        input_map_toggle_bank(&app->map);
        ui_overlay_show_toast(&app->overlay,
            (input_map_get_bank(&app->map) == 0) ? "Pad Bank A" : "Pad Bank B",
            u->now_ms, 1200);
    }

    // Hardware Transport buttons:
    // PLAY: Resume gameplay
    if (in->down & (1u << IN_PLAY)) {
        tic_runner_set_paused(app->runner, false);
        ui_overlay_set_mode(&app->overlay, UI_MODE_GAMEPLAY);
    }
    // STOP: Pause and open Menu
    if (in->down & (1u << IN_STOP)) {
        tic_runner_set_paused(app->runner, true);
        ui_overlay_set_mode(&app->overlay, UI_MODE_MENU);
    }
    // ERASE: Quick restart cart
    if (in->down & (1u << IN_ERASE)) {
        tic_runner_reset(app->runner);
        ui_overlay_show_toast(&app->overlay, "Cartridge Reset", u->now_ms, 1500);
    }

    // Master volume knob & Q-Link Fader
    if (in->main_moved && app->audio) {
        audio_engine_set_volume(app->audio, in->main);
    }

    // Tick TIC-80 emulation core during active gameplay
    if (app->overlay.mode == UI_MODE_GAMEPLAY && !tic_runner_is_paused(app->runner)) {
        tic80_input tic_in;
        input_map_translate(&app->map, in, &tic_in);

        tic_runner_tick(app->runner, &tic_in);

        // Synthesize audio
        tic_runner_sound(app->runner);
        int sample_count = 0;
        const int16_t *samples = tic_runner_get_samples(app->runner, &sample_count);
        if (samples && sample_count > 0 && app->audio) {
            audio_engine_push_samples(app->audio, samples, sample_count * 2);
        }
    }

    // Clear frame to deep matte black
    ui_clear(u, 0x080A0E);

    // Draw active overlay mode (gameplay, rebind modal, file browser, keyboard)
    int req_exit = ui_overlay_draw(&app->overlay, u, in, app->runner, &app->map);
    if (req_exit) {
        return 1;
    }

    // Update 16 physical RGB pad LEDs with mapped colors & hit flashes
    for (int p = 0; p < 16; p++) {
        uint8_t r = 0, g = 0, b = 0;
        input_map_get_pad_led(&app->map, in, p, &r, &g, &b);
        ui_led(u, p, r, g, b);
    }

    // Update hardware stereo VU meter LEDs (CC 83 & 84, right of display)
    if (app->audio) {
        int l_mask = 0, r_mask = 0;
        audio_engine_get_meter_masks(app->audio, &l_mask, &r_mask);
        ui_meter(u, l_mask, r_mask);
    }

    // Update physical button backlight LEDs
    bool is_playing = (app->overlay.mode == UI_MODE_GAMEPLAY && !tic_runner_is_paused(app->runner));
    ui_button_led(u, IN_PLAY, is_playing ? 2 : 0);
    ui_button_led(u, IN_STOP, !is_playing ? 2 : 0);
    ui_button_led(u, IN_PAD_BANK, (input_map_get_bank(&app->map) == 0) ? 1 : 2);
    ui_button_led(u, IN_ERASE, 0);

    // Request wake in 16ms (60 FPS game loop)
    ui_wake(u, 16);
    return 0;
}

int main(int argc, char **argv) {
    struct app_state app;
    memset(&app, 0, sizeof(app));

    input_map_init(&app.map);
    ui_overlay_init(&app.overlay);

    // Initialize TIC-80 core
    app.runner = tic_runner_create(44100);
    if (!app.runner) {
        fprintf(stderr, "Failed to create TIC-80 runner\n");
        return 1;
    }

    // Open Realtime ALSA Audio Engine
    app.audio = audio_engine_create(44100, 128);
    if (app.audio) {
        audio_engine_start(app.audio);
    }

    // Load cartridge if provided in arguments
    const char *initial_cart = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : NULL;
    if (initial_cart && access(initial_cart, F_OK) == 0) {
        tic_runner_load_file(app.runner, initial_cart);
        config_load_for_cart(&app.map, initial_cart);
        ui_overlay_set_mode(&app.overlay, UI_MODE_GAMEPLAY);
    } else {
        // Start in File Browser so user can choose a cartridge
        config_load_default(&app.map);
        ui_overlay_set_mode(&app.overlay, UI_MODE_FILEBROWSER);
    }

    // Run device display & MIDI surface event loop
    int rc = app_run(app_frame, &app);

    // Clean shutdown
    if (app.audio) {
        audio_engine_destroy(app.audio);
        app.audio = NULL;
    }

    if (app.runner) {
        tic_runner_destroy(app.runner);
        app.runner = NULL;
    }

    return rc;
}
