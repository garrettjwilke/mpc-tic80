#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "ui.h"
#include "input.h"
#include "widgets.h"
#include "filebrowser.h"
#include "input_map.h"
#include "tic_runner.h"

typedef enum {
    UI_MODE_GAMEPLAY = 0,
    UI_MODE_MENU,
    UI_MODE_REBIND,
    UI_MODE_FILEBROWSER,
    UI_MODE_KEYBOARD
} UiMode;

struct ui_overlay {
    UiMode mode;
    UiMode prev_mode;

    // File browser state
    struct ui_filebrowser fb;
    bool fb_initialized;

    // Virtual keyboard state
    struct ui_text_editor editor;
    char text_buf[128];
    bool editor_initialized;

    // Rebind state
    int rebind_selected_pad; // 0..31
    int rebind_category;     // 0=Gamepad P1, 1=Gamepad P2, 2=Common Keys, 3=Letters/Nums, 4=Mouse, 5=Clear
    int rebind_item_idx;
    bool rebind_picking_action;

    // Status / Message toast
    char toast_msg[64];
    int64_t toast_until_ms;

    // Scale the 240-wide game across the full 320-wide screen.
    bool stretch;
};

void ui_overlay_init(struct ui_overlay *o);

void ui_overlay_set_mode(struct ui_overlay *o, UiMode mode);
UiMode ui_overlay_get_mode(const struct ui_overlay *o);

void ui_overlay_show_toast(struct ui_overlay *o, const char *msg, int64_t now_ms, int duration_ms);

enum {
    UI_OVERLAY_INPUT = 1,    // soft keys and other gameplay button edges
    UI_OVERLAY_CHROME = 2,   // top bar, borders, status, toast
    UI_OVERLAY_VIEWPORT = 4  // TIC-80 framebuffer blit
};

// Draws the active mode. Gameplay honors draw_mask; other modes always paint.
// Returns 1 if the user requested exit, 0 otherwise.
int ui_overlay_draw(struct ui_overlay *o, struct ui *u, const struct input *in,
                    struct tic_runner *runner, struct input_map *map, unsigned draw_mask);
