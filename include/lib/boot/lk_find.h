//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Finding LK functions by the strings they use. See lib/boot/lk_find.c.
//

#pragma once

#include <stdint.h>

// LK's code as it sits in memory: size bytes at p, linked at base.
struct lk_code {
    const uint8_t* p;
    uint32_t base;
    uint32_t size;
};

// The Thumb function that refers to the string str, or 0. The string is
// matched from its start; with prefix it may go on after str, otherwise
// it must end there. Every reference has to lead to the same function.
uint32_t lk_find_function(const struct lk_code* lk, const char* str, int prefix);

// Whether addr looks like the start of a Thumb function: it saves LR
// (push {..., lr} or stmdb sp!, {..., lr}) and some bl calls it.
int lk_is_function_start(const struct lk_code* lk, uint32_t addr);

#ifndef LK_FIND_HOST_TEST
// LK in memory, LK_START to LK_END.
void lk_code_running(struct lk_code* lk);
#endif
