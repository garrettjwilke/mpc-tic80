#include "input_map.h"
#include <stdio.h>
#include <string.h>

#define COLOR_CYAN    0x00E5FF
#define COLOR_GREEN   0x00E676
#define COLOR_RED     0xFF1744
#define COLOR_YELLOW  0xFFD600
#define COLOR_BLUE    0x2979FF
#define COLOR_PURPLE  0xD500F9
#define COLOR_AMBER   0xFF9100
#define COLOR_ORANGE  0xFF6D00
#define COLOR_DIM     0x12151C

static const char *GP_BTN_NAMES[GP_BTN_COUNT] = {
    "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y"
};

const char* input_map_gp_btn_name(uint8_t btn) {
    if (btn < GP_BTN_COUNT) return GP_BTN_NAMES[btn];
    return "?";
}

const char* input_map_key_name(uint8_t key) {
    if (key >= tic_key_a && key <= tic_key_z) {
        static char letter[2] = {0};
        letter[0] = (char)('A' + (key - tic_key_a));
        return letter;
    }
    if (key >= tic_key_0 && key <= tic_key_9) {
        static char digit[2] = {0};
        digit[0] = (char)('0' + (key - tic_key_0));
        return digit;
    }
    switch (key) {
        case tic_key_space:     return "SPC";
        case tic_key_return:    return "RET";
        case tic_key_backspace: return "DEL";
        case tic_key_escape:    return "ESC";
        case tic_key_tab:       return "TAB";
        case tic_key_shift:     return "SHF";
        case tic_key_ctrl:      return "CTL";
        case tic_key_alt:       return "ALT";
        case tic_key_up:        return "ARW U";
        case tic_key_down:      return "ARW D";
        case tic_key_left:      return "ARW L";
        case tic_key_right:     return "ARW R";
        default:                return "KEY";
    }
}

uint32_t input_map_default_color_for(BindingType type, uint8_t code) {
    switch (type) {
        case BIND_TYPE_GAMEPAD:
            switch (code) {
                case GP_BTN_UP:
                case GP_BTN_DOWN:
                case GP_BTN_LEFT:
                case GP_BTN_RIGHT: return COLOR_CYAN;
                case GP_BTN_A:     return COLOR_GREEN;
                case GP_BTN_B:     return COLOR_RED;
                case GP_BTN_X:     return COLOR_YELLOW;
                case GP_BTN_Y:     return COLOR_BLUE;
                default:           return COLOR_CYAN;
            }
        case BIND_TYPE_KEY:
            switch (code) {
                case tic_key_space:
                case tic_key_return:
                case tic_key_escape: return COLOR_AMBER;
                case tic_key_z:
                case tic_key_x:
                case tic_key_c:      return COLOR_GREEN;
                default:             return COLOR_PURPLE;
            }
        case BIND_TYPE_MOUSE_BTN:
        case BIND_TYPE_MOUSE_DIR: return COLOR_ORANGE;
        case BIND_TYPE_MACRO:     return COLOR_AMBER;
        case BIND_TYPE_NONE:
        default:                  return COLOR_DIM;
    }
}

static void update_binding_label(struct pad_binding *b) {
    if (!b) return;
    switch (b->type) {
        case BIND_TYPE_GAMEPAD:
            if (b->player == 0) {
                snprintf(b->label, sizeof(b->label), "%s", input_map_gp_btn_name(b->code));
            } else {
                snprintf(b->label, sizeof(b->label), "P2 %s", input_map_gp_btn_name(b->code));
            }
            break;
        case BIND_TYPE_KEY:
            snprintf(b->label, sizeof(b->label), "%s", input_map_key_name(b->code));
            break;
        case BIND_TYPE_MOUSE_BTN:
            snprintf(b->label, sizeof(b->label), "M-BTN%d", b->code);
            break;
        case BIND_TYPE_MOUSE_DIR:
            snprintf(b->label, sizeof(b->label), "M-MOV");
            break;
        case BIND_TYPE_MACRO:
            switch (b->code) {
                case MACRO_PAUSE:    snprintf(b->label, sizeof(b->label), "PAUSE"); break;
                case MACRO_RESET:    snprintf(b->label, sizeof(b->label), "RESET"); break;
                case MACRO_KEYBOARD: snprintf(b->label, sizeof(b->label), "KBD");   break;
                case MACRO_BANK_SWAP:snprintf(b->label, sizeof(b->label), "BANK");  break;
                default:             snprintf(b->label, sizeof(b->label), "MACRO"); break;
            }
            break;
        case BIND_TYPE_NONE:
        default:
            snprintf(b->label, sizeof(b->label), "-");
            break;
    }
}

void input_map_set_binding(struct input_map *m, int pad_idx, BindingType type, uint8_t player, uint8_t code) {
    if (!m || pad_idx < 0 || pad_idx >= INPUT_MAP_TOTAL_PADS) return;
    struct pad_binding *b = &m->pads[pad_idx];
    b->type = type;
    b->player = player;
    b->code = code;
    b->color = input_map_default_color_for(type, code);
    update_binding_label(b);
    m->is_custom = true;
}

void input_map_clear_binding(struct input_map *m, int pad_idx) {
    if (!m || pad_idx < 0 || pad_idx >= INPUT_MAP_TOTAL_PADS) return;
    struct pad_binding *b = &m->pads[pad_idx];
    b->type = BIND_TYPE_NONE;
    b->player = 0;
    b->code = 0;
    b->color = COLOR_DIM;
    snprintf(b->label, sizeof(b->label), "-");
    m->is_custom = true;
}

void input_map_apply_preset(struct input_map *m, InputPreset preset) {
    if (!m) return;
    for (int i = 0; i < INPUT_MAP_TOTAL_PADS; i++) {
        input_map_clear_binding(m, i);
    }

    m->current_preset = preset;
    m->is_custom = false;

    switch (preset) {
        case PRESET_ARCADE:
            // Bank A (0..15):
            // D-Pad Inverted-T layout:
            // Pad 5 (idx 4): Left, Pad 6 (idx 5): Down, Pad 7 (idx 6): Right
            // Pad 10 (idx 9): Up
            input_map_set_binding(m, 4, BIND_TYPE_GAMEPAD, 0, GP_BTN_LEFT);
            input_map_set_binding(m, 5, BIND_TYPE_GAMEPAD, 0, GP_BTN_DOWN);
            input_map_set_binding(m, 6, BIND_TYPE_GAMEPAD, 0, GP_BTN_RIGHT);
            input_map_set_binding(m, 9, BIND_TYPE_GAMEPAD, 0, GP_BTN_UP);

            // Action Buttons:
            // Pad 11 (idx 10): X, Pad 12 (idx 11): Y
            // Pad 15 (idx 14): A, Pad 16 (idx 15): B
            input_map_set_binding(m, 10, BIND_TYPE_GAMEPAD, 0, GP_BTN_X);
            input_map_set_binding(m, 11, BIND_TYPE_GAMEPAD, 0, GP_BTN_Y);
            input_map_set_binding(m, 14, BIND_TYPE_GAMEPAD, 0, GP_BTN_A);
            input_map_set_binding(m, 15, BIND_TYPE_GAMEPAD, 0, GP_BTN_B);

            // Utilities:
            input_map_set_binding(m, 0, BIND_TYPE_KEY, 0, tic_key_escape); // Pad 1: Esc
            input_map_set_binding(m, 3, BIND_TYPE_KEY, 0, tic_key_return); // Pad 4: Return
            input_map_set_binding(m, 12, BIND_TYPE_KEY, 0, tic_key_space); // Pad 13: Space
            input_map_set_binding(m, 13, BIND_TYPE_KEY, 0, tic_key_z);     // Pad 14: Z

            // Bank B (16..31): Player 2 bindings for 2-player support
            input_map_set_binding(m, 16 + 4, BIND_TYPE_GAMEPAD, 1, GP_BTN_LEFT);
            input_map_set_binding(m, 16 + 5, BIND_TYPE_GAMEPAD, 1, GP_BTN_DOWN);
            input_map_set_binding(m, 16 + 6, BIND_TYPE_GAMEPAD, 1, GP_BTN_RIGHT);
            input_map_set_binding(m, 16 + 9, BIND_TYPE_GAMEPAD, 1, GP_BTN_UP);
            input_map_set_binding(m, 16 + 10, BIND_TYPE_GAMEPAD, 1, GP_BTN_X);
            input_map_set_binding(m, 16 + 11, BIND_TYPE_GAMEPAD, 1, GP_BTN_Y);
            input_map_set_binding(m, 16 + 14, BIND_TYPE_GAMEPAD, 1, GP_BTN_A);
            input_map_set_binding(m, 16 + 15, BIND_TYPE_GAMEPAD, 1, GP_BTN_B);
            break;

        case PRESET_ERGONOMIC:
            // Left hand cluster (Pads 1, 2, 5, 6 -> indices 0, 1, 4, 5)
            input_map_set_binding(m, 0, BIND_TYPE_GAMEPAD, 0, GP_BTN_LEFT);
            input_map_set_binding(m, 1, BIND_TYPE_GAMEPAD, 0, GP_BTN_DOWN);
            input_map_set_binding(m, 4, BIND_TYPE_GAMEPAD, 0, GP_BTN_UP);
            input_map_set_binding(m, 5, BIND_TYPE_GAMEPAD, 0, GP_BTN_RIGHT);

            // Right hand cluster (Pads 3, 4, 7, 8 -> indices 2, 3, 6, 7)
            input_map_set_binding(m, 2, BIND_TYPE_GAMEPAD, 0, GP_BTN_B);
            input_map_set_binding(m, 3, BIND_TYPE_GAMEPAD, 0, GP_BTN_A);
            input_map_set_binding(m, 6, BIND_TYPE_GAMEPAD, 0, GP_BTN_Y);
            input_map_set_binding(m, 7, BIND_TYPE_GAMEPAD, 0, GP_BTN_X);

            // Row 3 (Pads 9..12): Number keys 1, 2, 3, 4
            input_map_set_binding(m, 8, BIND_TYPE_KEY, 0, tic_key_1);
            input_map_set_binding(m, 9, BIND_TYPE_KEY, 0, tic_key_2);
            input_map_set_binding(m, 10, BIND_TYPE_KEY, 0, tic_key_3);
            input_map_set_binding(m, 11, BIND_TYPE_KEY, 0, tic_key_4);

            // Row 4 (Pads 13..16): Space, Return, Esc, Shift
            input_map_set_binding(m, 12, BIND_TYPE_KEY, 0, tic_key_space);
            input_map_set_binding(m, 13, BIND_TYPE_KEY, 0, tic_key_return);
            input_map_set_binding(m, 14, BIND_TYPE_KEY, 0, tic_key_escape);
            input_map_set_binding(m, 15, BIND_TYPE_KEY, 0, tic_key_shift);
            break;

        case PRESET_KEYBOARD:
            // WASD Navigation:
            input_map_set_binding(m, 4, BIND_TYPE_KEY, 0, tic_key_a);
            input_map_set_binding(m, 5, BIND_TYPE_KEY, 0, tic_key_s);
            input_map_set_binding(m, 6, BIND_TYPE_KEY, 0, tic_key_d);
            input_map_set_binding(m, 9, BIND_TYPE_KEY, 0, tic_key_w);

            // Action keys:
            input_map_set_binding(m, 10, BIND_TYPE_KEY, 0, tic_key_z);
            input_map_set_binding(m, 11, BIND_TYPE_KEY, 0, tic_key_x);
            input_map_set_binding(m, 14, BIND_TYPE_KEY, 0, tic_key_c);
            input_map_set_binding(m, 15, BIND_TYPE_KEY, 0, tic_key_space);

            input_map_set_binding(m, 0, BIND_TYPE_KEY, 0, tic_key_escape);
            input_map_set_binding(m, 3, BIND_TYPE_KEY, 0, tic_key_return);
            break;

        case PRESET_TWOPLAYER:
            // Player 1 on left columns (Pads 1, 2, 5, 6, 9, 10, 13, 14)
            input_map_set_binding(m, 4, BIND_TYPE_GAMEPAD, 0, GP_BTN_LEFT);
            input_map_set_binding(m, 5, BIND_TYPE_GAMEPAD, 0, GP_BTN_DOWN);
            input_map_set_binding(m, 8, BIND_TYPE_GAMEPAD, 0, GP_BTN_UP);
            input_map_set_binding(m, 9, BIND_TYPE_GAMEPAD, 0, GP_BTN_RIGHT);
            input_map_set_binding(m, 0, BIND_TYPE_GAMEPAD, 0, GP_BTN_A);
            input_map_set_binding(m, 1, BIND_TYPE_GAMEPAD, 0, GP_BTN_B);
            input_map_set_binding(m, 12, BIND_TYPE_GAMEPAD, 0, GP_BTN_X);
            input_map_set_binding(m, 13, BIND_TYPE_GAMEPAD, 0, GP_BTN_Y);

            // Player 2 on right columns (Pads 3, 4, 7, 8, 11, 12, 15, 16)
            input_map_set_binding(m, 6, BIND_TYPE_GAMEPAD, 1, GP_BTN_LEFT);
            input_map_set_binding(m, 7, BIND_TYPE_GAMEPAD, 1, GP_BTN_DOWN);
            input_map_set_binding(m, 10, BIND_TYPE_GAMEPAD, 1, GP_BTN_UP);
            input_map_set_binding(m, 11, BIND_TYPE_GAMEPAD, 1, GP_BTN_RIGHT);
            input_map_set_binding(m, 2, BIND_TYPE_GAMEPAD, 1, GP_BTN_A);
            input_map_set_binding(m, 3, BIND_TYPE_GAMEPAD, 1, GP_BTN_B);
            input_map_set_binding(m, 14, BIND_TYPE_GAMEPAD, 1, GP_BTN_X);
            input_map_set_binding(m, 15, BIND_TYPE_GAMEPAD, 1, GP_BTN_Y);
            break;

        default:
            break;
    }
}

const char* input_map_preset_name(InputPreset preset) {
    switch (preset) {
        case PRESET_ARCADE:     return "Standard Arcade";
        case PRESET_ERGONOMIC:  return "Ergonomic Split";
        case PRESET_KEYBOARD:   return "WASD Keyboard";
        case PRESET_TWOPLAYER:  return "2-Player Split";
        default:                return "Custom Profile";
    }
}

void input_map_init(struct input_map *m) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->active_bank = 0;
    input_map_apply_preset(m, PRESET_ARCADE);
}

void input_map_set_bank(struct input_map *m, int bank) {
    if (m) m->active_bank = (bank == 1) ? 1 : 0;
}

int input_map_get_bank(const struct input_map *m) {
    return m ? m->active_bank : 0;
}

void input_map_toggle_bank(struct input_map *m) {
    if (m) m->active_bank = (m->active_bank == 0) ? 1 : 0;
}

static void apply_gp_button(tic80_gamepads *gp, uint8_t player, uint8_t btn) {
    tic80_gamepad *p = (player == 0) ? &gp->first : &gp->second;
    switch (btn) {
        case GP_BTN_UP:    p->up = 1;    break;
        case GP_BTN_DOWN:  p->down = 1;  break;
        case GP_BTN_LEFT:  p->left = 1;  break;
        case GP_BTN_RIGHT: p->right = 1; break;
        case GP_BTN_A:     p->a = 1;     break;
        case GP_BTN_B:     p->b = 1;     break;
        case GP_BTN_X:     p->x = 1;     break;
        case GP_BTN_Y:     p->y = 1;     break;
        default: break;
    }
}

void input_map_translate(const struct input_map *m, const struct input *in, tic80_input *out_tic_input) {
    if (!m || !in || !out_tic_input) return;

    memset(out_tic_input, 0, sizeof(*out_tic_input));

    int base_idx = m->active_bank * 16;
    int key_count = 0;

    for (int p = 0; p < 16; p++) {
        if (!in->pad_down[p]) continue;

        const struct pad_binding *b = &m->pads[base_idx + p];
        switch (b->type) {
            case BIND_TYPE_GAMEPAD:
                apply_gp_button(&out_tic_input->gamepads, b->player, b->code);
                break;
            case BIND_TYPE_KEY:
                if (key_count < TIC80_KEY_BUFFER) {
                    out_tic_input->keyboard.keys[key_count++] = (tic_key)b->code;
                }
                break;
            case BIND_TYPE_MOUSE_BTN:
                if (b->code == 1) out_tic_input->mouse.left = 1;
                else if (b->code == 2) out_tic_input->mouse.right = 1;
                else if (b->code == 3) out_tic_input->mouse.middle = 1;
                break;
            default:
                break;
        }
    }

    // Physical button extras:
    // IN_MINUS -> Gamepad 1 X / Select fallback
    if (in->held & (1u << IN_MINUS)) {
        out_tic_input->gamepads.first.x = 1;
    }
    // IN_PLUS -> Gamepad 1 Y / Start fallback
    if (in->held & (1u << IN_PLUS)) {
        out_tic_input->gamepads.first.y = 1;
    }

    // Rotary encoder mouse scroll emulation:
    if (in->encoder != 0) {
        out_tic_input->mouse.scrolly = (int16_t)in->encoder;
    }
}

void input_map_get_pad_led(const struct input_map *m, const struct input *in, int pad, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (!r || !g || !b) return;
    if (!m || pad < 0 || pad >= 16) {
        *r = *g = *b = 0;
        return;
    }

    int base_idx = m->active_bank * 16;
    const struct pad_binding *binding = &m->pads[base_idx + pad];
    uint32_t col = binding->color;

    uint8_t br = (uint8_t)((col >> 16) & 0xFF);
    uint8_t bg = (uint8_t)((col >> 8) & 0xFF);
    uint8_t bb = (uint8_t)(col & 0xFF);

    if (in && in->pad_down[pad]) {
        // Flash bright white with velocity scaling
        int vel = in->pad_vel[pad];
        if (vel <= 0) vel = 127;
        uint8_t blend = (uint8_t)(vel * 2);
        *r = (uint8_t)(((int)br + (255 - br) * blend / 255));
        *g = (uint8_t)(((int)bg + (255 - bg) * blend / 255));
        *b = (uint8_t)(((int)bb + (255 - bb) * blend / 255));
    } else {
        *r = br;
        *g = bg;
        *b = bb;
    }
}
