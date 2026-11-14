//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Which OS to boot: the persistent choice and a one-shot override, kept in
// the bootloader's environment and published as fastboot variables
// kaeru-boot-target and kaeru-boot-once. See lib/boot/target.c.
//

#pragma once

enum boot_target {
    BOOT_TARGET_NONE = 0,
    BOOT_TARGET_ANDROID,
    BOOT_TARGET_LINUX,
};

#define KAERU_ENV_BOOT_TARGET "kaeru_boot_target"
#define KAERU_ENV_BOOT_ONCE "kaeru_boot_once"
// The ext2/3/4 partition holding extlinux.conf; CONFIG_BOOT_LINUX_PARTITION
// when unset.
#define KAERU_ENV_LINUX_PARTITION "kaeru_linux_partition"

// "android", "linux" or "none". Returns 0, or -1 for anything else.
int boot_target_parse(const char* s, enum boot_target* out);
const char* boot_target_name(enum boot_target t);

// The persistent target: android unless set to linux.
enum boot_target boot_target_get(void);
// The one-shot override, BOOT_TARGET_NONE when not set.
enum boot_target boot_once_get(void);

// Write the environment, read it back and refresh the fastboot variables.
// boot_target_set() takes android or linux; boot_once_set() also takes
// none, which clears the override. Return 0, or -1 if the write did not
// stick.
int boot_target_set(enum boot_target t);
int boot_once_set(enum boot_target t);

// Publishes the fastboot variables. Runs at kaeru's late init, after the
// bootloader has loaded its environment.
void boot_target_init(void);
