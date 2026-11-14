//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// 'fastboot oem boot-target linux|android' sets which OS boots by default.
// 'fastboot oem boot-once linux|android|none' overrides it for the next
// boot only; 'none' cancels a pending override.
//
// Read them back with 'fastboot getvar kaeru-boot-target' and
// 'fastboot getvar kaeru-boot-once'.
//

#include <lib/boot/target.h>
#include <lib/debug.h>
#include <lib/fastboot.h>
#include <lib/string.h>

static void boot_target_cmd(const char* arg, int once) {
    char tok[16];
    char line[FASTBOOT_INFO_MAX + 1];
    enum boot_target t;
    const char* var = once ? "kaeru-boot-once" : "kaeru-boot-target";

    next_token(arg, tok, sizeof(tok));
    if (boot_target_parse(tok, &t) || (!once && t == BOOT_TARGET_NONE)) {
        fastboot_fail(once ? "Usage: oem boot-once linux|android|none"
                           : "Usage: oem boot-target linux|android");
        return;
    }

    if (once ? boot_once_set(t) : boot_target_set(t)) {
        fastboot_fail("Failed to write the environment");
        return;
    }

    npf_snprintf(line, sizeof(line), "%s: %s", var, boot_target_name(t));
    fastboot_info(line);
    fastboot_okay("");
}

static void cmd_boot_target(const char* arg, void* data, unsigned sz) {
    (void)data;
    (void)sz;
    boot_target_cmd(arg, 0);
}

static void cmd_boot_once(const char* arg, void* data, unsigned sz) {
    (void)data;
    (void)sz;
    boot_target_cmd(arg, 1);
}

FASTBOOT_CMD(boot_target, "oem boot-target", cmd_boot_target, 1);
FASTBOOT_CMD(boot_once, "oem boot-once", cmd_boot_once, 1);
