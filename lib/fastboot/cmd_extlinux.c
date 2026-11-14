//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// 'fastboot oem extlinux [partition]': finds extlinux.conf on an ext2/3/4
// partition the way U-Boot does, picks the default label and reads its
// kernel, initrd and devicetree into the bootloader's scratch buffer (the
// same memory fastboot downloads into), with lib/boot/linux.c, the loader
// booting uses. It reports where each file landed, its size, format and
// SHA-256, and the command line. Nothing is booted and nothing is written
// to storage.
//
// The partition defaults to the kaeru_linux_partition environment
// variable, then to CONFIG_BOOT_LINUX_PARTITION.
//

#include <lib/boot/linux.h>
#include <lib/debug.h>
#include <lib/fastboot.h>
#include <lib/string.h>
#include <stage1/memory.h>

#define PART_MAX 40

static void info_line(const char* line) {
    fastboot_info(line);
}

static void show_append(const struct linux_image* img) {
    char line[FASTBOOT_INFO_MAX + 1];
    size_t n = strlen(img->append);

    if (!img->has_append) {
        fastboot_info("append: none, the tree's bootargs stay");
        return;
    }
    npf_snprintf(line, sizeof(line), "append: %u chars", (unsigned)n);
    fastboot_info(line);
    // INFO lines hold 59 characters; send the command line in pieces.
    for (size_t i = 0; i < n; i += 56) {
        npf_snprintf(line, sizeof(line), "  %.56s", img->append + i);
        fastboot_info(line);
    }
}

static void cmd_extlinux(const char* arg, void* data, unsigned sz) {
    char part[PART_MAX];
    char line[FASTBOOT_INFO_MAX + 1];
    struct linux_image* img;

    (void)sz;

    next_token(arg, part, sizeof(part));
    if (!part[0])
        linux_partition(part, sizeof(part));

    // Only ever write where fastboot itself downloads to.
    if ((uintptr_t)data != CONFIG_BOOT_LINUX_SCRATCH_ADDRESS) {
        npf_snprintf(line, sizeof(line), "download buffer 0x%08x, expected 0x%08x",
                     (unsigned)(uintptr_t)data, (unsigned)CONFIG_BOOT_LINUX_SCRATCH_ADDRESS);
        fastboot_info(line);
        fastboot_fail("Scratch buffer mismatch");
        return;
    }

    img = malloc(sizeof(*img));
    if (!img) {
        fastboot_fail("Out of memory");
        return;
    }
    if (linux_load(part, NULL, CONFIG_BOOT_LINUX_SCRATCH_ADDRESS, CONFIG_BOOT_LINUX_SCRATCH_SIZE, 0,
                   img, info_line)) {
        free(img);
        fastboot_fail("Cannot load");
        return;
    }
    show_append(img);
    npf_snprintf(line, sizeof(line), "checksum %08x", (unsigned)img->sum);
    fastboot_info(line);
    free(img);
    fastboot_okay("read only, not booted");
}

FASTBOOT_CMD(extlinux, "oem extlinux", cmd_extlinux, 1);
