//
// SPDX-FileCopyrightText: 2026 Roger Ortiz <roger@r0rt1z2.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//

#pragma once

#include <stdint.h>

#define OPTIONAL_INIT(fn) \
    do { if (fn) fn(); } while (0)

/* these are weak no-ops by default, overridden by the
   real implementation when the subsystem is compiled in */
extern uint32_t kaeru_reloc_delta;

void __attribute__((weak)) sej_init(void);
void __attribute__((weak)) storage_init(void);
void __attribute__((weak)) framebuffer_init(void);
void __attribute__((weak)) boot_target_init(void);

// Not weak: nothing else references lib/boot/linux_boot.o, so a weak
// reference would leave it out of the link.
void linux_boot_init(void);
