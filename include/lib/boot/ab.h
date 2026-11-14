//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// AOSP A/B metadata (bootloader_control in misc). See lib/boot/ab.c.
//

#pragma once

#include <stdint.h>

struct ab_slot {
    uint8_t priority;        // 0-15, 0 means unbootable
    uint8_t tries_remaining; // 0-7
    uint8_t successful_boot;
};

struct ab_state {
    int nb_slot;
    struct ab_slot slot[4];
    int current; // The highest priority slot, the one being booted
};

// Reads and checks bootloader_control from the misc partition. Returns 0,
// or -1 if there is none or it is not valid (magic, version, CRC).
int ab_read(struct ab_state* st);

// Parses the 32 bytes of bootloader_control. Exposed for host tests.
int ab_parse(const uint8_t* p, struct ab_state* st);
