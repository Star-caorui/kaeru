//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Opens a partition by name as an ext4_dev for lib/fs/ext4.c.
//

#pragma once

#include <lib/fs/ext4.h>

// Returns 0, or -1 if the bootloader knows no partition by that name.
// name must stay valid while dev is in use.
int ext4_part_open(const char* name, struct ext4_dev* dev);
