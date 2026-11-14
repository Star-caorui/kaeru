//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// 'fastboot oem boot-target linux|android' sets which OS boots by default.
// 'fastboot oem boot-once linux|android|none' overrides it for the next
// boot only; 'none' cancels a pending override. 'oem boot-once linux
// <label>' also picks the extlinux.conf label to boot instead of the
// default, so a new kernel can be tried once and the default comes back by
// itself; the label has to exist when the command is given.
//
// Read them back with 'fastboot getvar kaeru-boot-target' and
// 'fastboot getvar kaeru-boot-once'.
//
// Asking for Linux while the slot is not confirmed by Android yet says
// what will happen: boot-target waits for Android to confirm it, a
// boot-once boots Linux anyway and costs the slot a try (see
// lib/boot/linux_boot.c). A boot-once is refused outright when that would
// leave the slot fewer than BOOT_ONCE_MIN_TRIES tries to begin with: with
// so few left, Linux could take the last ones before Android gets to
// confirm an update. Without readable A/B metadata nothing is known about
// the tries, and a boot-once is let through with a warning: that is when
// Android may be broken and Linux the way in.
//

#ifdef CONFIG_BOOT_LINUX_AB_CHECK
#include <lib/boot/ab.h>
#endif
#ifdef CONFIG_EXTLINUX_SUPPORT
#include <lib/boot/linux.h>
#endif
#include <lib/boot/target.h>
#include <lib/debug.h>
#include <lib/fastboot.h>
#include <lib/string.h>

#ifdef CONFIG_BOOT_LINUX_AB_CHECK
static void info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void info(const char* fmt, ...) {
    char line[FASTBOOT_INFO_MAX + 1];
    va_list ap;

    va_start(ap, fmt);
    npf_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fastboot_info(line);
}

#define BOOT_ONCE_MIN_TRIES 3

// st is NULL without valid A/B metadata.
static int unconfirmed(const struct ab_state* st) {
    return st && !st->slot[st->current].successful_boot;
}

// Sends FAIL and returns -1 if a Linux boot-once must be refused.
static int refuse_boot_once(const struct ab_state* st) {
    const struct ab_slot* s;
    char line[FASTBOOT_INFO_MAX + 1];

    if (!unconfirmed(st))
        return 0;
    s = &st->slot[st->current];
    if (s->tries_remaining >= BOOT_ONCE_MIN_TRIES)
        return 0;
    info("slot %c is not confirmed by Android yet and has",
         (char)('a' + st->current));
    info("only %u %s left; Linux could use up the last", (unsigned)s->tries_remaining,
         s->tries_remaining == 1 ? "try" : "tries");
    info("ones and roll an update on it back.");
    npf_snprintf(line, sizeof(line), "Fewer than %d tries left, boot Android first",
                 BOOT_ONCE_MIN_TRIES);
    fastboot_fail(line);
    return -1;
}

static void warn_unconfirmed(int once, const struct ab_state* st) {
    const struct ab_slot* s;
    char slot;

    if (!st) {
        if (once) {
            info("warning: no valid A/B metadata in misc;");
            info("Linux boots anyway, as asked.");
        } else {
            info("note: no valid A/B metadata in misc, so");
            info("Android boots instead of Linux.");
        }
        return;
    }
    if (!unconfirmed(st))
        return;
    s = &st->slot[st->current];
    slot = (char)('a' + st->current);
    if (!once) {
        info("note: slot %c is not confirmed by Android yet.", slot);
        info("Android boots until it is, then Linux.");
        return;
    }
    info("warning: slot %c is not confirmed by Android yet.", slot);
    info("Booting Linux on it uses one of its %u tries;", (unsigned)s->tries_remaining);
    info("with none left, the bootloader falls back to");
    info("the other slot and an update on it is rolled back.");
    info("Boot Android once first to keep it.");
}
#endif

#ifdef CONFIG_EXTLINUX_SUPPORT
static void info_line(const char* line) {
    fastboot_info(line);
}
#endif

// Checks label for 'oem boot-once linux <label>'. Sends FAIL and returns
// -1 if it cannot be booted.
static int check_label(const char* label) {
#ifdef CONFIG_EXTLINUX_SUPPORT
    char part[40];

    if (strlen(label) > BOOT_ONCE_LABEL_MAX) {
        fastboot_fail("Label too long");
        return -1;
    }
    linux_partition(part, sizeof(part));
    if (linux_has_label(part, label, info_line)) {
        fastboot_fail("No such label in extlinux.conf");
        return -1;
    }
    return 0;
#else
    (void)label;
    fastboot_fail("Labels need extlinux.conf support");
    return -1;
#endif
}

static void boot_target_cmd(const char* arg, int once) {
    char tok[16];
    char label[BOOT_ONCE_LABEL_MAX + 2];
    char line[FASTBOOT_INFO_MAX + 1];
    enum boot_target t;
    const char* var = once ? "kaeru-boot-once" : "kaeru-boot-target";
#ifdef CONFIG_BOOT_LINUX_AB_CHECK
    struct ab_state ab;
    const struct ab_state* st = NULL;
#endif

    arg = next_token(arg, tok, sizeof(tok));
    next_token(arg, label, sizeof(label));
    if (boot_target_parse(tok, &t) || (!once && t == BOOT_TARGET_NONE) ||
        (label[0] && (!once || t != BOOT_TARGET_LINUX))) {
        fastboot_fail(once ? "Usage: oem boot-once linux [label]|android|none"
                           : "Usage: oem boot-target linux|android");
        return;
    }
    if (label[0] && check_label(label))
        return;

#ifdef CONFIG_BOOT_LINUX_AB_CHECK
    if (t == BOOT_TARGET_LINUX) {
        if (!ab_read(&ab))
            st = &ab;
        if (once && refuse_boot_once(st))
            return;
    }
#endif

    if (label[0] ? boot_once_set_label(label) : once ? boot_once_set(t) : boot_target_set(t)) {
        fastboot_fail("Failed to write the environment");
        return;
    }

    if (label[0])
        npf_snprintf(line, sizeof(line), "%s: linux:%s", var, label);
    else
        npf_snprintf(line, sizeof(line), "%s: %s", var, boot_target_name(t));
    fastboot_info(line);
#ifdef CONFIG_BOOT_LINUX_AB_CHECK
    if (t == BOOT_TARGET_LINUX)
        warn_unconfirmed(once, st);
#endif
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
