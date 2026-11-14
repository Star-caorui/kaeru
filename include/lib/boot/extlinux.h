//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// extlinux.conf parser, the subset U-Boot's distro boot (sysboot) reads and
// the files distributions generate (Debian u-boot-menu, Alpine
// update-extlinux, postmarketOS boot-deploy). See lib/boot/extlinux.c.
//

#pragma once

#include <stddef.h>
#include <stdint.h>

#define EXTLINUX_MAX_ENTRIES 32
#define EXTLINUX_NAME_MAX 64
#define EXTLINUX_PATH_MAX 256
// arm64's COMMAND_LINE_SIZE.
#define EXTLINUX_APPEND_MAX 2048
// Larger files are refused rather than parsed in part.
#define EXTLINUX_FILE_MAX (64 * 1024)

enum extlinux_err {
    EXTLINUX_OK = 0,
    EXTLINUX_ETOOLONG = -1,  // A value does not fit its field
    EXTLINUX_ETOOMANY = -2,  // More than EXTLINUX_MAX_ENTRIES labels
    EXTLINUX_ENOLABEL = -3,  // No LABEL at all
    EXTLINUX_ENOKERNEL = -4, // The chosen label has no LINUX/KERNEL
    EXTLINUX_EBADVAL = -5,   // A value that must be a number or non-empty is not
    EXTLINUX_ENOTFOUND = -6, // No label by the name asked for
};

struct extlinux_entry {
    char label[EXTLINUX_NAME_MAX];
    char menu_label[EXTLINUX_NAME_MAX];
    char kernel[EXTLINUX_PATH_MAX];
    char initrd[EXTLINUX_PATH_MAX];
    char fdt[EXTLINUX_PATH_MAX];
    char fdtdir[EXTLINUX_PATH_MAX];
    char append[EXTLINUX_APPEND_MAX];
    uint8_t has_append;   // APPEND given in this label (even if empty)
    uint8_t has_overlays; // FDTOVERLAYS given; not supported, only noted
    uint8_t localboot;    // LOCALBOOT: not a Linux entry
};

struct extlinux_config {
    struct extlinux_entry entries[EXTLINUX_MAX_ENTRIES];
    int count;
    int default_index; // Valid after a successful parse
    int timeout;       // Tenths of a second; 0 if not given
    // The DEFAULT line named no existing label; the first label was chosen.
    uint8_t default_missing;
    // Keywords this parser does not know or ignores (MENU TITLE, PROMPT...
    // are not counted; INCLUDE, SAY, ONTIMEOUT... are).
    int unknown;
    // Line of the first error, 1-based; 0 if the error has no line.
    int err_line;
};

// Parses len bytes of buf (need not be NUL-terminated). On success the
// entries are filled and default_index points at the one to boot: the
// label with MENU DEFAULT, else the one DEFAULT names, else the first.
// Labels without APPEND inherit an APPEND given before the first LABEL.
int extlinux_parse(const char* buf, size_t len, struct extlinux_config* cfg);

// The entry to boot, or NULL with *err set (EXTLINUX_ENOKERNEL).
const struct extlinux_entry* extlinux_default(const struct extlinux_config* cfg, int* err);

// The label named name (compared exactly, as U-Boot does), or NULL with
// *err set (EXTLINUX_ENOTFOUND, EXTLINUX_ENOKERNEL).
const struct extlinux_entry* extlinux_find(const struct extlinux_config* cfg, const char* name,
                                           int* err);

// U-Boot's path rules: absolute paths are from the root of the filesystem
// the config lives on, relative ones are from the config file's directory.
int extlinux_resolve(const char* cfg_path, const char* file, char* out, size_t out_len);

// FDTDIR plus the board's devicetree name, e.g. "mediatek/xxx.dtb".
int extlinux_fdtdir_path(const char* cfg_path, const char* fdtdir, const char* fdtfile, char* out,
                         size_t out_len);

// U-Boot's search order on a partition. Returns NULL past the last one.
const char* extlinux_config_path(int index);

const char* extlinux_strerror(int err);
