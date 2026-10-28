#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <math.h>

#include "tic_runner.h"
#include "input_map.h"
#include "audio_engine.h"
#include "config.h"
#include "ui.h"

static void test_tic_runner(void) {
    printf("[TEST] Running TIC-80 core lifecycle test...\n");

    struct tic_runner *r = tic_runner_create(44100);
    assert(r != NULL);

    const char *test_lua =
        "-- title: Hello MPC\n"
        "-- author: Test\n"
        "local t=0\n"
        "function TIC()\n"
        "  t = t + 1\n"
        "  cls(13)\n"
        "  print('HELLO MPC TIC-80', 60, 60, 15)\n"
        "  pix(10, 10, 14)\n"
        "end\n";

    int rc = tic_runner_load_buffer(r, test_lua, (int)strlen(test_lua), "test.lua");
    assert(rc == 0);
    assert(r->is_loaded == true);
    assert(strcmp(r->cart_title, "Hello MPC") == 0);

    // Tick 60 frames
    tic80_input in = {0};
    for (int i = 0; i < 60; i++) {
        tic_runner_tick(r, &in);
    }
    assert(r->frame_count == 60);

    // Test render into mock UI buffer
    uint32_t pixels[UI_W * UI_H];
    memset(pixels, 0, sizeof(pixels));
    struct ui u = {.px = pixels};

    tic_runner_render(r, &u);

    // Verify center pixel has been drawn (not zero)
    uint32_t center_pixel = pixels[(TIC_RUNNER_VP_Y + 60) * UI_W + (TIC_RUNNER_VP_X + 60)];
    assert(center_pixel != 0);

    // Test sound generation
    tic_runner_sound(r);
    int sample_count = 0;
    const int16_t *samples = tic_runner_get_samples(r, &sample_count);
    assert(samples != NULL);
    assert(sample_count > 0);

    tic_runner_destroy(r);
    printf("[PASS] TIC-80 core lifecycle test passed.\n");
}

static void test_input_mapping(void) {
    printf("[TEST] Running input mapping & rebinding test...\n");

    struct input_map map;
    input_map_init(&map);

    // Default Arcade preset: Pad 5 (index 4) should be Left
    assert(map.pads[4].type == BIND_TYPE_GAMEPAD);
    assert(map.pads[4].player == 0);
    assert(map.pads[4].code == GP_BTN_LEFT);

    // Test translation with Pad 5 pressed
    struct input in;
    memset(&in, 0, sizeof(in));
    in.pad_down[4] = 1;
    in.pad_vel[4] = 120;

    tic80_input out;
    input_map_translate(&map, &in, &out);
    assert(out.gamepads.first.left == 1);
    assert(out.gamepads.first.right == 0);

    // Test Bank B translation: switch bank to Bank B (player 2)
    input_map_set_bank(&map, 1);
    input_map_translate(&map, &in, &out);
    assert(out.gamepads.second.left == 1);
    assert(out.gamepads.first.left == 0);

    // Test custom binding: map Pad 1 (index 0) in Bank A to Space
    input_map_set_bank(&map, 0);
    input_map_set_binding(&map, 0, BIND_TYPE_KEY, 0, tic_key_space);
    in.pad_down[4] = 0;
    in.pad_down[0] = 1;

    input_map_translate(&map, &in, &out);
    assert(out.keyboard.keys[0] == tic_key_space);

    // Test pad LED color retrieval
    uint8_t r = 0, g = 0, b = 0;
    input_map_get_pad_led(&map, &in, 0, &r, &g, &b);
    assert(r > 0 || g > 0 || b > 0); // Should be lit

    printf("[PASS] Input mapping & rebinding test passed.\n");
}

static void test_config_persistence(void) {
    printf("[TEST] Running config JSON persistence test...\n");

    struct input_map m1, m2;
    input_map_init(&m1);
    input_map_init(&m2);

    // Make unique customizations
    input_map_set_binding(&m1, 2, BIND_TYPE_KEY, 0, tic_key_z);
    input_map_set_binding(&m1, 3, BIND_TYPE_KEY, 0, tic_key_x);
    input_map_set_binding(&m1, 15, BIND_TYPE_MOUSE_BTN, 0, 1);

    const char *test_path = ".test_bindings.json";
    int rc = config_save_profile(&m1, test_path);
    assert(rc == 0);

    rc = config_load_profile(&m2, test_path);
    assert(rc == 0);

    // Assert customizations match
    assert(m2.pads[2].type == BIND_TYPE_KEY);
    assert(m2.pads[2].code == tic_key_z);
    assert(m2.pads[3].type == BIND_TYPE_KEY);
    assert(m2.pads[3].code == tic_key_x);
    assert(m2.pads[15].type == BIND_TYPE_MOUSE_BTN);
    assert(m2.pads[15].code == 1);

    unlink(test_path);
    printf("[PASS] Config JSON persistence test passed.\n");
}

static void test_audio_engine(void) {
    printf("[TEST] Running audio engine mock test...\n");

    struct audio_engine *ae = audio_engine_create(44100, 128);
    assert(ae != NULL);

    int16_t dummy_samples[256];
    for (int i = 0; i < 256; i++) {
        dummy_samples[i] = (int16_t)(sin(i * 0.1) * 20000.0);
    }

    audio_engine_push_samples(ae, dummy_samples, 256);
    audio_engine_set_volume(ae, 120);
    assert(audio_engine_get_volume(ae) == 120);

    audio_engine_destroy(ae);
    printf("[PASS] Audio engine mock test passed.\n");
}

int main(void) {
    printf("========================================\n");
    printf("   MPC TIC-80 Host Unit Test Suite\n");
    printf("========================================\n");

    test_tic_runner();
    test_input_mapping();
    test_config_persistence();
    test_audio_engine();

    printf("========================================\n");
    printf("   ALL UNIT TESTS PASSED SUCCESSFULLY!  \n");
    printf("========================================\n");
    return 0;
}
