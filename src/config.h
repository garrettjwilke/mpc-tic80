#pragma once
#include <stdbool.h>
#include "input_map.h"

int config_save_profile(const struct input_map *m, const char *path);
int config_load_profile(struct input_map *m, const char *path);

// Auto-loads game-specific profile if it exists, otherwise loads global default
int config_load_for_cart(struct input_map *m, const char *cart_path);
int config_save_for_cart(const struct input_map *m, const char *cart_path);

int config_load_default(struct input_map *m);
int config_save_default(const struct input_map *m);
