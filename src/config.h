#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "input_map.h"

#define CONFIG_PATH_MAX 1024
#define CONFIG_DIR_MAX 512

struct app_settings {
    bool stretch;
    char default_dir[CONFIG_DIR_MAX];
};

// Remember the executable directory. Later loads and saves use only that directory.
void config_set_exe_path(const char *argv0);
int config_make_path(char *out, size_t n, const char *filename);

void config_settings_defaults(struct app_settings *s);
int config_load_settings(struct app_settings *s);
int config_save_settings(const struct app_settings *s);

int config_save_profile(const struct input_map *m, const char *path);
int config_load_profile(struct input_map *m, const char *path);

// Cart bindings are bindings-{name}.json beside the executable.
// A cart with no file uses default_bindings.json, then the built-in preset.
int config_load_for_cart(struct input_map *m, const char *cart_path);
int config_save_for_cart(const struct input_map *m, const char *cart_path);

int config_load_default(struct input_map *m);
int config_save_default(const struct input_map *m);
