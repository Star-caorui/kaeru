//
// SPDX-FileCopyrightText: 2025 Shomy <shomy@shomy.is-a.dev>
//                         2025 Roger Ortiz <roger@r0rt1z2.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The bootloader's heap, for stage 2. Same wrappers as stage1/memory.c,
// which only stage 1 links.
//

#include <stage1/memory.h>

void* malloc(size_t size) {
    return ((void* (*)(size_t))(CONFIG_MALLOC_ADDRESS | 1))(size);
}

void free(void* ptr) {
    ((void (*)(void*))(CONFIG_FREE_ADDRESS | 1))(ptr);
}
