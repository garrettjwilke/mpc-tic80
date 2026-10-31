#include "config.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char s_exe_dir[CONFIG_PATH_MAX];

static void set_exe_dir(const char *path) {
    char buf[CONFIG_PATH_MAX];
    snprintf(buf, sizeof buf, "%s", path ? path : "");
    char *slash = strrchr(buf, '/');
    if (!buf[0] || !slash)
        snprintf(s_exe_dir, sizeof s_exe_dir, ".");
    else if (slash == buf)
        snprintf(s_exe_dir, sizeof s_exe_dir, "/");
    else {
        *slash = '\0';
        snprintf(s_exe_dir, sizeof s_exe_dir, "%s", buf);
    }
}

void config_set_exe_path(const char *argv0) {
    /* Set by the toolkit autorun launcher. The binary runs from /run. */
    const char *home = getenv("MPC_APP_DIR");
    if (home && home[0] && strlen(home) < sizeof s_exe_dir) {
        snprintf(s_exe_dir, sizeof s_exe_dir, "%s", home);
        return;
    }
    char buf[CONFIG_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0 && n < (ssize_t)sizeof buf - 1) {
        buf[n] = '\0';
        set_exe_dir(buf);
        return;
    }
    if (argv0 && argv0[0]) {
        set_exe_dir(argv0);
        return;
    }
    snprintf(s_exe_dir, sizeof s_exe_dir, ".");
}

int config_make_path(char *out, size_t n, const char *filename) {
    if (!out || n == 0 || !filename || !filename[0]) return -1;
    if (!s_exe_dir[0])
        snprintf(s_exe_dir, sizeof s_exe_dir, ".");
    int wrote;
    if (strcmp(s_exe_dir, "/") == 0)
        wrote = snprintf(out, n, "/%s", filename);
    else
        wrote = snprintf(out, n, "%s/%s", s_exe_dir, filename);
    if (wrote < 0 || (size_t)wrote >= n) return -1;
    return 0;
}

static int binding_file_name(const char *cart_path, char *out, size_t n) {
    const char *base = cart_path ? cart_path : "";
    const char *slash = strrchr(base, '/');
    if (slash && slash[1]) base = slash + 1;
    char stem[181];
    snprintf(stem, sizeof stem, "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem) *dot = '\0';
    if (!stem[0] || strcmp(stem, ".") == 0 || strcmp(stem, "..") == 0)
        snprintf(stem, sizeof stem, "cart");
    for (char *p = stem; *p; p++) {
        if (*p == '/' || *p == '\\') *p = '_';
    }
    int wrote = snprintf(out, n, "bindings-%s.json", stem);
    if (wrote < 0 || (size_t)wrote >= n) return -1;
    return 0;
}

void config_settings_defaults(struct app_settings *s) {
    if (!s) return;
    s->stretch = true;
    s->default_dir[0] = '\0';
}

static char *read_file(const char *path, long limit) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > limit || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

int config_load_settings(struct app_settings *s) {
    if (!s) return -1;
    config_settings_defaults(s);
    char path[CONFIG_PATH_MAX];
    if (config_make_path(path, sizeof path, "settings.json") != 0) return -1;
    char *buf = read_file(path, 8192);
    if (!buf) return -1;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return -1;

    cJSON *stretch = cJSON_GetObjectItem(root, "stretch");
    if (stretch && cJSON_IsBool(stretch))
        s->stretch = cJSON_IsTrue(stretch);

    cJSON *dir = cJSON_GetObjectItem(root, "default_dir");
    if (dir && cJSON_IsString(dir) && dir->valuestring)
        snprintf(s->default_dir, sizeof s->default_dir, "%s", dir->valuestring);

    cJSON_Delete(root);
    return 0;
}

int config_save_settings(const struct app_settings *s) {
    if (!s) return -1;
    char path[CONFIG_PATH_MAX];
    if (config_make_path(path, sizeof path, "settings.json") != 0) return -1;

    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON_AddBoolToObject(root, "stretch", s->stretch);
    cJSON_AddStringToObject(root, "default_dir", s->default_dir);
    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return -1;

    FILE *f = fopen(path, "w");
    if (!f) {
        free(json);
        return -1;
    }
    fputs(json, f);
    fclose(f);
    free(json);
    return 0;
}

int config_save_profile(const struct input_map *m, const char *path) {
    if (!m || !path || !path[0]) return -1;

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

    char *buf = read_file(path, 65536);
    if (!buf) return -1;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return -1;

    cJSON *preset_item = cJSON_GetObjectItem(root, "preset");
    if (preset_item && cJSON_IsNumber(preset_item))
        m->current_preset = (InputPreset)preset_item->valueint;

    cJSON *custom_item = cJSON_GetObjectItem(root, "is_custom");
    if (custom_item && cJSON_IsBool(custom_item))
        m->is_custom = cJSON_IsTrue(custom_item);

    cJSON *pads_arr = cJSON_GetObjectItem(root, "pads");
    if (pads_arr && cJSON_IsArray(pads_arr)) {
        int count = cJSON_GetArraySize(pads_arr);
        for (int i = 0; i < count; i++) {
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

            if (type_item && cJSON_IsNumber(type_item))
                m->pads[idx].type = (BindingType)type_item->valueint;
            if (player_item && cJSON_IsNumber(player_item))
                m->pads[idx].player = (uint8_t)player_item->valueint;
            if (code_item && cJSON_IsNumber(code_item))
                m->pads[idx].code = (uint8_t)code_item->valueint;
            if (color_item && cJSON_IsNumber(color_item))
                m->pads[idx].color = (uint32_t)color_item->valuedouble;
            else
                m->pads[idx].color = input_map_default_color_for(m->pads[idx].type, m->pads[idx].code);

            cJSON *label_item = cJSON_GetObjectItem(pad_obj, "label");
            if (label_item && cJSON_IsString(label_item))
                snprintf(m->pads[idx].label, sizeof(m->pads[idx].label), "%s", label_item->valuestring);
        }
    }

    cJSON_Delete(root);
    return 0;
}

int config_load_default(struct input_map *m) {
    char path[CONFIG_PATH_MAX];
    if (!m || config_make_path(path, sizeof path, "default_bindings.json") != 0) return -1;
    if (access(path, F_OK) != 0) return -1;
    return config_load_profile(m, path);
}

int config_save_default(const struct input_map *m) {
    char path[CONFIG_PATH_MAX];
    if (config_make_path(path, sizeof path, "default_bindings.json") != 0) return -1;
    return config_save_profile(m, path);
}

int config_load_for_cart(struct input_map *m, const char *cart_path) {
    if (!m) return -1;
    if (cart_path && cart_path[0]) {
        char name[220];
        char path[CONFIG_PATH_MAX];
        if (binding_file_name(cart_path, name, sizeof name) == 0 &&
            config_make_path(path, sizeof path, name) == 0 &&
            access(path, F_OK) == 0)
            return config_load_profile(m, path);
    }
    if (config_load_default(m) == 0) return 0;
    input_map_apply_preset(m, PRESET_ARCADE);
    return 0;
}

int config_save_for_cart(const struct input_map *m, const char *cart_path) {
    if (!m || !cart_path || !cart_path[0]) return -1;
    char name[220];
    char path[CONFIG_PATH_MAX];
    if (binding_file_name(cart_path, name, sizeof name) != 0) return -1;
    if (config_make_path(path, sizeof path, name) != 0) return -1;
    return config_save_profile(m, path);
}
