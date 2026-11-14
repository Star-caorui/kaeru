//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// AOSP A/B metadata.
//
// AOSP's bootloader_control lives in misc at offset 2048 (struct
// bootloader_control in bootloader_message.h):
//
//   0   char slot_suffix[4]
//   4   uint32 magic, 0x42414342 ("BCAB")
//   8   uint8 version, 1
//   9   uint8 nb_slot:3, recovery_tries_remaining:3, merge_status:3 (low bits first)
//   10  uint8 reserved0[2]
//   12  slot_metadata slot_info[4], 2 bytes each:
//         priority:4, tries_remaining:3, successful_boot:1,
//         verity_corrupted:1, reserved:7
//   20  uint8 reserved1[8]
//   28  uint32 crc32_le of the 28 bytes before it
//
// The bootloader boots the highest priority slot. While that slot has not
// been marked successful (after an update, or 'fastboot --set-active'),
// each boot of it uses up one try, and only Android marks it successful.
// On gold this is MediaTek's preloader ("[AB] boot_success is 1!").
//

#include <lib/boot/ab.h>

#ifdef AB_HOST_TEST
#include <string.h>
#else
#include <lib/fs/ext4_part.h>
#include <lib/string.h>
#include <stage1/memory.h>
#endif

#define AB_OFFSET 2048
#define AB_MAGIC 0x42414342u
#define AB_SIZE 32
// Read whole sectors from the start of misc: UFS sectors are 4 KiB.
#define AB_READ 4096

static uint32_t le32(const uint8_t* p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t crc32_le(const uint8_t* p, uint32_t n) {
    uint32_t crc = 0xFFFFFFFFu;

    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

int ab_parse(const uint8_t* p, struct ab_state* st) {
    int best = -1;

    if (le32(p + 4) != AB_MAGIC || p[8] != 1 || le32(p + 28) != crc32_le(p, 28))
        return -1;

    st->nb_slot = p[9] & 7;
    if (st->nb_slot < 1 || st->nb_slot > 4)
        return -1;

    for (int i = 0; i < st->nb_slot; i++) {
        uint8_t b = p[12 + 2 * i];

        st->slot[i].priority = b & 0xF;
        st->slot[i].tries_remaining = (b >> 4) & 7;
        st->slot[i].successful_boot = b >> 7;
        // Ties go to the lower slot, as in AOSP's reference bootloader.
        if (st->slot[i].priority && (best < 0 || st->slot[i].priority > st->slot[best].priority))
            best = i;
    }
    st->current = best;
    return best < 0 ? -1 : 0;
}

#ifndef AB_HOST_TEST
int ab_read(struct ab_state* st) {
    struct ext4_dev dev;
    uint8_t* buf;
    int ret = -1;

    if (ext4_part_open("misc", &dev) || dev.size < AB_READ)
        return -1;
    buf = malloc(AB_READ);
    if (!buf)
        return -1;
    if (!dev.read(dev.ctx, 0, buf, AB_READ))
        ret = ab_parse(buf + AB_OFFSET, st);
    free(buf);
    return ret;
}
#endif
