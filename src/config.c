#include "config.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libgen.h>

static void ensure_dir_for_file(const char *filepath) {
    if (!filepath) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", filepath);
    char *dir = dirname(tmp);
    if (dir && strcmp(dir, ".") != 0 && strcmp(dir, "/") != 0) {
        mkdir(dir, 0755);
    }
}

int config_save_profile(const struct input_map *m, const char *path) {
    if (!m || !path || !path[0]) return -1;

    ensure_dir_for_file(path);

    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddNumberToObject(root, "preset", (double)m->current_preset);
    cJSON_AddBoolToObject(root, "is_custom", m->is_custom);

    cJSON *pads_arr = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "pads", pads_arr);

    for (int i = 0; i < INPUT_MAP_TOTAL_PADS; i++) {
        const struct pad_binding *b = &m->pads[i];
        cJSON *pad_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(pad_obj, "index", i);
        cJSON_AddNumberToObject(pad_obj, "type", b->type);
        cJSON_AddNumberToObject(pad_obj, "player", b->player);
        cJSON_AddNumberToObject(pad_obj, "code", b->code);
        cJSON_AddNumberToObject(pad_obj, "color", b->color);
        cJSON_AddStringToObject(pad_obj, "label", b->label);
        cJSON_AddItemToArray(pads_arr, pad_obj);
    }

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) return -1;

    FILE *f = fopen(path, "w");
    if (!f) {
        free(json_str);
        return -1;
    }

    fputs(json_str, f);
    fclose(f);
    free(json_str);
    return 0;
}

int config_load_profile(struct input_map *m, const char *path) {
    if (!m || !path || !path[0]) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 65536) {
        fclose(f);
        return -1;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }

    if (fread(buf, 1, sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    buf[sz] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return -1;

    cJSON *preset_item = cJSON_GetObjectItem(root, "preset");
    if (preset_item && cJSON_IsNumber(preset_item)) {
        m->current_preset = (InputPreset)preset_item->valueint;
    }

    cJSON *custom_item = cJSON_GetObjectItem(root, "is_custom");
    if (custom_item && cJSON_IsBool(custom_item)) {
        m->is_custom = cJSON_IsTrue(custom_item);
    }

    cJSON *pads_arr = cJSON_GetObjectItem(root, "pads");
    if (pads_arr && cJSON_IsArray(pads_arr)) {
        int n = cJSON_GetArraySize(pads_arr);
        for (int i = 0; i < n; i++) {
            cJSON *pad_obj = cJSON_GetArrayItem(pads_arr, i);
            if (!pad_obj) continue;

            cJSON *idx_item = cJSON_GetObjectItem(pad_obj, "index");
            if (!idx_item || !cJSON_IsNumber(idx_item)) continue;
            int idx = idx_item->valueint;
            if (idx < 0 || idx >= INPUT_MAP_TOTAL_PADS) continue;

            cJSON *type_item = cJSON_GetObjectItem(pad_obj, "type");
            cJSON *player_item = cJSON_GetObjectItem(pad_obj, "player");
            cJSON *code_item = cJSON_GetObjectItem(pad_obj, "code");
            cJSON *color_item = cJSON_GetObjectItem(pad_obj, "color");

            if (type_item && cJSON_IsNumber(type_item)) {
                m->pads[idx].type = (BindingType)type_item->valueint;
            }
            if (player_item && cJSON_IsNumber(player_item)) {
                m->pads[idx].player = (uint8_t)player_item->valueint;
            }
            if (code_item && cJSON_IsNumber(code_item)) {
                m->pads[idx].code = (uint8_t)code_item->valueint;
            }
            if (color_item && cJSON_IsNumber(color_item)) {
                m->pads[idx].color = (uint32_t)color_item->valuedouble;
            } else {
                m->pads[idx].color = input_map_default_color_for(m->pads[idx].type, m->pads[idx].code);
            }

            cJSON *label_item = cJSON_GetObjectItem(pad_obj, "label");
            if (label_item && cJSON_IsString(label_item)) {
                snprintf(m->pads[idx].label, sizeof(m->pads[idx].label), "%s", label_item->valuestring);
            }
        }
    }

    cJSON_Delete(root);
    return 0;
}

static const char* default_storage_root(void) {
    if (access("/media/MPC", F_OK) == 0) {
        return "/media/MPC";
    }
    if (access("/data", F_OK) == 0) {
        return "/data";
    }
    return ".";
}

int config_load_default(struct input_map *m) {
    char path[512];
    snprintf(path, sizeof(path), "%s/tic80/default_bindings.json", default_storage_root());
    if (access(path, F_OK) == 0) {
        return config_load_profile(m, path);
    }
    // Fallback to local
    if (access(".tic80_default.json", F_OK) == 0) {
        return config_load_profile(m, ".tic80_default.json");
    }
    return -1;
}

int config_save_default(const struct input_map *m) {
    char path[512];
    snprintf(path, sizeof(path), "%s/tic80/default_bindings.json", default_storage_root());
    if (config_save_profile(m, path) == 0) return 0;
    return config_save_profile(m, ".tic80_default.json");
}

int config_load_for_cart(struct input_map *m, const char *cart_path) {
    if (!m || !cart_path || !cart_path[0]) {
        return config_load_default(m);
    }

    char cfg_path[600];
    snprintf(cfg_path, sizeof(cfg_path), "%s.cfg", cart_path);
    if (access(cfg_path, F_OK) == 0) {
        return config_load_profile(m, cfg_path);
    }

    // Try profiles/<cartname>.json
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", cart_path);
    char *base = basename(tmp);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';

    snprintf(cfg_path, sizeof(cfg_path), "%s/tic80/profiles/%s.json", default_storage_root(), base);
    if (access(cfg_path, F_OK) == 0) {
        return config_load_profile(m, cfg_path);
    }

    // Fall back to default
    return config_load_default(m);
}

int config_save_for_cart(const struct input_map *m, const char *cart_path) {
    if (!m || !cart_path || !cart_path[0]) return -1;
    char cfg_path[600];
    snprintf(cfg_path, sizeof(cfg_path), "%s.cfg", cart_path);
    return config_save_profile(m, cfg_path);
}
