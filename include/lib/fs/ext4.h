//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read-only ext2/3/4 driver, ported from GRUB. See lib/fs/ext4.c.
//

#pragma once

#include <stddef.h>
#include <stdint.h>

enum ext4_err {
    EXT4_OK = 0,
    EXT4_EIO = -1,         // The backend failed to read, or a read ran off the device
    EXT4_ENOMEM = -2,      // malloc() failed
    EXT4_EBADFS = -3,      // Not ext2/3/4, or the metadata is inconsistent
    EXT4_EUNSUPP = -4,     // Incompatible feature this driver does not know
    EXT4_ENOENT = -5,      // A path component does not exist
    EXT4_ENOTDIR = -6,     // A path component is not a directory
    EXT4_ENOTREG = -7,     // The path does not name a regular file
    EXT4_ELOOP = -8,       // Too many nested symlinks
    EXT4_EINVAL = -9,      // Bad argument (e.g. a relative path)
    EXT4_EENCRYPTED = -10, // The inode is encrypted (fscrypt)
};

// Where the filesystem lives. read() gets a byte offset from the start of
// the filesystem and must fill exactly len bytes, returning 0 on success.
// The driver only ever asks for whole filesystem blocks at block aligned
// offsets, except for the superblock (4096 bytes at offset 0).
struct ext4_dev {
    int (*read)(void* ctx, uint64_t off, void* buf, uint32_t len);
    void* ctx;
    uint64_t size; // Bytes available to read()
};

struct ext4_fs;
struct ext4_file;

// Mounting copies *dev, so the caller's struct may go away afterwards.
int ext4_mount(const struct ext4_dev* dev, struct ext4_fs** out);
void ext4_umount(struct ext4_fs* fs);

uint32_t ext4_block_size(const struct ext4_fs* fs);
// The journal was not replayed when the filesystem was last in use. Files
// written shortly before an unclean shutdown may read back stale.
int ext4_needs_recovery(const struct ext4_fs* fs);
// Copies the volume label (up to 16 bytes) and NUL-terminates it.
void ext4_label(const struct ext4_fs* fs, char out[17]);

// path must be absolute. Symlinks are followed, also on the last component.
int ext4_open(struct ext4_fs* fs, const char* path, struct ext4_file** out);
void ext4_close(struct ext4_file* file);
uint64_t ext4_file_size(const struct ext4_file* file);

// Reads up to len bytes at pos. Returns the number of bytes read (0 at or
// past the end of the file), or a negative enum ext4_err.
long ext4_read(struct ext4_file* file, uint64_t pos, void* buf, uint32_t len);

const char* ext4_strerror(int err);
