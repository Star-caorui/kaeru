//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Boot target selection.
//
// Both values live in the bootloader's environment, which survives
// reboots. Anything unset or unreadable means "android": a broken setting
// must never keep the device from booting what it booted before kaeru.
//
// The fastboot variables are published once with pointers to the buffers
// below; the bootloader keeps the pointers, so updating the buffers in
// place is what makes 'getvar' report a value set later in the session.
//

#include <lib/boot/target.h>
#include <lib/environment.h>
#include <lib/fastboot.h>
#include <lib/string.h>

// Longest value is "android".
static char target_var[8];
static char once_var[8];

int boot_target_parse(const char* s, enum boot_target* out) {
    if (!strcmp(s, "android"))
        *out = BOOT_TARGET_ANDROID;
    else if (!strcmp(s, "linux"))
        *out = BOOT_TARGET_LINUX;
    else if (!strcmp(s, "none"))
        *out = BOOT_TARGET_NONE;
    else
        return -1;
    return 0;
}

const char* boot_target_name(enum boot_target t) {
    switch (t) {
        case BOOT_TARGET_ANDROID:
            return "android";
        case BOOT_TARGET_LINUX:
            return "linux";
        default:
            return "none";
    }
}

static enum boot_target env_target(const char* key, enum boot_target fallback) {
    char* v = get_env((char*)key);
    enum boot_target t;

    if (!v || boot_target_parse(v, &t))
        return fallback;
    return t;
}

enum boot_target boot_target_get(void) {
    enum boot_target t = env_target(KAERU_ENV_BOOT_TARGET, BOOT_TARGET_ANDROID);

    return t == BOOT_TARGET_NONE ? BOOT_TARGET_ANDROID : t;
}

enum boot_target boot_once_get(void) {
    return env_target(KAERU_ENV_BOOT_ONCE, BOOT_TARGET_NONE);
}

static void set_var(char* var, enum boot_target t) {
    const char* name = boot_target_name(t);
    size_t i = 0;

    for (; name[i] && i < sizeof(target_var) - 1; i++)
        var[i] = name[i];
    var[i] = '\0';
}

static void refresh(void) {
    set_var(target_var, boot_target_get());
    set_var(once_var, boot_once_get());
}

static int write_env(const char* key, enum boot_target t, enum boot_target (*get)(void)) {
    int ret = set_env((char*)key, (char*)boot_target_name(t));

    refresh();
    return ret < 0 || get() != t ? -1 : 0;
}

int boot_target_set(enum boot_target t) {
    if (t != BOOT_TARGET_ANDROID && t != BOOT_TARGET_LINUX)
        return -1;
    return write_env(KAERU_ENV_BOOT_TARGET, t, boot_target_get);
}

int boot_once_set(enum boot_target t) {
    return write_env(KAERU_ENV_BOOT_ONCE, t, boot_once_get);
}

void boot_target_init(void) {
    refresh();
    fastboot_publish("kaeru-boot-target", target_var);
    fastboot_publish("kaeru-boot-once", once_var);
}
