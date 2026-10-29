#pragma once
#include <stddef.h>
#include "tic_runner.h"

#define SAVESTATE_SLOTS 16

struct savestate;

// What the overlay draws while SAMPLE SELECT is held.
struct save_hud {
    int slot;        // 0..15
    unsigned filled; // bit per slot
    int choosing;
};

struct savestate *savestate_create(void);
void savestate_destroy(struct savestate *s);

// Load the slot file for the cart that is currently in the runner.
// A different cart switches files. The same cart image is left alone.
void savestate_bind(struct savestate *s, struct tic_runner *r);

int savestate_slot(const struct savestate *s);
void savestate_set_slot(struct savestate *s, int slot);
int savestate_filled(const struct savestate *s, int slot);
unsigned savestate_filled_mask(const struct savestate *s);
void savestate_hud(const struct savestate *s, int choosing, struct save_hud *out);

// 0 on success. Save leaves the previous slot untouched when the cart
// cannot be snapshotted. Load fails when the slot is empty.
int savestate_save(struct savestate *s, struct tic_runner *r);
int savestate_load(struct savestate *s, struct tic_runner *r);
