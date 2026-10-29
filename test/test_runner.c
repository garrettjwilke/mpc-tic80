#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "tic_runner.h"
#include "input_map.h"
#include "audio_engine.h"
#include "config.h"
#include "ui.h"
#include "savestate.h"
#include "core/core.h"
#include "lua.h"

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

    tic_runner_render(r, &u, false);

    // Verify center pixel has been drawn (not zero)
    uint32_t center_pixel = pixels[(TIC_RUNNER_VP_Y + 60) * UI_W + (TIC_RUNNER_VP_X + 60)];
    assert(center_pixel != 0);

    // samples.count is int16 words, channels included: 44100 * 2 / 60 = 1470.
    tic_runner_sound(r);
    int sample_count = 0;
    const int16_t *samples = tic_runner_get_samples(r, &sample_count);
    assert(samples != NULL);
    assert(sample_count == 44100 * 2 / 60);
    volatile int16_t last_sample = samples[sample_count - 1];
    (void)last_sample;

    tic_runner_reset(r);
    assert(r->frame_count == 0);
    assert(strcmp(r->cart_title, "Hello MPC") == 0);
    tic_runner_tick(r, &in);
    assert(r->frame_count == 1);
    tic_runner_render(r, &u, false);
    center_pixel = pixels[(TIC_RUNNER_VP_Y + 60) * UI_W + (TIC_RUNNER_VP_X + 60)];
    assert(center_pixel != 0);

    tic_runner_destroy(r);
    printf("[PASS] TIC-80 core lifecycle test passed.\n");
}

static uint32_t vp_pix(const uint32_t *px, int x, int y) {
    return px[(TIC_RUNNER_VP_Y + y) * UI_W + (TIC_RUNNER_VP_X + x)];
}

// "Press Z" carts call btnp(4). Z has to become button A, the way the
// TIC-80 studio's keymap does, and a held key must stay visible to key().
static void test_key_reaches_buttons(void) {
    printf("[TEST] Running keyboard-to-button mapping test...\n");
    assert(tic_key_z == 26);

    struct tic_runner *r = tic_runner_create(44100);
    assert(r != NULL);

    const char *lua =
        "function TIC()\n"
        "  local c = 1\n"
        "  if btnp(4) then c = 2\n"
        "  elseif key(26) then c = 3\n"
        "  end\n"
        "  cls(c)\n"
        "end\n";
    assert(tic_runner_load_buffer(r, lua, (int)strlen(lua), "keys.lua") == 0);

    uint32_t pixels[UI_W * UI_H];
    struct ui u = {.px = pixels};
    tic80_input in = {0};

    tic_runner_tick(r, &in);
    tic_runner_render(r, &u, false);
    uint32_t idle = vp_pix(pixels, 20, 20);

    in.keyboard.keys[0] = tic_key_z;
    tic_runner_tick(r, &in);
    tic_runner_render(r, &u, false);
    uint32_t pressed = vp_pix(pixels, 20, 20);
    assert(pressed != idle);

    tic_runner_tick(r, &in);
    tic_runner_render(r, &u, false);
    uint32_t held = vp_pix(pixels, 20, 20);
    assert(held != idle);
    assert(held != pressed);

    memset(&in, 0, sizeof in);
    tic_runner_tick(r, &in);
    in.gamepads.first.a = 1;
    tic_runner_tick(r, &in);
    tic_runner_render(r, &u, false);
    assert(vp_pix(pixels, 20, 20) == pressed);

    // Arcade pad 14 is the Z key. The same press must light button A.
    struct input_map map;
    input_map_init(&map);
    struct input hw;
    memset(&hw, 0, sizeof hw);
    memset(&in, 0, sizeof in);
    tic_runner_tick(r, &in);
    hw.pad_down[13] = 1;
    input_map_translate(&map, &hw, &in);
    assert(in.keyboard.keys[0] == tic_key_z);
    tic_runner_tick(r, &in);
    tic_runner_render(r, &u, false);
    assert(vp_pix(pixels, 20, 20) == pressed);

    tic_runner_destroy(r);
    printf("[PASS] Keyboard-to-button mapping test passed.\n");
}

static uint32_t screen_pix(const uint32_t *px, int x, int y) {
    return px[y * UI_W + x];
}

// Stretch scales 240x136 onto the full width, from under the top bar to the
// bottom of the screen. Width is exactly 4/3, so every third source column
// is drawn twice.
static void test_stretch_render(void) {
    printf("[TEST] Running stretch viewport test...\n");
    assert(TIC_RUNNER_STRETCH_W == 320);
    assert(TIC_RUNNER_STRETCH_H == UI_H - TIC_RUNNER_STRETCH_Y);
    assert(TIC_RUNNER_STRETCH_H == 216);
    assert(TIC_RUNNER_STRETCH_X == 0);
    assert(TIC_RUNNER_STRETCH_Y == TIC_RUNNER_VP_Y);

    struct tic_runner *r = tic_runner_create(44100);
    assert(r != NULL);

    const char *lua =
        "function TIC()\n"
        "  cls(1)\n"
        "  pix(0, 0, 12)\n"
        "  pix(1, 0, 14)\n"
        "  pix(2, 0, 15)\n"
        "  pix(239, 135, 10)\n"
        "end\n";
    assert(tic_runner_load_buffer(r, lua, (int)strlen(lua), "stretch.lua") == 0);

    tic80_input in = {0};
    tic_runner_tick(r, &in);

    uint32_t stretched[UI_W * UI_H];
    uint32_t native[UI_W * UI_H];
    memset(stretched, 0, sizeof(stretched));
    memset(native, 0, sizeof(native));
    struct ui us = {.px = stretched};
    struct ui un = {.px = native};
    tic_runner_render(r, &us, true);
    tic_runner_render(r, &un, false);

    int top = TIC_RUNNER_STRETCH_Y;
    int bot = TIC_RUNNER_STRETCH_Y + TIC_RUNNER_STRETCH_H - 1;
    uint32_t c0 = screen_pix(stretched, 0, top);
    uint32_t c1 = screen_pix(stretched, 2, top);
    uint32_t c2 = screen_pix(stretched, 3, top);
    assert(c0 != 0);
    assert(screen_pix(stretched, 1, top) == c0);
    assert(c1 != c0);
    assert(c2 != c1);
    assert(screen_pix(stretched, 319, top) != c0);
    assert(screen_pix(stretched, 319, bot) != screen_pix(stretched, 319, bot - 1));
    assert(bot == UI_H - 1);
    assert(screen_pix(stretched, 0, top - 1) == 0);
    assert(screen_pix(stretched, 0, top) == screen_pix(native, TIC_RUNNER_VP_X, TIC_RUNNER_VP_Y));
    assert(screen_pix(native, 0, TIC_RUNNER_VP_Y) == 0);

    tic_runner_destroy(r);
    printf("[PASS] Stretch viewport test passed.\n");
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

static void test_config_beside_exe(void) {
    printf("[TEST] Running config files beside executable...\n");
    config_set_exe_path("build/host/test-runner");

    char cart_path[CONFIG_PATH_MAX];
    char def_path[CONFIG_PATH_MAX];
    char settings_path[CONFIG_PATH_MAX];
    assert(config_make_path(cart_path, sizeof cart_path, "bindings-Puzzle.json") == 0);
    assert(config_make_path(def_path, sizeof def_path, "default_bindings.json") == 0);
    assert(config_make_path(settings_path, sizeof settings_path, "settings.json") == 0);
    unlink(cart_path);
    unlink(def_path);
    unlink(settings_path);

    struct input_map m1, m2;
    input_map_init(&m1);
    input_map_set_binding(&m1, 4, BIND_TYPE_KEY, 0, tic_key_z);
    assert(config_save_for_cart(&m1, "/media/MPC/carts/Puzzle.tic") == 0);
    assert(access("/media/MPC/carts/Puzzle.tic.cfg", F_OK) != 0);
    assert(access(cart_path, F_OK) == 0);

    input_map_init(&m2);
    assert(config_load_for_cart(&m2, "/tmp/other/Puzzle.tic") == 0);
    assert(m2.pads[4].type == BIND_TYPE_KEY);
    assert(m2.pads[4].code == tic_key_z);

    unlink(cart_path);
    input_map_init(&m1);
    input_map_set_binding(&m1, 5, BIND_TYPE_KEY, 0, tic_key_x);
    assert(config_save_default(&m1) == 0);
    input_map_init(&m2);
    input_map_set_binding(&m2, 5, BIND_TYPE_KEY, 0, tic_key_z);
    assert(config_load_for_cart(&m2, "/tmp/other/NoSave.tic") == 0);
    assert(m2.pads[5].code == tic_key_x);

    unlink(def_path);
    input_map_init(&m2);
    input_map_set_binding(&m2, 5, BIND_TYPE_KEY, 0, tic_key_c);
    assert(config_load_for_cart(&m2, "/tmp/other/NoSave.tic") == 0);
    assert(m2.pads[5].type == BIND_TYPE_GAMEPAD);
    assert(m2.pads[5].code != tic_key_c);

    struct app_settings saved, loaded;
    config_settings_defaults(&saved);
    saved.stretch = false;
    snprintf(saved.default_dir, sizeof saved.default_dir, "%s", "/media/MPC/games");
    assert(config_save_settings(&saved) == 0);
    config_settings_defaults(&loaded);
    assert(config_load_settings(&loaded) == 0);
    assert(loaded.stretch == false);
    assert(strcmp(loaded.default_dir, "/media/MPC/games") == 0);

    unlink(cart_path);
    unlink(def_path);
    unlink(settings_path);
    printf("[PASS] Config files stay beside the executable.\n");
}

static int g_fill_calls;

static void test_fill(void *ctx, int16_t *dst, int frames) {
    (void)ctx;
    g_fill_calls++;
    for (int i = 0; i < frames * 2; i++)
        dst[i] = (int16_t)(i * 31);
}

static void test_audio_engine(void) {
    printf("[TEST] Running audio engine mock test...\n");

    struct audio_engine *ae = audio_engine_create(44100, 128);
    assert(ae != NULL);

    g_fill_calls = 0;
    audio_engine_set_source(ae, test_fill, NULL);
    audio_engine_set_active(ae, true);
    audio_engine_set_volume(ae, 120);
    assert(audio_engine_get_volume(ae) == 120);
    assert(audio_engine_start(ae));
    usleep(80 * 1000);
    audio_engine_stop(ae);
    assert(g_fill_calls > 0);

    audio_engine_destroy(ae);
    printf("[PASS] Audio engine mock test passed.\n");
}

static int ram_at(struct tic_runner *r, int addr) {
    tic_mem *mem = (tic_mem *)r->tic;
    return mem && mem->ram ? mem->ram->data[addr] : -1;
}

static int global_int(struct tic_runner *r, const char *name) {
    tic_core *core = (tic_core *)r->tic;
    lua_State *L = core->currentVM;
    lua_getglobal(L, name);
    int v = (int)lua_tointeger(L, -1);
    int ok = lua_isinteger(L, -1);
    lua_pop(L, 1);
    return ok ? v : -1;
}

static int upvalue_int(struct tic_runner *r, const char *func, const char *up) {
    tic_core *core = (tic_core *)r->tic;
    lua_State *L = core->currentVM;
    lua_getglobal(L, func);
    int v = -1;
    if (lua_isfunction(L, -1)) {
        for (int i = 1; ; i++) {
            const char *name = lua_getupvalue(L, -1, i);
            if (!name) break;
            if (strcmp(name, up) == 0 && lua_isinteger(L, -1))
                v = (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return v;
}

static struct tic_runner *load_named(const char *lua, const char *path) {
    struct tic_runner *r = tic_runner_create(44100);
    assert(r);
    assert(tic_runner_load_buffer(r, lua, (int)strlen(lua), path) == 0);
    snprintf(r->cart_path, sizeof r->cart_path, "%s", path);
    return r;
}

static void tick_n(struct tic_runner *r, int n) {
    tic80_input in = {0};
    for (int i = 0; i < n; i++) tic_runner_tick(r, &in);
}

static void test_savestate(void) {
    printf("[TEST] Running save state roundtrip...\n");
    config_set_exe_path("build/host/test-runner");

    const char *cart =
        "local t = 0\n"
        "n = 0\n"
        "function TIC()\n"
        "  t = t + 1\n"
        "  n = n + 1\n"
        "  poke(0, n)\n"
        "  poke(1, t)\n"
        "end\n";
    struct tic_runner *r = load_named(cart, "roundtrip.tic");
    struct savestate *s = savestate_create();
    assert(s);
    savestate_bind(s, r);
    char file[CONFIG_PATH_MAX];
    assert(config_make_path(file, sizeof file, "states-roundtrip.bin") == 0);
    unlink(file);

    tick_n(r, 10);
    assert(global_int(r, "n") == 10);
    assert(upvalue_int(r, "TIC", "t") == 10);
    assert(ram_at(r, 0) == 10);
    assert(ram_at(r, 1) == 10);
    assert(savestate_save(s, r) == 0);
    assert(savestate_filled(s, 0));

    tick_n(r, 5);
    assert(global_int(r, "n") == 15);
    assert(ram_at(r, 0) == 15);
    assert(savestate_load(s, r) == 0);
    assert(global_int(r, "n") == 10);
    assert(upvalue_int(r, "TIC", "t") == 10);
    assert(ram_at(r, 0) == 10);
    assert(ram_at(r, 1) == 10);
    tick_n(r, 1);
    assert(global_int(r, "n") == 11);
    assert(upvalue_int(r, "TIC", "t") == 11);
    assert(ram_at(r, 0) == 11);
    assert(ram_at(r, 1) == 11);

    savestate_destroy(s);
    tick_n(r, 4);
    assert(global_int(r, "n") == 15);
    s = savestate_create();
    savestate_bind(s, r);
    assert(savestate_slot(s) == 0);
    assert(savestate_filled(s, 0));
    assert(savestate_load(s, r) == 0);
    assert(global_int(r, "n") == 10);
    assert(upvalue_int(r, "TIC", "t") == 10);

    savestate_set_slot(s, 4);
    assert(savestate_load(s, r) != 0);
    assert(!savestate_filled(s, 4));

    tic_core *core = (tic_core *)r->tic;
    lua_newuserdata(core->currentVM, 4);
    lua_setglobal(core->currentVM, "blob");
    savestate_set_slot(s, 0);
    assert(savestate_save(s, r) != 0);
    assert(savestate_load(s, r) == 0);
    assert(global_int(r, "n") == 10);
    lua_pushnil(core->currentVM);
    lua_setglobal(core->currentVM, "blob");

    const char *co_cart =
        "local x = 0\n"
        "local co = coroutine.create(function()\n"
        "  while true do\n"
        "    x = x + 1\n"
        "    coroutine.yield()\n"
        "  end\n"
        "end)\n"
        "function TIC()\n"
        "  coroutine.resume(co)\n"
        "  poke(0, x)\n"
        "end\n";
    struct tic_runner *co = load_named(co_cart, "coroutine.tic");
    struct savestate *cs = savestate_create();
    savestate_bind(cs, co);
    char co_file[CONFIG_PATH_MAX];
    assert(config_make_path(co_file, sizeof co_file, "states-coroutine.bin") == 0);
    unlink(co_file);
    tick_n(co, 8);
    assert(ram_at(co, 0) == 8);
    assert(savestate_save(cs, co) == 0);
    tick_n(co, 3);
    assert(ram_at(co, 0) == 11);
    assert(savestate_load(cs, co) == 0);
    assert(ram_at(co, 0) == 8);
    tick_n(co, 1);
    assert(ram_at(co, 0) == 9);

    const char *open_cart =
        "local read\n"
        "local co\n"
        "function BOOT()\n"
        "  co = coroutine.create(function()\n"
        "    local y = 0\n"
        "    read = function() return y end\n"
        "    while true do\n"
        "      y = y + 1\n"
        "      coroutine.yield()\n"
        "    end\n"
        "  end)\n"
        "  coroutine.resume(co)\n"
        "end\n"
        "function TIC()\n"
        "  coroutine.resume(co)\n"
        "  poke(0, read())\n"
        "end\n";
    struct tic_runner *op = load_named(open_cart, "openup.tic");
    struct savestate *os = savestate_create();
    savestate_bind(os, op);
    char open_file[CONFIG_PATH_MAX];
    assert(config_make_path(open_file, sizeof open_file, "states-openup.bin") == 0);
    unlink(open_file);
    tick_n(op, 6);
    int saved = ram_at(op, 0);
    assert(saved > 0);
    assert(savestate_save(os, op) == 0);
    tick_n(op, 4);
    assert(ram_at(op, 0) == saved + 4);
    assert(savestate_load(os, op) == 0);
    assert(ram_at(op, 0) == saved);
    tick_n(op, 1);
    assert(ram_at(op, 0) == saved + 1);

    unlink(file);
    unlink(co_file);
    unlink(open_file);
    savestate_destroy(s);
    savestate_destroy(cs);
    savestate_destroy(os);
    tic_runner_destroy(r);
    tic_runner_destroy(co);
    tic_runner_destroy(op);
    printf("[PASS] Save state roundtrip passed.\n");
}

int main(void) {
    printf("========================================\n");
    printf("   MPC TIC-80 Host Unit Test Suite\n");
    printf("========================================\n");

    test_tic_runner();
    test_stretch_render();
    test_key_reaches_buttons();
    test_input_mapping();
    test_config_persistence();
    test_config_beside_exe();
    test_savestate();
    test_audio_engine();

    printf("========================================\n");
    printf("   ALL UNIT TESTS PASSED SUCCESSFULLY!  \n");
    printf("========================================\n");
    return 0;
}
