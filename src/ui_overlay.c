#include "ui_overlay.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static const char *CATEGORY_NAMES[] = {
    "Gamepad P1", "Gamepad P2", "Common Keys", "Numbers", "Letters", "Mouse", "Clear"
};
#define NUM_CATEGORIES 7

static const uint8_t COMMON_KEYS[] = {
    tic_key_z, tic_key_x, tic_key_c, tic_key_space, tic_key_return,
    tic_key_escape, tic_key_up, tic_key_down, tic_key_left, tic_key_right,
    tic_key_shift, tic_key_tab
};
#define NUM_COMMON_KEYS 12

static struct ui_list_state s_menu_ls = {0};

void ui_overlay_init(struct ui_overlay *o) {
    if (!o) return;
    memset(o, 0, sizeof(*o));
    o->mode = UI_MODE_GAMEPLAY;
    o->prev_mode = UI_MODE_GAMEPLAY;
    o->rebind_selected_pad = 4; // Default to Pad 5
    o->rebind_category = 0;
    o->rebind_item_idx = 0;
}

void ui_overlay_set_mode(struct ui_overlay *o, UiMode mode) {
    if (!o) return;
    o->prev_mode = o->mode;
    o->mode = mode;
}

UiMode ui_overlay_get_mode(const struct ui_overlay *o) {
    return o ? o->mode : UI_MODE_GAMEPLAY;
}

void ui_overlay_show_toast(struct ui_overlay *o, const char *msg, int64_t now_ms, int duration_ms) {
    if (!o || !msg) return;
    snprintf(o->toast_msg, sizeof(o->toast_msg), "%s", msg);
    o->toast_until_ms = now_ms + ((duration_ms > 0) ? duration_ms : 2000);
}

static void draw_top_bar(struct ui *u, const char *b1_lbl, const char *b2_lbl, const char *b3_lbl,
                         const char *center_title) {
    // Top bar background (y = 0..21)
    ui_box(u, 0, 0, UI_W, 22, (struct ui_paint){
        .fill_set = 1, .fill = 0x141820,
        .border_set = 0
    });

    // Subtle divider line (y = 22..23)
    ui_box(u, 0, 22, UI_W, 2, (struct ui_paint){
        .fill_set = 1, .fill = 0x2A3244,
        .border_set = 0
    });

    // Soft key button markers matching physical B1, B2, B3 positions
    // B1: Left (x = 8, w = 74)
    ui_box(u, 8, 2, 74, 18, (struct ui_paint){
        .fill_set = 1, .fill = 0x222834,
        .border_set = 1, .border = 0x3E485C, .border_px = 1
    });
    ui_text(u, 14, 5, b1_lbl, 0x00E5FF, 1);

    // B2: Center-left (x = 88, w = 74)
    ui_box(u, 88, 2, 74, 18, (struct ui_paint){
        .fill_set = 1, .fill = 0x222834,
        .border_set = 1, .border = 0x3E485C, .border_px = 1
    });
    ui_text(u, 94, 5, b2_lbl, 0xFFD600, 1);

    // B3: Right (x = 238, w = 74)
    ui_box(u, 238, 2, 74, 18, (struct ui_paint){
        .fill_set = 1, .fill = 0x222834,
        .border_set = 1, .border = 0x3E485C, .border_px = 1
    });
    ui_text(u, 246, 5, b3_lbl, 0x00E676, 1);

    // Center Title / Status
    if (center_title && center_title[0]) {
        ui_text(u, 170, 5, center_title, 0xE8ECF0, 1);
    }
}

static void draw_toast(struct ui *u, struct ui_overlay *o) {
    if (o->toast_msg[0] && u->now_ms < o->toast_until_ms) {
        int w = 220, h = 24;
        int x = (UI_W - w) / 2;
        int y = 180;
        ui_box(u, x, y, w, h, (struct ui_paint){
            .fill_set = 1, .fill = 0x1A2230,
            .border_set = 1, .border = 0x00E5FF, .border_px = 2
        });
        ui_text(u, x + 10, y + 6, o->toast_msg, 0xFFFFFF, 1);
    }
}

// -------------------------------------------------------------------------
// GAMEPLAY MODE
// -------------------------------------------------------------------------
static int draw_gameplay(struct ui_overlay *o, struct ui *u, const struct input *in,
                         struct tic_runner *runner, struct input_map *map) {
    // Check Soft-key shortcuts
    if (in->down & (1u << IN_B1)) {
        ui_overlay_set_mode(o, UI_MODE_MENU);
        return 0;
    }
    if (in->down & (1u << IN_B2)) {
        ui_overlay_set_mode(o, UI_MODE_REBIND);
        return 0;
    }
    if (in->down & (1u << IN_B3)) {
        ui_overlay_set_mode(o, UI_MODE_FILEBROWSER);
        return 0;
    }

    // Top soft-keys bar
    draw_top_bar(u, "B1:MENU", "B2:REBIND", "B3:LOAD", runner ? runner->cart_title : "TIC-80");

    // Render TIC-80 screen inside viewport (x=40, y=24, w=240, h=136)
    if (runner) {
        tic_runner_render(runner, u);
    }

    // Framing around viewport: deep clean matte black
    // Left border: (0..39, y=24..239)
    ui_box(u, 0, 24, TIC_RUNNER_VP_X, UI_H - 24, (struct ui_paint){
        .fill_set = 1, .fill = 0x080A0E, .border_set = 0
    });
    // Right border: (280..319, y=24..239)
    ui_box(u, TIC_RUNNER_VP_X + TIC_RUNNER_VP_W, 24, UI_W - (TIC_RUNNER_VP_X + TIC_RUNNER_VP_W), UI_H - 24, (struct ui_paint){
        .fill_set = 1, .fill = 0x080A0E, .border_set = 0
    });
    // Bottom border: (40..279, y=160..239)
    ui_box(u, TIC_RUNNER_VP_X, TIC_RUNNER_VP_Y + TIC_RUNNER_VP_H, TIC_RUNNER_VP_W, UI_H - (TIC_RUNNER_VP_Y + TIC_RUNNER_VP_H), (struct ui_paint){
        .fill_set = 1, .fill = 0x080A0E, .border_set = 0
    });

    // Subtle single-line status below game (y = 168)
    int bank = input_map_get_bank(map);
    char status_str[64];
    snprintf(status_str, sizeof(status_str), "[BANK %c] %s", bank == 0 ? 'A' : 'B',
             runner ? runner->cart_title : "");
    ui_text(u, TIC_RUNNER_VP_X + 4, 168, status_str, 0x687890, 1);

    draw_toast(u, o);
    return 0;
}

// -------------------------------------------------------------------------
// REBIND MODAL
// -------------------------------------------------------------------------
static int draw_rebind(struct ui_overlay *o, struct ui *u, const struct input *in,
                       struct tic_runner *runner, struct input_map *map) {
    if (in->down & (1u << IN_B1)) {
        ui_overlay_set_mode(o, UI_MODE_GAMEPLAY);
        return 0;
    }
    if (in->down & (1u << IN_B2)) {
        InputPreset next_p = (InputPreset)((map->current_preset + 1) % PRESET_COUNT);
        input_map_apply_preset(map, next_p);
        ui_overlay_show_toast(o, input_map_preset_name(next_p), u->now_ms, 1500);
    }
    if (in->down & (1u << IN_B3)) {
        if (runner && runner->cart_path[0]) {
            config_save_for_cart(map, runner->cart_path);
            ui_overlay_show_toast(o, "Saved for this Cart!", u->now_ms, 2000);
        } else {
            config_save_default(map);
            ui_overlay_show_toast(o, "Saved as Default!", u->now_ms, 2000);
        }
    }

    draw_top_bar(u, "B1:DONE", "B2:PRESET", "B3:SAVE", "REBIND PADS");

    // Check physical pad hits to select pad directly
    for (int p = 0; p < 16; p++) {
        if (in->pad_hit[p] >= 0) {
            o->rebind_selected_pad = (input_map_get_bank(map) * 16) + p;
        }
    }

    // Encoder navigation
    if (in->encoder != 0) {
        if (in->held & (1u << IN_SHIFT)) {
            o->rebind_selected_pad = (o->rebind_selected_pad + in->encoder + INPUT_MAP_TOTAL_PADS) % INPUT_MAP_TOTAL_PADS;
        } else {
            o->rebind_item_idx += in->encoder;
            if (o->rebind_item_idx < 0) o->rebind_item_idx = 0;
        }
    }

    // Minus / Plus changes category
    if (in->down & (1u << IN_MINUS)) {
        o->rebind_category = (o->rebind_category - 1 + NUM_CATEGORIES) % NUM_CATEGORIES;
        o->rebind_item_idx = 0;
    }
    if (in->down & (1u << IN_PLUS)) {
        o->rebind_category = (o->rebind_category + 1) % NUM_CATEGORIES;
        o->rebind_item_idx = 0;
    }

    // Draw 4x4 Pad Matrix representation (matching physical pads: Pad 1 at bottom-left)
    int bank_offset = input_map_get_bank(map) * 16;
    int pad_w = 46, pad_h = 28, pad_gap = 4;
    int base_x = 60, base_y = 28;

    for (int row = 0; row < 4; row++) {
        int phy_row = 3 - row;
        for (int col = 0; col < 4; col++) {
            int pad_local = phy_row * 4 + col;
            int pad_global = bank_offset + pad_local;
            bool is_selected = (o->rebind_selected_pad == pad_global);

            int px = base_x + col * (pad_w + pad_gap);
            int py = base_y + row * (pad_h + pad_gap);

            const struct pad_binding *b = &map->pads[pad_global];
            uint32_t fill_col = b->color;

            ui_box(u, px, py, pad_w, pad_h, (struct ui_paint){
                .fill_set = 1, .fill = fill_col,
                .border_set = 1,
                .border = is_selected ? 0xFFFFFF : 0x303848,
                .border_px = is_selected ? 2 : 1
            });

            char num_str[8];
            snprintf(num_str, sizeof(num_str), "%d", pad_local + 1);
            ui_text(u, px + 2, py + 2, num_str, 0x000000, 1);
            ui_text(u, px + 14, py + 8, b->label, 0xFFFFFF, 1);
        }
    }

    // Encoder click assigns the selected item
    if (in->encoder_press) {
        int sel = o->rebind_selected_pad;
        switch (o->rebind_category) {
            case 0: // Gamepad P1
                input_map_set_binding(map, sel, BIND_TYPE_GAMEPAD, 0, (uint8_t)(o->rebind_item_idx % GP_BTN_COUNT));
                break;
            case 1: // Gamepad P2
                input_map_set_binding(map, sel, BIND_TYPE_GAMEPAD, 1, (uint8_t)(o->rebind_item_idx % GP_BTN_COUNT));
                break;
            case 2: // Common keys
                input_map_set_binding(map, sel, BIND_TYPE_KEY, 0, COMMON_KEYS[o->rebind_item_idx % NUM_COMMON_KEYS]);
                break;
            case 3: // Numbers
                input_map_set_binding(map, sel, BIND_TYPE_KEY, 0, (uint8_t)(tic_key_0 + (o->rebind_item_idx % 10)));
                break;
            case 4: // Letters
                input_map_set_binding(map, sel, BIND_TYPE_KEY, 0, (uint8_t)(tic_key_a + (o->rebind_item_idx % 26)));
                break;
            case 5: // Mouse
                input_map_set_binding(map, sel, BIND_TYPE_MOUSE_BTN, 0, (uint8_t)(1 + (o->rebind_item_idx % 3)));
                break;
            case 6: // Clear
                input_map_clear_binding(map, sel);
                break;
            default: break;
        }
        ui_overlay_show_toast(o, "Assigned!", u->now_ms, 1000);
    }

    // Lower panel: Current selection and Category browser (y = 156..236)
    ui_box(u, 20, 156, UI_W - 40, 80, (struct ui_paint){
        .fill_set = 1, .fill = 0x141824,
        .border_set = 1, .border = 0x2E384C, .border_px = 1
    });

    int cur_pad = o->rebind_selected_pad;
    char pad_info[64];
    snprintf(pad_info, sizeof(pad_info), "PAD %d (%s): [%s]",
             (cur_pad % 16) + 1, (cur_pad < 16) ? "Bank A" : "Bank B", map->pads[cur_pad].label);
    ui_text(u, 28, 162, pad_info, 0xFFD600, 1);

    char cat_str[64];
    snprintf(cat_str, sizeof(cat_str), "Cat [-/+]: <%s>", CATEGORY_NAMES[o->rebind_category]);
    ui_text(u, 28, 180, cat_str, 0x00E5FF, 1);

    char item_str[64];
    switch (o->rebind_category) {
        case 0:
        case 1:
            snprintf(item_str, sizeof(item_str), "Target [Turn Wheel]: %s",
                     input_map_gp_btn_name((uint8_t)(o->rebind_item_idx % GP_BTN_COUNT)));
            break;
        case 2:
            snprintf(item_str, sizeof(item_str), "Target [Turn Wheel]: %s",
                     input_map_key_name(COMMON_KEYS[o->rebind_item_idx % NUM_COMMON_KEYS]));
            break;
        case 3:
            snprintf(item_str, sizeof(item_str), "Target [Turn Wheel]: Number %d", o->rebind_item_idx % 10);
            break;
        case 4:
            snprintf(item_str, sizeof(item_str), "Target [Turn Wheel]: Key '%c'", (char)('A' + (o->rebind_item_idx % 26)));
            break;
        case 5:
            snprintf(item_str, sizeof(item_str), "Target [Turn Wheel]: Mouse Btn %d", 1 + (o->rebind_item_idx % 3));
            break;
        case 6:
            snprintf(item_str, sizeof(item_str), "Action [Click Wheel]: Unmap Pad");
            break;
        default:
            snprintf(item_str, sizeof(item_str), "Action: Click Wheel to Assign");
            break;
    }
    ui_text(u, 28, 198, item_str, 0xFFFFFF, 1);
    ui_text(u, 28, 216, "Press Wheel to Assign | B1: Done", 0x788898, 1);

    draw_toast(u, o);
    return 0;
}

// -------------------------------------------------------------------------
// PAUSE / SYSTEM MENU
// -------------------------------------------------------------------------
static const char *MENU_ROWS[] = {
    "1. Resume Game",
    "2. Reset Cartridge",
    "3. Rebind Controls",
    "4. Load Cartridge (Files)",
    "5. Virtual Keyboard",
    "6. Cycle Preset",
    "7. Save Default Bindings",
    "8. Quit to Boot Menu"
};
#define MENU_ROWS_COUNT 8

static int draw_menu(struct ui_overlay *o, struct ui *u, const struct input *in,
                     struct tic_runner *runner, struct input_map *map) {
    if (in->down & (1u << IN_B1)) {
        ui_overlay_set_mode(o, UI_MODE_GAMEPLAY);
        return 0;
    }
    if (in->down & (1u << IN_B2)) {
        ui_overlay_set_mode(o, UI_MODE_REBIND);
        return 0;
    }

    draw_top_bar(u, "B1:RESUME", "B2:REBIND", "B3:SELECT", "TIC-80 MENU");

    // Background box
    ui_box(u, 20, 30, UI_W - 40, UI_H - 40, (struct ui_paint){
        .fill_set = 1, .fill = 0x10141E,
        .border_set = 1, .border = 0x2A3448, .border_px = 1
    });

    struct ui_list_style st = {
        .text = 0xD0D8E8,
        .highlight = 0x00E5FF,
        .row_h = 22,
        .highlight_px = 2,
        .current_fill_set = 1,
        .current_fill = 0x1E2638
    };

    int chosen = ui_list(u, 26, 36, UI_W - 52, UI_H - 52, MENU_ROWS, MENU_ROWS_COUNT, st, in, &s_menu_ls);

    if (in->down & (1u << IN_B3)) {
        chosen = s_menu_ls.cursor;
    }

    if (chosen >= 0) {
        switch (chosen) {
            case 0: // Resume
                ui_overlay_set_mode(o, UI_MODE_GAMEPLAY);
                break;
            case 1: // Reset
                if (runner) tic_runner_reset(runner);
                ui_overlay_show_toast(o, "Cartridge Reset", u->now_ms, 1500);
                ui_overlay_set_mode(o, UI_MODE_GAMEPLAY);
                break;
            case 2: // Rebind
                ui_overlay_set_mode(o, UI_MODE_REBIND);
                break;
            case 3: // Files
                ui_overlay_set_mode(o, UI_MODE_FILEBROWSER);
                break;
            case 4: // Keyboard
                ui_overlay_set_mode(o, UI_MODE_KEYBOARD);
                break;
            case 5: // Cycle Preset
            {
                InputPreset next_p = (InputPreset)((map->current_preset + 1) % PRESET_COUNT);
                input_map_apply_preset(map, next_p);
                ui_overlay_show_toast(o, input_map_preset_name(next_p), u->now_ms, 1500);
                break;
            }
            case 6: // Save Default
                config_save_default(map);
                ui_overlay_show_toast(o, "Saved as Default!", u->now_ms, 1500);
                break;
            case 7: // Quit
                return 1;
            default: break;
        }
    }

    draw_toast(u, o);
    return 0;
}

// -------------------------------------------------------------------------
// FILE BROWSER MODAL
// -------------------------------------------------------------------------
static const char *CART_EXTENSIONS[] = {".tic", ".png", ".lua", NULL};

static int draw_filebrowser(struct ui_overlay *o, struct ui *u, const struct input *in,
                            struct tic_runner *runner, struct input_map *map) {
    if (!o->fb_initialized) {
        struct ui_filebrowser_config cfg = {
            .initial_path = (access("/media/MPC", F_OK) == 0) ? "/media/MPC" : ".",
            .internal_path = "/data",
            .external_path = (access("/media/MPC", F_OK) == 0) ? "/media/MPC" : NULL,
            .title = "SELECT CARTRIDGE",
            .action_label = "LOAD",
            .options = UI_FB_PRESET_DEFAULT,
            .extensions = CART_EXTENSIONS,
        };
        ui_filebrowser_init(&o->fb, &cfg);
        o->fb_initialized = true;
    }

    struct ui_filebrowser_style st = ui_filebrowser_style_default();
    int status = ui_filebrowser(u, 0, 0, UI_W, UI_H, &o->fb, st, in);

    if (status == UI_FB_SELECT) {
        const char *sel = o->fb.selected_path;
        if (sel && sel[0]) {
            if (runner) {
                tic_runner_load_file(runner, sel);
            }
            if (map) {
                config_load_for_cart(map, sel);
            }
            ui_overlay_show_toast(o, "Cartridge Loaded!", u->now_ms, 2000);
        }
        ui_overlay_set_mode(o, UI_MODE_GAMEPLAY);
    } else if (status == UI_FB_CANCEL) {
        ui_overlay_set_mode(o, UI_MODE_GAMEPLAY);
    }

    return 0;
}

// -------------------------------------------------------------------------
// VIRTUAL KEYBOARD MODAL
// -------------------------------------------------------------------------
static int draw_keyboard(struct ui_overlay *o, struct ui *u, const struct input *in,
                         struct tic_runner *runner, struct input_map *map) {
    (void)map;
    (void)runner;
    if (!o->editor_initialized) {
        memset(o->text_buf, 0, sizeof(o->text_buf));
        ui_text_editor_init(&o->editor, o->text_buf, sizeof(o->text_buf), "KEYBOARD INPUT");
        o->editor_initialized = true;
    }

    struct ui_text_editor_style st = ui_text_editor_style_default();
    int res = ui_text_editor(u, 0, 0, UI_W, UI_H, &o->editor, st, in);

    if (res == UI_TEXT_OK) {
        ui_overlay_show_toast(o, "Text Submitted", u->now_ms, 1500);
        o->editor_initialized = false;
        ui_overlay_set_mode(o, UI_MODE_GAMEPLAY);
    } else if (res == UI_TEXT_CANCEL) {
        o->editor_initialized = false;
        ui_overlay_set_mode(o, UI_MODE_GAMEPLAY);
    }

    return 0;
}

int ui_overlay_draw(struct ui_overlay *o, struct ui *u, const struct input *in,
                    struct tic_runner *runner, struct input_map *map) {
    if (!o || !u || !in) return 0;

    switch (o->mode) {
        case UI_MODE_GAMEPLAY:
            return draw_gameplay(o, u, in, runner, map);
        case UI_MODE_REBIND:
            return draw_rebind(o, u, in, runner, map);
        case UI_MODE_MENU:
            return draw_menu(o, u, in, runner, map);
        case UI_MODE_FILEBROWSER:
            return draw_filebrowser(o, u, in, runner, map);
        case UI_MODE_KEYBOARD:
            return draw_keyboard(o, u, in, runner, map);
        default:
            return draw_gameplay(o, u, in, runner, map);
    }
}
