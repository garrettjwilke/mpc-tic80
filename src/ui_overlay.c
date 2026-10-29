#include "ui_overlay.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

static struct ui_list_state s_file_ls;
static struct ui_list_state s_game_ls;
static struct ui_list_state s_settings_ls;

static const char *CART_EXTENSIONS[] = {".tic", ".png", ".lua", NULL};

void ui_overlay_init(struct ui_overlay *o) {
    if (!o) return;
    memset(o, 0, sizeof(*o));
    o->mode = UI_MODE_GAMEPLAY;
    o->prev_mode = UI_MODE_GAMEPLAY;
    o->rebind_selected_pad = 4; // Default to Pad 5
    o->rebind_category = 0;
    o->rebind_item_idx = 0;
    o->stretch = true;
    o->stretch_default = true;
}

void ui_overlay_set_mode(struct ui_overlay *o, UiMode mode) {
    if (!o) return;
    if (o->mode != mode) {
        if (mode == UI_MODE_FILE_MENU) s_file_ls.cursor = 0;
        if (mode == UI_MODE_GAME_MENU) s_game_ls.cursor = 0;
        if (mode == UI_MODE_SETTINGS_MENU) s_settings_ls.cursor = 0;
    }
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

static const char *cart_title(const struct tic_runner *runner) {
    if (runner && runner->cart_title[0]) return runner->cart_title;
    return "TIC-80";
}

static int is_dir(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *path_leaf(const char *path) {
    if (!path || !path[0]) return "Not set";
    const char *slash = strrchr(path, '/');
    if (slash && slash[1]) return slash + 1;
    return path;
}

static const char *browser_start(const struct ui_overlay *o) {
    if (is_dir(o->default_dir)) return o->default_dir;
    if (is_dir("/media/MPC")) return "/media/MPC";
    return ".";
}

static int save_settings(struct ui_overlay *o, struct ui *u) {
    struct app_settings s;
    config_settings_defaults(&s);
    s.stretch = o->stretch_default;
    snprintf(s.default_dir, sizeof s.default_dir, "%s", o->default_dir);
    if (config_save_settings(&s) != 0) {
        ui_overlay_show_toast(o, "Could not save settings", u->now_ms, 1600);
        return -1;
    }
    return 0;
}

static void load_cart(struct ui_overlay *o, struct tic_runner *runner, struct input_map *map, const char *path) {
    if (runner) tic_runner_load_file(runner, path);
    if (map) config_load_for_cart(map, path);
    o->stretch = o->stretch_default;
}

static void begin_browser(struct ui_overlay *o, UiMode mode) {
    o->fb_initialized = false;
    ui_overlay_set_mode(o, mode);
}

static void draw_top_bar(struct ui *u, const char *b1_lbl, const char *b2_lbl, const char *b3_lbl,
                         const char *center_title) {
    ui_box(u, 0, 0, UI_W, 22, (struct ui_paint){
        .fill_set = 1, .fill = 0x141820,
        .border_set = 0
    });
    ui_box(u, 0, 22, UI_W, 2, (struct ui_paint){
        .fill_set = 1, .fill = 0x2A3244,
        .border_set = 0
    });

    ui_box(u, 8, 2, 74, 18, (struct ui_paint){
        .fill_set = 1, .fill = 0x222834,
        .border_set = 1, .border = 0x3E485C, .border_px = 1
    });
    ui_text(u, 14, 5, b1_lbl, 0x00E5FF, 1);

    ui_box(u, 88, 2, 74, 18, (struct ui_paint){
        .fill_set = 1, .fill = 0x222834,
        .border_set = 1, .border = 0x3E485C, .border_px = 1
    });
    ui_text(u, 94, 5, b2_lbl, 0xFFD600, 1);

    ui_box(u, 238, 2, 74, 18, (struct ui_paint){
        .fill_set = 1, .fill = 0x222834,
        .border_set = 1, .border = 0x3E485C, .border_px = 1
    });
    ui_text(u, 246, 5, b3_lbl, 0x00E676, 1);

    if (center_title && center_title[0])
        ui_text(u, 170, 5, center_title, 0xE8ECF0, 1);
}

static void draw_app_button(struct ui *u, int x, const char *label, uint32_t color, int active) {
    ui_box(u, x, 2, 74, 18, (struct ui_paint){
        .fill_set = 1,
        .fill = active ? 0x2C384C : 0x222834,
        .border_set = 1,
        .border = active ? color : 0x3E485C,
        .border_px = active ? 2 : 1
    });
    int tw = (int)strlen(label) * 6;
    int tx = x + (74 - tw) / 2;
    ui_text(u, tx, 5, label, color, 1);
}

static void draw_app_bar(struct ui *u, int active, const char *title) {
    ui_box(u, 0, 0, UI_W, 22, (struct ui_paint){
        .fill_set = 1, .fill = 0x141820, .border_set = 0
    });
    ui_box(u, 0, 22, UI_W, 2, (struct ui_paint){
        .fill_set = 1, .fill = 0x2A3244, .border_set = 0
    });
    if (title && title[0]) {
        char gap[12];
        snprintf(gap, sizeof gap, "%.11s", title);
        ui_text(u, 166, 5, gap, 0xE8ECF0, 1);
    }
    draw_app_button(u, 8, "FILE", 0x00E5FF, active == 0);
    draw_app_button(u, 88, "GAME", 0xFFD600, active == 1);
    draw_app_button(u, 238, "SETTINGS", 0x00E676, active == 2);
}

static void draw_toast(struct ui *u, struct ui_overlay *o, int y) {
    if (o->toast_msg[0] && u->now_ms < o->toast_until_ms) {
        int w = 220, h = 24;
        int x = (UI_W - w) / 2;
        ui_box(u, x, y, w, h, (struct ui_paint){
            .fill_set = 1, .fill = 0x1A2230,
            .border_set = 1, .border = 0x00E5FF, .border_px = 2
        });
        ui_text(u, x + 10, y + 6, o->toast_msg, 0xFFFFFF, 1);
    }
}

static void draw_picture(struct ui_overlay *o, struct ui *u, struct tic_runner *runner) {
    ui_box(u, 0, 0, UI_W, UI_H, (struct ui_paint){
        .fill_set = 1, .fill = 0x080A0E, .border_set = 0
    });
    int vx, vy, vw, vh;
    tic_runner_viewport(o->stretch, &vx, &vy, &vw, &vh);
    if (runner && runner->is_loaded)
        tic_runner_render(runner, u, o->stretch);
    else
        ui_box(u, vx, vy, vw, vh, (struct ui_paint){
            .fill_set = 1, .fill = 0x080A0E, .border_set = 0
        });
}

static int handle_app_buttons(struct ui_overlay *o, const struct input *in) {
    if (in->down & (1u << IN_B1)) {
        ui_overlay_set_mode(o, o->mode == UI_MODE_FILE_MENU ? UI_MODE_GAMEPLAY : UI_MODE_FILE_MENU);
        return 1;
    }
    if (in->down & (1u << IN_B2)) {
        ui_overlay_set_mode(o, o->mode == UI_MODE_GAME_MENU ? UI_MODE_GAMEPLAY : UI_MODE_GAME_MENU);
        return 1;
    }
    if (in->down & (1u << IN_B3)) {
        ui_overlay_set_mode(o, o->mode == UI_MODE_SETTINGS_MENU ? UI_MODE_GAMEPLAY : UI_MODE_SETTINGS_MENU);
        return 1;
    }
    return 0;
}

static struct ui_list_style menu_style(void) {
    struct ui_list_style st = {
        .text = 0xD0D8E8,
        .highlight = 0x00E5FF,
        .row_h = 22,
        .highlight_px = 2,
        .current_fill_set = 1,
        .current_fill = 0x1E2638
    };
    return st;
}

static void menu_panel(int which, int rows, int *x, int *y, int *w, int *h) {
    *y = 28;
    *h = 8 + rows * 22;
    if (which == 0) {
        *x = 8;
        *w = 148;
    } else if (which == 1) {
        *x = 88;
        *w = 176;
    } else {
        *w = 220;
        *x = UI_W - 8 - *w;
    }
}

static int paint_rows(struct ui_overlay *o, struct ui *u, const struct input *in,
                      struct tic_runner *runner, int which,
                      const char **rows, int n, struct ui_list_state *ls) {
    draw_picture(o, u, runner);
    int x, y, w, h;
    menu_panel(which, n, &x, &y, &w, &h);
    ui_box(u, x, y, w, h, (struct ui_paint){
        .fill_set = 1, .fill = 0x10141E,
        .border_set = 1, .border = 0x2A3448, .border_px = 1
    });
    int chosen = ui_list(u, x + 4, y + 4, w - 8, n * 22, rows, n, menu_style(), in, ls);
    draw_app_bar(u, which, cart_title(runner));
    draw_toast(u, o, 180);
    return chosen;
}

// -------------------------------------------------------------------------
// GAMEPLAY MODE
// -------------------------------------------------------------------------
static int draw_gameplay(struct ui_overlay *o, struct ui *u, const struct input *in,
                         struct tic_runner *runner, struct input_map *map, unsigned mask) {
    if ((mask & UI_OVERLAY_INPUT) && handle_app_buttons(o, in))
        return 0;

    int vx, vy, vw, vh;
    tic_runner_viewport(o->stretch, &vx, &vy, &vw, &vh);

    if (mask & UI_OVERLAY_CHROME) {
        ui_box(u, 0, 24, vx, UI_H - 24, (struct ui_paint){
            .fill_set = 1, .fill = 0x080A0E, .border_set = 0
        });
        ui_box(u, vx + vw, 24, UI_W - (vx + vw), UI_H - 24, (struct ui_paint){
            .fill_set = 1, .fill = 0x080A0E, .border_set = 0
        });
        ui_box(u, vx, vy + vh, vw, UI_H - (vy + vh), (struct ui_paint){
            .fill_set = 1, .fill = 0x080A0E, .border_set = 0
        });

        int bank = input_map_get_bank(map);
        char status_str[64];
        snprintf(status_str, sizeof(status_str), "[BANK %c] %s", bank == 0 ? 'A' : 'B',
                 runner && runner->cart_title[0] ? runner->cart_title : "");
        ui_text(u, vx + 4, vy + vh + 8, status_str, 0x687890, 1);
        draw_app_bar(u, -1, cart_title(runner));
    }

    if (mask & UI_OVERLAY_VIEWPORT) {
        if (runner && runner->is_loaded)
            tic_runner_render(runner, u, o->stretch);
        else
            ui_box(u, vx, vy, vw, vh, (struct ui_paint){
                .fill_set = 1, .fill = 0x080A0E, .border_set = 0
            });
    }

    int toast_y = 180;
    bool toast_on_game = toast_y < vy + vh;
    if ((mask & UI_OVERLAY_CHROME) || (toast_on_game && (mask & UI_OVERLAY_VIEWPORT)))
        draw_toast(u, o, toast_y);
    return 0;
}

// -------------------------------------------------------------------------
// FILE / GAME / SETTINGS
// -------------------------------------------------------------------------
static int paint_file_menu(struct ui_overlay *o, struct ui *u, const struct input *in,
                           struct tic_runner *runner) {
    const char *rows[] = {"Load Cart", "Reset Cart", "Quit"};
    int chosen = paint_rows(o, u, in, runner, 0, rows, 3, &s_file_ls);
    if (chosen == 0) {
        begin_browser(o, UI_MODE_FILEBROWSER);
    } else if (chosen == 1) {
        if (runner) tic_runner_reset(runner);
        ui_overlay_show_toast(o, "Cartridge Reset", u->now_ms, 1500);
        ui_overlay_set_mode(o, UI_MODE_GAMEPLAY);
    } else if (chosen == 2) {
        return 1;
    }
    return 0;
}

static int paint_game_menu(struct ui_overlay *o, struct ui *u, const struct input *in,
                           struct tic_runner *runner) {
    char stretch_row[32];
    snprintf(stretch_row, sizeof stretch_row, "Stretch: %s", o->stretch ? "On" : "Off");
    const char *rows[] = {"Rebind Controls", stretch_row, "Keyboard"};
    int chosen = paint_rows(o, u, in, runner, 1, rows, 3, &s_game_ls);
    if (chosen == 0) {
        ui_overlay_set_mode(o, UI_MODE_REBIND);
    } else if (chosen == 1) {
        o->stretch = !o->stretch;
        ui_overlay_show_toast(o, o->stretch ? "Stretch On" : "Stretch Off", u->now_ms, 1200);
    } else if (chosen == 2) {
        ui_overlay_set_mode(o, UI_MODE_KEYBOARD);
    }
    return 0;
}

static int paint_settings_menu(struct ui_overlay *o, struct ui *u, const struct input *in,
                               struct tic_runner *runner) {
    char stretch_row[40];
    char folder_row[40];
    snprintf(stretch_row, sizeof stretch_row, "Start Stretched: %s", o->stretch_default ? "On" : "Off");
    snprintf(folder_row, sizeof folder_row, "Folder: %.22s",
             o->default_dir[0] ? path_leaf(o->default_dir) : "Not set");
    const char *rows[3];
    int n = 0;
    rows[n++] = stretch_row;
    rows[n++] = folder_row;
    if (o->default_dir[0]) rows[n++] = "Clear Folder";

    int chosen = paint_rows(o, u, in, runner, 2, rows, n, &s_settings_ls);
    if (chosen == 0) {
        o->stretch_default = !o->stretch_default;
        o->stretch = o->stretch_default;
        if (save_settings(o, u) == 0)
            ui_overlay_show_toast(o, o->stretch_default ? "Start Stretched: On" : "Start Stretched: Off",
                                  u->now_ms, 1200);
    } else if (chosen == 1) {
        begin_browser(o, UI_MODE_DIRPICKER);
    } else if (chosen == 2) {
        o->default_dir[0] = '\0';
        if (save_settings(o, u) == 0)
            ui_overlay_show_toast(o, "Folder Cleared", u->now_ms, 1200);
    }
    return 0;
}

static struct input quiet_press(const struct input *in) {
    struct input rest = *in;
    rest.encoder_press = 0;
    return rest;
}

static int draw_file_menu(struct ui_overlay *o, struct ui *u, const struct input *in,
                          struct tic_runner *runner, struct input_map *map) {
    (void)map;
    if (handle_app_buttons(o, in)) {
        struct input rest = quiet_press(in);
        if (o->mode == UI_MODE_GAME_MENU) return paint_game_menu(o, u, &rest, runner);
        if (o->mode == UI_MODE_SETTINGS_MENU) return paint_settings_menu(o, u, &rest, runner);
        return 0;
    }
    return paint_file_menu(o, u, in, runner);
}

static int draw_game_menu(struct ui_overlay *o, struct ui *u, const struct input *in,
                          struct tic_runner *runner, struct input_map *map) {
    (void)map;
    if (handle_app_buttons(o, in)) {
        struct input rest = quiet_press(in);
        if (o->mode == UI_MODE_FILE_MENU) return paint_file_menu(o, u, &rest, runner);
        if (o->mode == UI_MODE_SETTINGS_MENU) return paint_settings_menu(o, u, &rest, runner);
        return 0;
    }
    return paint_game_menu(o, u, in, runner);
}

static int draw_settings_menu(struct ui_overlay *o, struct ui *u, const struct input *in,
                              struct tic_runner *runner, struct input_map *map) {
    (void)map;
    if (handle_app_buttons(o, in)) {
        struct input rest = quiet_press(in);
        if (o->mode == UI_MODE_FILE_MENU) return paint_file_menu(o, u, &rest, runner);
        if (o->mode == UI_MODE_GAME_MENU) return paint_game_menu(o, u, &rest, runner);
        return 0;
    }
    return paint_settings_menu(o, u, in, runner);
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
        int saved;
        if (runner && runner->cart_path[0])
            saved = config_save_for_cart(map, runner->cart_path);
        else
            saved = config_save_default(map);
        if (saved == 0)
            ui_overlay_show_toast(o, runner && runner->cart_path[0] ? "Saved for this Cart!" : "Saved as Default!",
                                  u->now_ms, 2000);
        else
            ui_overlay_show_toast(o, "Could not save bindings", u->now_ms, 2000);
    }

    draw_top_bar(u, "B1:DONE", "B2:PRESET", "B3:SAVE", "REBIND PADS");

    for (int p = 0; p < 16; p++) {
        if (in->pad_hit[p] >= 0)
            o->rebind_selected_pad = (input_map_get_bank(map) * 16) + p;
    }

    if (in->encoder != 0) {
        if (in->held & (1u << IN_SHIFT))
            o->rebind_selected_pad = (o->rebind_selected_pad + in->encoder + INPUT_MAP_TOTAL_PADS) % INPUT_MAP_TOTAL_PADS;
        else {
            o->rebind_item_idx += in->encoder;
            if (o->rebind_item_idx < 0) o->rebind_item_idx = 0;
        }
    }

    if (in->down & (1u << IN_MINUS)) {
        o->rebind_category = (o->rebind_category - 1 + NUM_CATEGORIES) % NUM_CATEGORIES;
        o->rebind_item_idx = 0;
    }
    if (in->down & (1u << IN_PLUS)) {
        o->rebind_category = (o->rebind_category + 1) % NUM_CATEGORIES;
        o->rebind_item_idx = 0;
    }

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
            ui_box(u, px, py, pad_w, pad_h, (struct ui_paint){
                .fill_set = 1, .fill = b->color,
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

    if (in->encoder_press) {
        int sel = o->rebind_selected_pad;
        switch (o->rebind_category) {
            case 0:
                input_map_set_binding(map, sel, BIND_TYPE_GAMEPAD, 0, (uint8_t)(o->rebind_item_idx % GP_BTN_COUNT));
                break;
            case 1:
                input_map_set_binding(map, sel, BIND_TYPE_GAMEPAD, 1, (uint8_t)(o->rebind_item_idx % GP_BTN_COUNT));
                break;
            case 2:
                input_map_set_binding(map, sel, BIND_TYPE_KEY, 0, COMMON_KEYS[o->rebind_item_idx % NUM_COMMON_KEYS]);
                break;
            case 3:
                input_map_set_binding(map, sel, BIND_TYPE_KEY, 0, (uint8_t)(tic_key_0 + (o->rebind_item_idx % 10)));
                break;
            case 4:
                input_map_set_binding(map, sel, BIND_TYPE_KEY, 0, (uint8_t)(tic_key_a + (o->rebind_item_idx % 26)));
                break;
            case 5:
                input_map_set_binding(map, sel, BIND_TYPE_MOUSE_BTN, 0, (uint8_t)(1 + (o->rebind_item_idx % 3)));
                break;
            case 6:
                input_map_clear_binding(map, sel);
                break;
            default: break;
        }
        ui_overlay_show_toast(o, "Assigned!", u->now_ms, 1000);
    }

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

    draw_toast(u, o, 180);
    return 0;
}

// -------------------------------------------------------------------------
// FILE BROWSER
// -------------------------------------------------------------------------
static int draw_browser(struct ui_overlay *o, struct ui *u, const struct input *in,
                        struct tic_runner *runner, struct input_map *map, int picking_dir) {
    if (!o->fb_initialized) {
        const char *start = browser_start(o);
        struct ui_filebrowser_config cfg = {
            .initial_path = start,
            .internal_path = "/data",
            .external_path = is_dir("/media/MPC") ? "/media/MPC" : NULL,
            .title = picking_dir ? "DEFAULT FOLDER" : "SELECT CARTRIDGE",
            .action_label = picking_dir ? "SET" : "LOAD",
            .options = picking_dir
                ? (UI_FB_OPT_DIRS_ONLY | UI_FB_OPT_SELECT_DIR | UI_FB_OPT_HIDE_DOTFILES | UI_FB_OPT_BACK)
                : (UI_FB_PRESET_DEFAULT | UI_FB_OPT_BACK),
            .extensions = picking_dir ? NULL : CART_EXTENSIONS,
        };
        ui_filebrowser_init(&o->fb, &cfg);
        o->fb_initialized = true;
    }

    struct ui_filebrowser_style st = ui_filebrowser_style_default();
    int status = ui_filebrowser(u, 0, 0, UI_W, UI_H, &o->fb, st, in);

    if (status == UI_FB_SELECT) {
        const char *sel = o->fb.selected_path;
        o->fb_initialized = false;
        if (picking_dir) {
            if (sel && is_dir(sel)) {
                snprintf(o->default_dir, sizeof o->default_dir, "%s", sel);
                size_t n = strlen(o->default_dir);
                while (n > 1 && o->default_dir[n - 1] == '/')
                    o->default_dir[--n] = '\0';
                if (save_settings(o, u) == 0)
                    ui_overlay_show_toast(o, "Folder Set", u->now_ms, 1500);
            } else {
                ui_overlay_show_toast(o, "Not a folder", u->now_ms, 1500);
            }
            ui_overlay_set_mode(o, UI_MODE_SETTINGS_MENU);
        } else {
            if (sel && sel[0]) {
                load_cart(o, runner, map, sel);
                ui_overlay_show_toast(o, "Cartridge Loaded!", u->now_ms, 2000);
            }
            ui_overlay_set_mode(o, UI_MODE_GAMEPLAY);
        }
    } else if (status == UI_FB_CANCEL) {
        o->fb_initialized = false;
        ui_overlay_set_mode(o, picking_dir ? UI_MODE_SETTINGS_MENU : UI_MODE_FILE_MENU);
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
                    struct tic_runner *runner, struct input_map *map, unsigned draw_mask) {
    if (!o || !u || !in) return 0;

    switch (o->mode) {
        case UI_MODE_GAMEPLAY:
            return draw_gameplay(o, u, in, runner, map, draw_mask);
        case UI_MODE_FILE_MENU:
            return draw_file_menu(o, u, in, runner, map);
        case UI_MODE_GAME_MENU:
            return draw_game_menu(o, u, in, runner, map);
        case UI_MODE_SETTINGS_MENU:
            return draw_settings_menu(o, u, in, runner, map);
        case UI_MODE_REBIND:
            return draw_rebind(o, u, in, runner, map);
        case UI_MODE_FILEBROWSER:
            return draw_browser(o, u, in, runner, map, 0);
        case UI_MODE_DIRPICKER:
            return draw_browser(o, u, in, runner, map, 1);
        case UI_MODE_KEYBOARD:
            return draw_keyboard(o, u, in, runner, map);
        default:
            return draw_gameplay(o, u, in, runner, map, draw_mask);
    }
}
