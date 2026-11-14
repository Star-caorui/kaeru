// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read-only ext2/3/4 driver for kaeru.
//
// Ported from GRUB 2.14 (grub-2.14.tar.xz from ftp.gnu.org, SHA-256
// bc8d3c73535b8838d8c8e2654d73edc4e6ae8c8acdb45d5df5dc9a1547446d43):
//   grub-core/fs/ext2.c   - superblock, group descriptors, inodes,
//                           extent and indirect block mapping, directories
//   grub-core/fs/fshelp.c - path walking with symlinks, file reads
// Both files are GPLv3-or-later; their notices are kept below. This port is
// distributed under the same license.
//
// SPDX-FileCopyrightText: 2003-2009 Free Software Foundation, Inc.
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
//
// Changes from GRUB:
//  - The GRUB module, disk, file and error frameworks are replaced by a
//    small byte-offset read callback (struct ext4_dev) and returned error
//    codes. No global state; everything hangs off struct ext4_fs.
//  - Metadata goes through a one-block cache and data is read in whole
//    blocks, so the backend only sees block aligned requests (UFS friendly).
//    Contiguous extents are read with a single backend call.
//  - Directory lookup compares names inline instead of going through
//    fshelp's iterate hook. Like GRUB, htree (dir_index) directories are
//    scanned linearly; the htree blocks look like empty entries to that scan.
//  - Uninitialized (unwritten) extents read back as zeros. GRUB 2.14 reads
//    whatever the preallocated blocks hold.
//  - Fast symlinks are recognised the way Linux does (no data blocks),
//    not only by length.
//  - Extra validation of extent headers, directory entries, block numbers
//    and inode numbers, so a corrupted filesystem fails instead of reading
//    out of bounds.
//  - All copies are byte or word wise on aligned data only: kaeru builds
//    with -mno-unaligned-access and its memcpy faults on unaligned pointers.
//  - No 64-bit division, so nothing pulls in __aeabi_uldivmod on ARM32.
//  - Read only. Nothing here can write to the device.
//
// ---------------------------------------------------------------------------
// grub-core/fs/ext2.c - Second Extended filesystem
//
//  GRUB  --  GRand Unified Bootloader
//  Copyright (C) 2003,2004,2005,2007,2008,2009  Free Software Foundation, Inc.
//
//  GRUB is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  GRUB is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
// ---------------------------------------------------------------------------
// grub-core/fs/fshelp.c - Filesystem helper functions
//
//  GRUB  --  GRand Unified Bootloader
//  Copyright (C) 2004,2005,2006,2007,2008  Free Software Foundation, Inc.
//
//  GRUB is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  GRUB is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <lib/fs/ext4.h>

#ifdef EXT4_HOST_TEST
#include <stdlib.h>
#else
#include <stage1/memory.h>
#endif

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "ext4.c assumes a little endian CPU"
#endif

// ext2/3/4 is little endian on disk, as are all kaeru targets.
#define le16(x) ((uint16_t)(x))
#define le32(x) ((uint32_t)(x))

#define EXT2_MAGIC 0xEF53
#define INDIRECT_BLOCKS 12

#define EXT2_GOOD_OLD_REVISION 0
#define EXT2_GOOD_OLD_INODE_SIZE 128

#define FILETYPE_UNKNOWN 0
#define FILETYPE_REG 1
#define FILETYPE_DIRECTORY 2
#define FILETYPE_SYMLINK 7

#define FILETYPE_INO_MASK 0170000
#define FILETYPE_INO_REG 0100000
#define FILETYPE_INO_DIRECTORY 0040000
#define FILETYPE_INO_SYMLINK 0120000

#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001

#define EXT2_FEATURE_INCOMPAT_COMPRESSION 0x0001
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002
#define EXT3_FEATURE_INCOMPAT_RECOVER 0x0004
#define EXT3_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008
#define EXT2_FEATURE_INCOMPAT_META_BG 0x0010
#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT 0x0080
#define EXT4_FEATURE_INCOMPAT_MMP 0x0100
#define EXT4_FEATURE_INCOMPAT_FLEX_BG 0x0200
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED 0x2000
#define EXT4_FEATURE_INCOMPAT_LARGEDIR 0x4000
#define EXT4_FEATURE_INCOMPAT_ENCRYPT 0x10000

// Same sets as GRUB 2.14; see the rationale there. Anything else in
// s_feature_incompat (inline_data, ea_inode, dirdata, casefold, ...) makes
// the mount fail.
#define EXT2_DRIVER_SUPPORTED_INCOMPAT                                             \
    (EXT2_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS |              \
     EXT4_FEATURE_INCOMPAT_FLEX_BG | EXT2_FEATURE_INCOMPAT_META_BG |               \
     EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_ENCRYPT)
#define EXT2_DRIVER_IGNORED_INCOMPAT                                               \
    (EXT3_FEATURE_INCOMPAT_RECOVER | EXT4_FEATURE_INCOMPAT_MMP |                   \
     EXT4_FEATURE_INCOMPAT_CSUM_SEED | EXT4_FEATURE_INCOMPAT_LARGEDIR)

#define EXT4_ENCRYPT_FLAG 0x800
#define EXT4_EXTENTS_FLAG 0x80000

#define EXT4_EXT_MAGIC 0xf30a
// Linux caps the extent tree depth at 5 (EXT4_MAX_EXTENT_DEPTH).
#define EXT4_EXT_MAX_DEPTH 5
// ee_len above this marks an uninitialized extent of (ee_len - 32768) blocks.
#define EXT4_EXT_INIT_MAX_LEN 32768

#define MAX_NAMELEN 255
#define MAX_SYMLINK_NEST 8

// The ext2 superblock, up to s_blocks_count_hi.
struct ext2_sblock {
    uint32_t total_inodes;
    uint32_t total_blocks;
    uint32_t reserved_blocks;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t first_data_block;
    uint32_t log2_block_size;
    uint32_t log2_fragment_size;
    uint32_t blocks_per_group;
    uint32_t fragments_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t utime;
    uint16_t mnt_count;
    uint16_t max_mnt_count;
    uint16_t magic;
    uint16_t fs_state;
    uint16_t error_handling;
    uint16_t minor_revision_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t revision_level;
    uint16_t uid_reserved;
    uint16_t gid_reserved;
    uint32_t first_inode;
    uint16_t inode_size;
    uint16_t block_group_number;
    uint32_t feature_compatibility;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint16_t uuid[8];
    char volume_name[16];
    char last_mounted_on[64];
    uint32_t compression_info;
    uint8_t prealloc_blocks;
    uint8_t prealloc_dir_blocks;
    uint16_t reserved_gdt_blocks;
    uint8_t journal_uuid[16];
    uint32_t journal_inum;
    uint32_t journal_dev;
    uint32_t last_orphan;
    uint32_t hash_seed[4];
    uint8_t def_hash_version;
    uint8_t jnl_backup_type;
    uint16_t group_desc_size;
    uint32_t default_mount_opts;
    uint32_t first_meta_bg;
    uint32_t mkfs_time;
    uint32_t jnl_blocks[17];
    uint32_t total_blocks_hi;
};

_Static_assert(sizeof(struct ext2_sblock) == 0x154, "superblock layout");

struct ext2_block_group {
    uint32_t block_id;
    uint32_t inode_id;
    uint32_t inode_table_id;
    uint16_t free_blocks;
    uint16_t free_inodes;
    uint16_t used_dirs;
    uint16_t pad;
    uint32_t reserved[3];
    uint32_t block_id_hi;
    uint32_t inode_id_hi;
    uint32_t inode_table_id_hi;
    uint16_t free_blocks_hi;
    uint16_t free_inodes_hi;
    uint16_t used_dirs_hi;
    uint16_t pad2;
    uint32_t reserved2[3];
};

_Static_assert(sizeof(struct ext2_block_group) == 64, "group descriptor layout");

struct ext2_inode {
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t nlinks;
    uint32_t blockcnt; // In 512 byte units unless huge_file says otherwise
    uint32_t flags;
    uint32_t osd1;
    union {
        struct {
            uint32_t dir_blocks[INDIRECT_BLOCKS];
            uint32_t indir_block;
            uint32_t double_indir_block;
            uint32_t triple_indir_block;
        } blocks;
        char symlink[60];
    };
    uint32_t version;
    uint32_t acl;
    uint32_t size_high;
    uint32_t fragment_addr;
    uint32_t osd2[3];
};

_Static_assert(sizeof(struct ext2_inode) == 128, "inode layout");

struct ext2_dirent {
    uint32_t inode;
    uint16_t direntlen;
    uint8_t namelen;
    uint8_t filetype;
};

struct ext4_extent_header {
    uint16_t magic;
    uint16_t entries;
    uint16_t max;
    uint16_t depth;
    uint32_t generation;
};

struct ext4_extent {
    uint32_t block;
    uint16_t len;
    uint16_t start_hi;
    uint32_t start;
};

struct ext4_extent_idx {
    uint32_t block;
    uint32_t leaf;
    uint16_t leaf_hi;
    uint16_t unused;
};

enum node_type {
    NODE_UNKNOWN,
    NODE_REG,
    NODE_DIR,
    NODE_SYMLINK,
};

struct ext4_node {
    struct ext4_fs* fs;
    uint32_t ino;
    int inode_read;
    struct ext2_inode inode;
};

struct ext4_fs {
    struct ext4_dev dev;
    struct ext2_sblock sblock;
    uint32_t log2_bs;  // log2 of the block size in bytes
    uint32_t bs;       // Block size in bytes
    uint32_t inode_size;
    int log_group_desc_size;
    uint64_t total_blocks;
    uint32_t group_count;

    // One block of metadata, so a directory scan or indirect block walk
    // does not hit the backend for every few bytes.
    uint8_t* cache;
    uint64_t cache_blk;
    int cache_valid;

    // Holds a non-root extent tree node during a lookup. Tagged, so the
    // next lookup in the same file usually finds its node already here.
    uint8_t* ext_buf;
    uint64_t ext_buf_blk;
    int ext_buf_valid;

    struct ext4_node root;
};

struct ext4_file {
    struct ext4_node node;
    uint64_t size;
};

// Path walking stack, as in GRUB's fshelp.
struct stack_element {
    struct stack_element* parent;
    struct ext4_node* node;
    enum node_type type;
};

struct find_ctx {
    struct ext4_node* rootnode;
    int symlinknest;
    struct stack_element* currnode;
};

// ---------------------------------------------------------------------------
// Small helpers. Byte loops on purpose: see the header comment.

static void copy_bytes(void* dst, const void* src, uint32_t n) {
    uint8_t* d = dst;
    const uint8_t* s = src;

    if ((((uintptr_t)d | (uintptr_t)s) & 3) == 0) {
        while (n >= 4) {
            *(uint32_t*)d = *(const uint32_t*)s;
            d += 4;
            s += 4;
            n -= 4;
        }
    }
    while (n--)
        *d++ = *s++;
}

static void zero_bytes(void* dst, uint32_t n) {
    uint8_t* d = dst;

    while (n && ((uintptr_t)d & 3)) {
        *d++ = 0;
        n--;
    }
    while (n >= 4) {
        *(uint32_t*)d = 0;
        d += 4;
        n -= 4;
    }
    while (n--)
        *d++ = 0;
}

static int names_equal(const char* a, uint32_t alen, const char* b, uint32_t blen) {
    if (alen != blen)
        return 0;
    for (uint32_t i = 0; i < alen; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

#define EXT2_INODE_SIZE(sb)                                                        \
    (le32((sb)->revision_level) == EXT2_GOOD_OLD_REVISION ? EXT2_GOOD_OLD_INODE_SIZE \
                                                         : le16((sb)->inode_size))

static uint64_t inode_size_bytes(const struct ext2_inode* inode) {
    return le32(inode->size) | ((uint64_t)le32(inode->size_high) << 32);
}

static enum node_type inode_type(const struct ext2_inode* inode) {
    switch (le16(inode->mode) & FILETYPE_INO_MASK) {
        case FILETYPE_INO_DIRECTORY:
            return NODE_DIR;
        case FILETYPE_INO_SYMLINK:
            return NODE_SYMLINK;
        case FILETYPE_INO_REG:
            return NODE_REG;
        default:
            return NODE_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// Block access.

static int dev_read(struct ext4_fs* fs, uint64_t off, void* buf, uint32_t len) {
    if (off > fs->dev.size || len > fs->dev.size - off)
        return EXT4_EIO;
    return fs->dev.read(fs->dev.ctx, off, buf, len) ? EXT4_EIO : EXT4_OK;
}

// Reads COUNT whole blocks starting at BLK.
static int read_blocks(struct ext4_fs* fs, uint64_t blk, uint32_t count, void* buf) {
    if (!count)
        return EXT4_OK;
    if (blk >= fs->total_blocks || count > fs->total_blocks - blk)
        return EXT4_EBADFS;
    return dev_read(fs, blk << fs->log2_bs, buf, count << fs->log2_bs);
}

static int cache_block(struct ext4_fs* fs, uint64_t blk) {
    int err;

    if (fs->cache_valid && fs->cache_blk == blk)
        return EXT4_OK;

    fs->cache_valid = 0;
    err = read_blocks(fs, blk, 1, fs->cache);
    if (err)
        return err;

    fs->cache_blk = blk;
    fs->cache_valid = 1;
    return EXT4_OK;
}

// grub_disk_read() equivalent for metadata: LEN bytes at byte OFF of block
// BLK, possibly running into the following blocks.
static int read_meta(struct ext4_fs* fs, uint64_t blk, uint32_t off, uint32_t len, void* buf) {
    uint8_t* dst = buf;

    blk += off >> fs->log2_bs;
    off &= fs->bs - 1;

    while (len) {
        uint32_t chunk = fs->bs - off;
        int err;

        if (chunk > len)
            chunk = len;

        err = cache_block(fs, blk);
        if (err)
            return err;

        copy_bytes(dst, fs->cache + off, chunk);
        dst += chunk;
        len -= chunk;
        off = 0;
        blk++;
    }

    return EXT4_OK;
}

// ---------------------------------------------------------------------------
// Group descriptors and inodes (GRUB: grub_ext2_blockgroup,
// grub_ext2_read_inode).

// Check is a = b^x for some x.
static int is_power_of(uint64_t a, uint32_t b) {
    uint64_t c;
    // Prevent overflow assuming b < 8.
    if (a >= (1ULL << 60))
        return 0;
    for (c = 1; c <= a; c *= b)
        ;
    return (c == a);
}

static int group_has_super_block(struct ext4_fs* fs, uint64_t group) {
    if (!(le32(fs->sblock.feature_ro_compat) & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER))
        return 1;
    // Algorithm looked up in Linux source.
    if (group <= 1)
        return 1;
    // Even number is never a power of odd number.
    if (!(group & 1))
        return 0;
    return (is_power_of(group, 7) || is_power_of(group, 5) || is_power_of(group, 3));
}

static int read_blockgroup(struct ext4_fs* fs, uint32_t group, struct ext2_block_group* blkgrp) {
    uint64_t full_offset = ((uint64_t)group << fs->log_group_desc_size);
    uint64_t block = full_offset >> fs->log2_bs;
    uint32_t offset = (uint32_t)(full_offset & (fs->bs - 1));
    uint32_t desc_size = 1U << fs->log_group_desc_size;

    if (group >= fs->group_count)
        return EXT4_EBADFS;

    if ((le32(fs->sblock.feature_incompat) & EXT2_FEATURE_INCOMPAT_META_BG) &&
        block >= le32(fs->sblock.first_meta_bg)) {
        // Find the first block group for which a descriptor is stored in
        // given block.
        uint64_t first_block_group = block << (fs->log2_bs - fs->log_group_desc_size);

        block = first_block_group * le32(fs->sblock.blocks_per_group);
        if (group_has_super_block(fs, first_block_group))
            block++;
    } else {
        // Superblock.
        block++;
    }

    zero_bytes(blkgrp, sizeof(*blkgrp));
    if (desc_size > sizeof(*blkgrp))
        desc_size = sizeof(*blkgrp);

    return read_meta(fs, le32(fs->sblock.first_data_block) + block, offset, desc_size, blkgrp);
}

static int read_inode(struct ext4_fs* fs, uint32_t ino, struct ext2_inode* inode) {
    struct ext2_block_group blkgrp;
    struct ext2_sblock* sblock = &fs->sblock;
    uint32_t inodes_per_block, blkno, blkoff, idx;
    uint64_t base;
    int err;

    if (ino == 0 || ino > le32(sblock->total_inodes))
        return EXT4_EBADFS;

    // It is easier to calculate if the first inode is 0.
    ino--;

    err = read_blockgroup(fs, ino / le32(sblock->inodes_per_group), &blkgrp);
    if (err)
        return err;

    idx = ino % le32(sblock->inodes_per_group);
    inodes_per_block = fs->bs / fs->inode_size;
    blkno = idx / inodes_per_block;
    blkoff = idx % inodes_per_block;

    base = le32(blkgrp.inode_table_id);
    if (fs->log_group_desc_size >= 6)
        base |= (uint64_t)le32(blkgrp.inode_table_id_hi) << 32;

    return read_meta(fs, base + blkno, fs->inode_size * blkoff, sizeof(*inode), inode);
}

static int node_read_inode(struct ext4_node* node) {
    int err;

    if (node->inode_read)
        return EXT4_OK;

    err = read_inode(node->fs, node->ino, &node->inode);
    if (err)
        return err;

    node->inode_read = 1;
    return EXT4_OK;
}

// ---------------------------------------------------------------------------
// Block mapping (GRUB: grub_ext4_find_leaf, grub_ext2_read_block).
//
// Maps file block FILEBLOCK to a disk block in *PHYS (0 for a hole or an
// uninitialized extent, both read as zeros). *RUN is how many following
// file blocks map contiguously in the same way, at least 1.

static int map_extent(struct ext4_node* node, uint64_t fileblock, uint64_t* phys, uint32_t* run) {
    struct ext4_fs* fs = node->fs;
    const struct ext4_extent_header* hdr =
            (const struct ext4_extent_header*)node->inode.blocks.dir_blocks;
    const struct ext4_extent* ext;
    // Entries that fit after the header, inline (60 bytes) or in a block.
    uint32_t cap = (sizeof(node->inode.blocks) / sizeof(struct ext4_extent)) - 1;
    uint32_t nent, depth = 0;
    int i;

    *phys = 0;
    *run = 1;

    // Logical block numbers are 32 bits in extent trees.
    if (fileblock > 0xFFFFFFFFULL)
        return EXT4_OK;

    while (1) {
        const struct ext4_extent_idx* index = (const struct ext4_extent_idx*)(hdr + 1);
        uint64_t block;

        if (le16(hdr->magic) != EXT4_EXT_MAGIC)
            return EXT4_EBADFS;

        nent = le16(hdr->entries);
        if (nent > cap || nent > le16(hdr->max))
            return EXT4_EBADFS;

        if (le16(hdr->depth) == 0)
            break;

        if (++depth > EXT4_EXT_MAX_DEPTH)
            return EXT4_EBADFS;

        for (i = 0; i < (int)nent; i++) {
            if (fileblock < le32(index[i].block))
                break;
        }

        // Before the first index: leaf for the given block is absent
        // (i.e. sparse).
        if (--i < 0)
            return EXT4_OK;

        block = le16(index[i].leaf_hi);
        block = (block << 32) | le32(index[i].leaf);

        // The index entry is consumed before ext_buf is overwritten, so a
        // node in ext_buf may be replaced by its child.
        if (!fs->ext_buf_valid || fs->ext_buf_blk != block) {
            fs->ext_buf_valid = 0;
            if (read_blocks(fs, block, 1, fs->ext_buf))
                return EXT4_EBADFS;
            fs->ext_buf_blk = block;
            fs->ext_buf_valid = 1;
        }

        hdr = (const struct ext4_extent_header*)fs->ext_buf;
        cap = (fs->bs / sizeof(struct ext4_extent)) - 1;
    }

    ext = (const struct ext4_extent*)(hdr + 1);

    for (i = 0; i < (int)nent; i++) {
        if (fileblock < le32(ext[i].block))
            break;
    }

    if (--i < 0) {
        // A hole before the first extent of this leaf.
        if (nent)
            *run = le32(ext[0].block) - (uint32_t)fileblock;
        return EXT4_OK;
    }

    {
        uint32_t off = (uint32_t)fileblock - le32(ext[i].block);
        uint32_t len = le16(ext[i].len);
        int uninit = 0;

        if (len > EXT4_EXT_INIT_MAX_LEN) {
            len -= EXT4_EXT_INIT_MAX_LEN;
            uninit = 1;
        }

        if (off >= len) {
            // A hole between this extent and the next one.
            if (i + 1 < (int)nent && le32(ext[i + 1].block) > fileblock)
                *run = le32(ext[i + 1].block) - (uint32_t)fileblock;
            return EXT4_OK;
        }

        *run = len - off;
        if (!uninit) {
            uint64_t start = le16(ext[i].start_hi);
            start = (start << 32) + le32(ext[i].start);
            *phys = start + off;
        }
    }

    return EXT4_OK;
}

static int map_indirect(struct ext4_node* node, uint64_t fileblock, uint64_t* phys) {
    struct ext4_fs* fs = node->fs;
    struct ext2_inode* inode = &node->inode;
    uint64_t blksz_quarter = fs->bs / 4;
    int log_perblock = fs->log2_bs - 2;
    uint32_t indir;
    int shift;

    *phys = 0;

    // Direct blocks.
    if (fileblock < INDIRECT_BLOCKS) {
        *phys = le32(inode->blocks.dir_blocks[fileblock]);
        return EXT4_OK;
    }
    fileblock -= INDIRECT_BLOCKS;
    // Indirect.
    if (fileblock < blksz_quarter) {
        indir = inode->blocks.indir_block;
        shift = 0;
        goto indirect;
    }
    fileblock -= blksz_quarter;
    // Double indirect.
    if (fileblock < blksz_quarter * blksz_quarter) {
        indir = inode->blocks.double_indir_block;
        shift = 1;
        goto indirect;
    }
    fileblock -= blksz_quarter * blksz_quarter;
    // Triple indirect.
    if (fileblock < blksz_quarter * blksz_quarter * (blksz_quarter + 1)) {
        indir = inode->blocks.triple_indir_block;
        shift = 2;
        goto indirect;
    }
    // Quadruple indirect blocks do not exist.
    return EXT4_EBADFS;

indirect:
    do {
        // If the indirect block is zero, all child blocks are absent
        // (i.e. filled with zeros.)
        if (indir == 0)
            return EXT4_OK;
        if (read_meta(fs, le32(indir),
                      (uint32_t)((fileblock >> (log_perblock * shift)) &
                                 ((1U << log_perblock) - 1)) *
                              sizeof(indir),
                      sizeof(indir), &indir))
            return EXT4_EBADFS;
    } while (shift--);

    *phys = le32(indir);
    return EXT4_OK;
}

static int map_block(struct ext4_node* node, uint64_t fileblock, uint64_t* phys, uint32_t* run) {
    int err;

    if (node->inode.flags & EXT4_EXTENTS_FLAG) {
        err = map_extent(node, fileblock, phys, run);
    } else {
        *run = 1;
        err = map_indirect(node, fileblock, phys);
    }

    if (err)
        return err;

    if (*phys) {
        if (*phys >= node->fs->total_blocks)
            return EXT4_EBADFS;
        if (*run > node->fs->total_blocks - *phys)
            *run = (uint32_t)(node->fs->total_blocks - *phys);
    }

    return EXT4_OK;
}

// ---------------------------------------------------------------------------
// File reads (GRUB: grub_fshelp_read_file).

static long read_node(struct ext4_node* node, uint64_t filesize, uint64_t pos, void* buf,
                      uint32_t len) {
    struct ext4_fs* fs = node->fs;
    uint8_t* dst = buf;
    uint32_t done = 0;

    if (pos >= filesize)
        return 0;
    // Adjust LEN so it we can't read past the end of the file.
    if (len > filesize - pos)
        len = (uint32_t)(filesize - pos);

    while (done < len) {
        uint64_t fileblock = pos >> fs->log2_bs;
        uint32_t blockoff = (uint32_t)(pos & (fs->bs - 1));
        uint32_t left = len - done;
        uint64_t phys;
        uint32_t run, chunk;
        int err;

        err = map_block(node, fileblock, &phys, &run);
        if (err)
            return err;

        if (blockoff == 0 && left >= fs->bs) {
            // Whole blocks, straight into the caller's buffer.
            uint32_t nblocks = left >> fs->log2_bs;

            if (nblocks > run)
                nblocks = run;
            chunk = nblocks << fs->log2_bs;

            // If the block number is 0 this block is not stored on disk
            // but is zero filled instead.
            if (phys) {
                err = read_blocks(fs, phys, nblocks, dst + done);
                if (err)
                    return err;
            } else {
                zero_bytes(dst + done, chunk);
            }
        } else {
            // Head or tail of a block, through the cache.
            chunk = fs->bs - blockoff;
            if (chunk > left)
                chunk = left;

            if (phys) {
                err = cache_block(fs, phys);
                if (err)
                    return err;
                copy_bytes(dst + done, fs->cache + blockoff, chunk);
            } else {
                zero_bytes(dst + done, chunk);
            }
        }

        done += chunk;
        pos += chunk;
    }

    return (long)len;
}

// ---------------------------------------------------------------------------
// Directories and symlinks (GRUB: grub_ext2_iterate_dir,
// grub_ext2_read_symlink).

// Looks NAME up in directory DIR. On success *FOUND is a new node (free
// with free()), or NULL if no entry matched.
static int dir_lookup(struct ext4_node* dir, const char* name, uint32_t namelen,
                      struct ext4_node** found, enum node_type* foundtype) {
    struct ext4_fs* fs = dir->fs;
    uint64_t dirsize;
    uint64_t fpos = 0;
    int err;

    *found = NULL;

    err = node_read_inode(dir);
    if (err)
        return err;

    if (dir->inode.flags & EXT4_ENCRYPT_FLAG)
        return EXT4_EENCRYPTED;

    // GRUB reads only the low 32 bits of a directory's size.
    dirsize = le32(dir->inode.size);

    // Search the file.
    while (fpos < dirsize) {
        struct ext2_dirent dirent;
        char filename[MAX_NAMELEN + 1];
        uint32_t inblock = (uint32_t)(fpos & (fs->bs - 1));
        long r;

        r = read_node(dir, dirsize, fpos, &dirent, sizeof(dirent));
        if (r < 0)
            return (int)r;
        if (r != sizeof(dirent))
            return EXT4_EBADFS;

        if (le16(dirent.direntlen) == 0)
            return EXT4_OK;

        // Entries never straddle blocks and always cover their name.
        if (le16(dirent.direntlen) < sizeof(dirent) + dirent.namelen ||
            (le16(dirent.direntlen) & 3) || inblock + le16(dirent.direntlen) > fs->bs)
            return EXT4_EBADFS;

        if (le32(dirent.inode) != 0 && dirent.namelen == namelen) {
            r = read_node(dir, dirsize, fpos + sizeof(dirent), filename, dirent.namelen);
            if (r < 0)
                return (int)r;
            if (r != dirent.namelen)
                return EXT4_EBADFS;

            if (names_equal(filename, dirent.namelen, name, namelen)) {
                struct ext4_node* fdiro = malloc(sizeof(*fdiro));
                enum node_type type = NODE_UNKNOWN;

                if (!fdiro)
                    return EXT4_ENOMEM;

                fdiro->fs = fs;
                fdiro->ino = le32(dirent.inode);
                fdiro->inode_read = 0;

                if (dirent.filetype != FILETYPE_UNKNOWN) {
                    if (dirent.filetype == FILETYPE_DIRECTORY)
                        type = NODE_DIR;
                    else if (dirent.filetype == FILETYPE_SYMLINK)
                        type = NODE_SYMLINK;
                    else if (dirent.filetype == FILETYPE_REG)
                        type = NODE_REG;
                } else {
                    // The filetype can not be read from the dirent, read
                    // the inode to get more information.
                    err = node_read_inode(fdiro);
                    if (err) {
                        free(fdiro);
                        return err;
                    }
                    type = inode_type(&fdiro->inode);
                }

                *found = fdiro;
                *foundtype = type;
                return EXT4_OK;
            }
        }

        fpos += le16(dirent.direntlen);
    }

    return EXT4_OK;
}

// Returns a malloc()ed, NUL terminated target in *OUT.
static int read_symlink(struct ext4_node* node, char** out) {
    struct ext4_fs* fs = node->fs;
    uint64_t size;
    char* symlink;
    int err;

    err = node_read_inode(node);
    if (err)
        return err;

    if (node->inode.flags & EXT4_ENCRYPT_FLAG)
        return EXT4_EENCRYPTED;

    size = inode_size_bytes(&node->inode);
    // Linux limits symlink targets to one block (PATH_MAX at most).
    if (size == 0 || size >= fs->bs)
        return EXT4_EBADFS;

    symlink = malloc((uint32_t)size + 1);
    if (!symlink)
        return EXT4_ENOMEM;

    // A fast symlink keeps its target in the inode's block array and has
    // no data blocks (Linux: ext4_inode_is_fast_symlink()). GRUB only looks
    // at the length.
    if (size < sizeof(node->inode.symlink) &&
        (le32(node->inode.blockcnt) == 0 ||
         (le32(node->inode.acl) && le32(node->inode.blockcnt) == (fs->bs >> 9)))) {
        copy_bytes(symlink, node->inode.symlink, (uint32_t)size);
    } else {
        long r = read_node(node, size, 0, symlink, (uint32_t)size);
        if (r < 0 || (uint64_t)r != size) {
            free(symlink);
            return r < 0 ? (int)r : EXT4_EBADFS;
        }
    }

    symlink[size] = '\0';
    *out = symlink;
    return EXT4_OK;
}

// ---------------------------------------------------------------------------
// Path walking (GRUB: grub_fshelp_find_file and helpers).

static void free_node(struct ext4_node* node, struct find_ctx* ctx) {
    if (node != ctx->rootnode)
        free(node);
}

static void pop_element(struct find_ctx* ctx) {
    struct stack_element* el = ctx->currnode;
    ctx->currnode = el->parent;
    free_node(el->node, ctx);
    free(el);
}

static void free_stack(struct find_ctx* ctx) {
    while (ctx->currnode)
        pop_element(ctx);
}

static void go_up_a_level(struct find_ctx* ctx) {
    if (!ctx->currnode->parent)
        return;
    pop_element(ctx);
}

static int push_node(struct find_ctx* ctx, struct ext4_node* node, enum node_type type) {
    struct stack_element* nst = malloc(sizeof(*nst));
    if (!nst)
        return EXT4_ENOMEM;
    nst->node = node;
    nst->type = type;
    nst->parent = ctx->currnode;
    ctx->currnode = nst;
    return EXT4_OK;
}

static int go_to_root(struct find_ctx* ctx) {
    free_stack(ctx);
    return push_node(ctx, ctx->rootnode, NODE_DIR);
}

static int find_file(const char* currpath, struct find_ctx* ctx) {
    const char *name, *next;
    int err;

    for (name = currpath;; name = next) {
        struct ext4_node* foundnode = NULL;
        enum node_type foundtype = NODE_UNKNOWN;

        // Remove all leading slashes.
        while (*name == '/')
            name++;

        // Found the node!
        if (!*name)
            return EXT4_OK;

        // Extract the actual part from the pathname.
        for (next = name; *next && *next != '/'; next++)
            ;

        if (next - name > MAX_NAMELEN)
            return EXT4_ENOENT;

        // At this point it is expected that the current node is a
        // directory, check if this is true.
        if (ctx->currnode->type != NODE_DIR)
            return EXT4_ENOTDIR;

        // Don't rely on fs providing actual . in the listing.
        if (next - name == 1 && name[0] == '.')
            continue;

        // Don't rely on fs providing actual .. in the listing.
        if (next - name == 2 && name[0] == '.' && name[1] == '.') {
            go_up_a_level(ctx);
            continue;
        }

        err = dir_lookup(ctx->currnode->node, name, (uint32_t)(next - name), &foundnode,
                         &foundtype);
        if (err)
            return err;

        if (!foundnode)
            return EXT4_ENOENT;

        err = push_node(ctx, foundnode, foundtype);
        if (err) {
            free(foundnode);
            return err;
        }

        // Read in the symlink and follow it.
        if (ctx->currnode->type == NODE_SYMLINK) {
            char* symlink;

            // Test if the symlink does not loop.
            if (++ctx->symlinknest == MAX_SYMLINK_NEST)
                return EXT4_ELOOP;

            err = read_symlink(ctx->currnode->node, &symlink);
            if (err)
                return err;

            if (symlink[0] == '/') {
                // The symlink is an absolute path, go back to the root inode.
                err = go_to_root(ctx);
                if (err) {
                    free(symlink);
                    return err;
                }
            } else {
                // Get from symlink to containing directory.
                go_up_a_level(ctx);
            }

            // Lookup the node the symlink points to.
            err = find_file(symlink, ctx);
            free(symlink);

            if (err)
                return err;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API (GRUB: grub_ext2_mount, grub_ext2_open, grub_ext2_read).

int ext4_mount(const struct ext4_dev* dev, struct ext4_fs** out) {
    struct ext4_fs* fs;
    uint8_t* sb_buf;
    uint32_t log2_block_size, incompat;
    uint64_t data_blocks;
    int err;

    *out = NULL;

    if (!dev || !dev->read)
        return EXT4_EINVAL;

    fs = malloc(sizeof(*fs));
    if (!fs)
        return EXT4_ENOMEM;
    zero_bytes(fs, sizeof(*fs));
    copy_bytes(&fs->dev, dev, sizeof(fs->dev));

    // Read the superblock. It lives at byte 1024; read the first 4 KiB so
    // the request is aligned for 4 KiB sector devices too.
    sb_buf = malloc(4096);
    if (!sb_buf) {
        err = EXT4_ENOMEM;
        goto fail;
    }
    err = dev_read(fs, 0, sb_buf, 4096);
    if (!err)
        copy_bytes(&fs->sblock, sb_buf + 1024, sizeof(fs->sblock));
    free(sb_buf);
    if (err)
        goto fail;

    // Make sure this is an ext2 filesystem.
    log2_block_size = le32(fs->sblock.log2_block_size);
    if (le16(fs->sblock.magic) != EXT2_MAGIC ||
        // 1 KiB to 64 KiB, the range Linux accepts.
        log2_block_size > 6 || le32(fs->sblock.inodes_per_group) == 0 ||
        le32(fs->sblock.blocks_per_group) == 0) {
        err = EXT4_EBADFS;
        goto fail;
    }

    fs->log2_bs = log2_block_size + 10;
    fs->bs = 1U << fs->log2_bs;
    fs->inode_size = EXT2_INODE_SIZE(&fs->sblock);

    if (fs->inode_size < EXT2_GOOD_OLD_INODE_SIZE || fs->inode_size > fs->bs ||
        (fs->inode_size & (fs->inode_size - 1))) {
        err = EXT4_EBADFS;
        goto fail;
    }

    // Check the FS doesn't have feature bits enabled that we don't support.
    incompat = le32(fs->sblock.feature_incompat);
    if (le32(fs->sblock.revision_level) != EXT2_GOOD_OLD_REVISION &&
        (incompat & ~(EXT2_DRIVER_SUPPORTED_INCOMPAT | EXT2_DRIVER_IGNORED_INCOMPAT))) {
        err = EXT4_EUNSUPP;
        goto fail;
    }

    if (le32(fs->sblock.revision_level) != EXT2_GOOD_OLD_REVISION &&
        (incompat & EXT4_FEATURE_INCOMPAT_64BIT) && fs->sblock.group_desc_size != 0 &&
        ((le16(fs->sblock.group_desc_size) & (le16(fs->sblock.group_desc_size) - 1)) == 0) &&
        (le16(fs->sblock.group_desc_size) & 0x1fe0)) {
        uint16_t b = le16(fs->sblock.group_desc_size);
        for (fs->log_group_desc_size = 0; b != (1 << fs->log_group_desc_size);
             fs->log_group_desc_size++)
            ;
    } else {
        fs->log_group_desc_size = 5;
    }

    fs->total_blocks = le32(fs->sblock.total_blocks);
    if (le32(fs->sblock.revision_level) != EXT2_GOOD_OLD_REVISION &&
        (incompat & EXT4_FEATURE_INCOMPAT_64BIT))
        fs->total_blocks |= (uint64_t)le32(fs->sblock.total_blocks_hi) << 32;

    if (fs->total_blocks <= le32(fs->sblock.first_data_block) ||
        (fs->total_blocks << fs->log2_bs) >> fs->log2_bs != fs->total_blocks ||
        (fs->total_blocks << fs->log2_bs) > dev->size) {
        err = EXT4_EBADFS;
        goto fail;
    }

    // Number of block groups, rounded up. 32-bit division only: ARM32
    // would need libgcc for a 64-bit one, and a block count above 2^32
    // (16 TiB with 4 KiB blocks) is far beyond what this driver is for.
    data_blocks = fs->total_blocks - le32(fs->sblock.first_data_block);
    if (data_blocks > 0xFFFFFFFFULL) {
        err = EXT4_EUNSUPP;
        goto fail;
    }
    {
        uint32_t bpg = le32(fs->sblock.blocks_per_group);
        uint32_t n = (uint32_t)data_blocks;

        fs->group_count = n / bpg + (n % bpg ? 1 : 0);
    }
    if ((uint64_t)fs->group_count * le32(fs->sblock.inodes_per_group) <
        le32(fs->sblock.total_inodes)) {
        err = EXT4_EBADFS;
        goto fail;
    }

    fs->cache = malloc(fs->bs);
    fs->ext_buf = malloc(fs->bs);
    if (!fs->cache || !fs->ext_buf) {
        err = EXT4_ENOMEM;
        goto fail;
    }

    fs->root.fs = fs;
    fs->root.ino = 2;
    err = node_read_inode(&fs->root);
    if (err)
        goto fail;

    if (inode_type(&fs->root.inode) != NODE_DIR) {
        err = EXT4_EBADFS;
        goto fail;
    }

    *out = fs;
    return EXT4_OK;

fail:
    ext4_umount(fs);
    return err;
}

void ext4_umount(struct ext4_fs* fs) {
    if (!fs)
        return;
    if (fs->cache)
        free(fs->cache);
    if (fs->ext_buf)
        free(fs->ext_buf);
    free(fs);
}

uint32_t ext4_block_size(const struct ext4_fs* fs) {
    return fs->bs;
}

int ext4_needs_recovery(const struct ext4_fs* fs) {
    return (le32(fs->sblock.feature_incompat) & EXT3_FEATURE_INCOMPAT_RECOVER) != 0;
}

void ext4_label(const struct ext4_fs* fs, char out[17]) {
    copy_bytes(out, fs->sblock.volume_name, 16);
    out[16] = '\0';
}

int ext4_open(struct ext4_fs* fs, const char* path, struct ext4_file** out) {
    struct find_ctx ctx = {
            .rootnode = &fs->root,
            .symlinknest = 0,
            .currnode = NULL,
    };
    struct ext4_file* file;
    struct ext4_node* node;
    enum node_type type;
    int err;

    *out = NULL;

    if (!path || path[0] != '/')
        return EXT4_EINVAL;

    err = go_to_root(&ctx);
    if (err)
        return err;

    err = find_file(path, &ctx);
    if (err) {
        free_stack(&ctx);
        return err;
    }

    node = ctx.currnode->node;
    type = ctx.currnode->type;

    // Check if the node that was found was of the expected type.
    if (type != NODE_REG) {
        free_stack(&ctx);
        return EXT4_ENOTREG;
    }

    file = malloc(sizeof(*file));
    if (!file) {
        free_stack(&ctx);
        return EXT4_ENOMEM;
    }

    copy_bytes(&file->node, node, sizeof(file->node));
    free_stack(&ctx);

    err = node_read_inode(&file->node);
    if (err) {
        free(file);
        return err;
    }

    // The directory entry said regular file; trust the inode over it.
    if (inode_type(&file->node.inode) != NODE_REG) {
        free(file);
        return EXT4_ENOTREG;
    }

    if (file->node.inode.flags & EXT4_ENCRYPT_FLAG) {
        free(file);
        return EXT4_EENCRYPTED;
    }

    file->size = inode_size_bytes(&file->node.inode);
    *out = file;
    return EXT4_OK;
}

void ext4_close(struct ext4_file* file) {
    if (file)
        free(file);
}

uint64_t ext4_file_size(const struct ext4_file* file) {
    return file->size;
}

long ext4_read(struct ext4_file* file, uint64_t pos, void* buf, uint32_t len) {
    // Keep the result representable as a positive long on 32-bit targets.
    if (len > 0x40000000U)
        len = 0x40000000U;
    return read_node(&file->node, file->size, pos, buf, len);
}

const char* ext4_strerror(int err) {
    switch (err) {
        case EXT4_OK:
            return "success";
        case EXT4_EIO:
            return "read error";
        case EXT4_ENOMEM:
            return "out of memory";
        case EXT4_EBADFS:
            return "not ext2/3/4 or corrupted";
        case EXT4_EUNSUPP:
            return "unsupported incompatible feature";
        case EXT4_ENOENT:
            return "no such file or directory";
        case EXT4_ENOTDIR:
            return "not a directory";
        case EXT4_ENOTREG:
            return "not a regular file";
        case EXT4_ELOOP:
            return "too many levels of symlinks";
        case EXT4_EINVAL:
            return "invalid argument";
        case EXT4_EENCRYPTED:
            return "file is encrypted";
        default:
            return "unknown error";
    }
}
