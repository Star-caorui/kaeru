//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Loading an arm64 Linux kernel, initrd and devicetree named by an
// extlinux.conf, and preparing the tree to boot it with. See
// lib/boot/linux.c.
//

#pragma once

#include <stdint.h>

#include <lib/boot/extlinux.h>

struct linux_image {
    char label[EXTLINUX_NAME_MAX];
    char append[EXTLINUX_APPEND_MAX];
    uint8_t has_append;
    uint8_t is_image;      // The kernel is a raw arm64 Image
    uint32_t kernel;       // Entry point: a 2 MiB boundary (text_offset is 0)
    uint32_t kernel_size;  // File size
    uint32_t image_size;   // Memory the kernel claims from its entry, bss included
    uint32_t initrd;       // 0 if there is none
    uint32_t initrd_size;
    uint32_t fdt;          // The tree as read from the partition
    uint32_t fdt_size;
    uint32_t tree;         // Room for the tree to boot with
    uint32_t tree_size;
    uint32_t devinfo;      // One page for LK's devinfo words
    uint32_t sum;          // linux_checksum() right after loading
};

// One line of progress, without a newline. Lines stay within 59
// characters, the most a fastboot INFO can carry.
typedef void (*linux_log_t)(const char* line);

// Finds extlinux.conf on the ext2/3/4 partition part, takes the label
// named name (the default one if name is NULL or empty, or if there is no
// such label) and reads the kernel, initrd and tree into
// [base, base + size), leaving room after them for the tree to boot with
// and the devinfo page. With require_image, anything but a raw arm64 Image
// is refused. Returns 0, or -1 after logging why.
int linux_load(const char* part, const char* name, uint32_t base, uint32_t size,
               int require_image, struct linux_image* img, linux_log_t log);

// The partition holding extlinux.conf: kaeru_linux_partition in the
// environment, else CONFIG_BOOT_LINUX_PARTITION.
void linux_partition(char* out, size_t n);

// Whether extlinux.conf on part has a bootable label named name. Returns
// 0, or -1 after logging why not.
int linux_has_label(const char* part, const char* name, linux_log_t log);

// A checksum over the loaded files, to tell whether anything overwrote
// them between loading and booting.
uint32_t linux_checksum(const struct linux_image* img);

// Builds the tree to boot in img->tree: the loaded tree with the memory
// node, reserved regions and devinfo LK put in lk_fdt, the APPEND line as
// bootargs and the initrd. Returns 0, or -1 after logging why.
int linux_fixup(const struct linux_image* img, const void* lk_fdt, linux_log_t log);
