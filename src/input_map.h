#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "tic80.h"
#include "tic.h"
#include "input.h"

#define INPUT_MAP_TOTAL_PADS 32 // 16 Bank A (0..15), 16 Bank B (16..31)

typedef enum {
    BIND_TYPE_NONE = 0,
    BIND_TYPE_GAMEPAD,   // Gamepad button (P1 or P2)
    BIND_TYPE_KEY,       // Keyboard key (tic_key)
    BIND_TYPE_MOUSE_BTN, // Mouse button (left/middle/right)
    BIND_TYPE_MOUSE_DIR, // Mouse move direction
    BIND_TYPE_MACRO      // App macro (menu, reset, etc.)
} BindingType;

typedef enum {
    GP_BTN_UP = 0,
    GP_BTN_DOWN,
    GP_BTN_LEFT,
    GP_BTN_RIGHT,
    GP_BTN_A,
    GP_BTN_B,
    GP_BTN_X,
    GP_BTN_Y,
    GP_BTN_COUNT
} GamepadButton;

typedef enum {
    MACRO_NONE = 0,
    MACRO_PAUSE,
    MACRO_RESET,
    MACRO_KEYBOARD,
    MACRO_BANK_SWAP
} MacroAction;

struct pad_binding {
    BindingType type;
    uint8_t player;    // 0 = P1, 1 = P2
    uint8_t code;      // GamepadButton, tic_key, mouse button, or MacroAction
    uint32_t color;    // 0x00RRGGBB pad LED color
    char label[12];    // Short display label e.g. "P1 UP", "KEY Z", "A", "SPC"
};

typedef enum {
    PRESET_ARCADE = 0,
    PRESET_ERGONOMIC,
    PRESET_KEYBOARD,
    PRESET_TWOPLAYER,
    PRESET_COUNT
} InputPreset;

struct input_map {
    struct pad_binding pads[INPUT_MAP_TOTAL_PADS];
    int active_bank; // 0 = Bank A (pads 0..15), 1 = Bank B (pads 16..31)
    InputPreset current_preset;
    bool is_custom;
};

void input_map_init(struct input_map *m);
void input_map_apply_preset(struct input_map *m, InputPreset preset);
const char* input_map_preset_name(InputPreset preset);

void input_map_set_bank(struct input_map *m, int bank);
int input_map_get_bank(const struct input_map *m);
void input_map_toggle_bank(struct input_map *m);

void input_map_set_binding(struct input_map *m, int pad_idx, BindingType type, uint8_t player, uint8_t code);
void input_map_clear_binding(struct input_map *m, int pad_idx);

// Translates hardware MPC input state to TIC-80 input state
void input_map_translate(const struct input_map *m, const struct input *in, tic80_input *out_tic_input);

// Returns RGB color for physical pad LEDs (including press brightness flash)
void input_map_get_pad_led(const struct input_map *m, const struct input *in, int pad, uint8_t *r, uint8_t *g, uint8_t *b);

const char* input_map_key_name(uint8_t key);
const char* input_map_gp_btn_name(uint8_t btn);
uint32_t input_map_default_color_for(BindingType type, uint8_t code);
