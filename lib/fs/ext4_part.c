//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Partition backends for the ext4 driver: kaeru's own GPT view when the
// storage library is built, the bootloader's partition API otherwise.
//

#include <lib/fs/ext4_part.h>

#ifdef CONFIG_STORAGE_SUPPORT
#include <lib/mt_part.h>
#include <lib/storage.h>

// kaeru's own GPT view of the disk.
static int ext4_part_read(void* ctx, uint64_t off, void* buf, uint32_t len) {
    return storage_part_read((const struct part_info*)ctx, buf, off, len) == (ssize_t)len ? 0 : -1;
}

int ext4_part_open(const char* name, struct ext4_dev* dev) {
    const struct part_info* part = storage_part_find(name);

    if (!part)
        return -1;

    dev->read = ext4_part_read;
    dev->ctx = (void*)part;
    dev->size = (uint64_t)part->size_blocks * BLOCK_SIZE;
    return 0;
}

#else

// The bootloader's own partition API, the same one stage1 loads us with.
// It knows the device's sector size and which hardware partition the
// user area is (both differ between eMMC and UFS).
static int ext4_part_read(void* ctx, uint64_t off, void* buf, uint32_t len) {
    long ret = ((long (*)(const char*, long long, uint8_t*, size_t))(CONFIG_PARTITION_READ_ADDRESS |
                                                                     1))(
            (const char*)ctx, (long long)off, (uint8_t*)buf, len);
    return ret == (long)len ? 0 : -1;
}

int ext4_part_open(const char* name, struct ext4_dev* dev) {
    uint64_t size = ((uint64_t (*)(const char*))(CONFIG_PARTITION_GET_SIZE_BY_NAME_ADDRESS | 1))(
            name);

    // (uint64_t)-1 is what LK returns for an unknown name.
    if (size == 0 || size == (uint64_t)-1)
        return -1;

    dev->read = ext4_part_read;
    dev->ctx = (void*)name;
    dev->size = size;
    return 0;
}

#endif
