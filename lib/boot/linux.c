//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// arm64 Linux from an extlinux.conf.
//
// linux_load() reads the default label's files from an ext2/3/4 partition
// into one region:
//
//   kernel    at the first 2 MiB boundary, image_size bytes (its bss too)
//   initrd    page aligned, if the label has one
//   fdt       page aligned, the tree as read
//   tree      page aligned, the tree's size plus TREE_SLACK, filled in by
//             linux_fixup()
//   devinfo   one page
//
// The kernel runs where it was loaded: arm64 Images may sit at any 2 MiB
// boundary in RAM (flags bit 3), so nothing is copied before the jump. A
// gzip kernel (Image.gz) is read to the end of the region first and
// unpacked to that boundary, with tinf; the unpacked Image must end before
// the compressed file starts.
//
// linux_fixup() does what a bootloader has to on this hardware: the tree
// the kernel gets is the one from the partition, with what depends on the
// device and its RAM taken from the tree LK built for Android:
//
//  - The memory node is LK's.
//  - Reserved regions are matched by start address. A region both trees
//    have keeps the partition tree's node (its name and properties, such
//    as ramoops') and takes LK's size; one only LK has is added as LK names
//    it; one only the partition tree has is dropped. Nodes without a reg
//    (dynamically placed ones) are left alone.
//  - LK's devinfo, its copy of the efuse words the kernel needs (thermal
//    calibration, speed bins), comes as /chosen/atag,devinfo (a word count,
//    then the words, little endian); the efuse reads as zeros from the
//    normal world. The words go to the devinfo page, and
//    /reserved-memory/devinfo, if the tree has that node, gets the page as
//    its reg.
//  - APPEND becomes /chosen/bootargs, the initrd /chosen/linux,initrd-*.
//
// /chosen/kaeru,linux-fixup records <memory kept resized added dropped>
// and /chosen/kaeru,linux-devinfo the number of devinfo words. If LK's
// tree is not usable the partition tree is used as it is, with memory 0;
// if a change fails half way, with memory 0xffffffff.
//
// The fixup follows mainline-gold's shim-fixup.c, which did the same from
// a 64-bit shim (same author).
//
// linux_fixup() can run after LK has turned the MMU and caches off, so
// nothing here makes unaligned accesses: libfdt reads properties a byte
// at a time and gets byte-wise helpers (lib/libfdt/libfdt_env.h).
//

#include <arch/cache.h>
#include <lib/boot/linux.h>
#include <lib/common.h>
#include <lib/crypto/sha256.h>
#include <lib/debug.h>
#include <lib/fs/ext4.h>
#include <lib/fs/ext4_part.h>
#include <lib/string.h>
#include <stage1/memory.h>

#include <tinf.h>

// After kaeru's headers: it maps the string helpers to byte-wise ones.
#include <libfdt.h>

#define SZ_2M 0x200000
#define PAGE 0x1000
#define READ_CHUNK (128 * 1024)
// Room for what the fixup adds: LK places some 30 reserved regions.
#define TREE_SLACK 0x10000
#define DEVINFO_SIZE PAGE
#define MAX_REGIONS 64

static void say(linux_log_t log, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(linux_log_t log, const char* fmt, ...) {
    char line[60];
    va_list ap;

    va_start(ap, fmt);
    npf_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log(line);
}

static void hex(char* out, const uint8_t* in, int n) {
    static const char digits[] = "0123456789abcdef";

    for (int i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0xF];
    }
    out[2 * n] = '\0';
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// The kernel's format; sets img->is_image and image_size for an Image.
static void describe_kernel(struct linux_image* img, linux_log_t log) {
    const uint8_t* p = (const uint8_t*)(uintptr_t)img->kernel;
    uint32_t n = img->kernel_size;

    if (n >= 0x40 && p[0x38] == 'A' && p[0x39] == 'R' && p[0x3A] == 'M' && p[0x3B] == 0x64) {
        uint32_t text_offset = LE32(p + 8), size_lo = LE32(p + 16), size_hi = LE32(p + 20);

        say(log, "  arm64 Image, image_size 0x%x", (unsigned)size_lo);
        if (text_offset) {
            say(log, "  text_offset 0x%x is not supported", (unsigned)text_offset);
        } else if (size_hi || size_lo < n) {
            say(log, "  bad image_size");
        } else {
            img->is_image = 1;
            img->image_size = size_lo;
        }
    } else if (n >= 0x20 && p[0] == 'M' && p[1] == 'Z' && p[4] == 'z' && p[5] == 'i' &&
               p[6] == 'm' && p[7] == 'g') {
        char comp[9];

        for (int i = 0; i < 8; i++)
            comp[i] = (char)p[0x18 + i];
        comp[8] = '\0';
        say(log, "  EFI zboot (%s), not supported yet", comp);
    } else {
        say(log, "  unknown format (%02x %02x %02x %02x)", n > 0 ? (unsigned)p[0] : 0u,
             n > 1 ? (unsigned)p[1] : 0u, n > 2 ? (unsigned)p[2] : 0u,
             n > 3 ? (unsigned)p[3] : 0u);
    }
}

static int describe_fdt(const struct linux_image* img, linux_log_t log) {
    const uint8_t* p = (const uint8_t*)(uintptr_t)img->fdt;
    uint32_t n = img->fdt_size, magic = n >= 8 ? be32(p) : 0;

    if (magic == 0xD00DFEED) {
        uint32_t total = be32(p + 4);

        say(log, "  FDT, totalsize %u", (unsigned)total);
        if (total > n || fdt_check_header(p)) {
            say(log, "  the tree is damaged");
            return -1;
        }
        return 0;
    }
    if (magic == 0xD7B7AB1E)
        say(log, "  Android dt_table, not a plain FDT");
    else
        say(log, "  not an FDT (magic %08x)", (unsigned)magic);
    return -1;
}

struct loader {
    struct ext4_fs* fs;
    const char* cfg_path;
    uint32_t next;  // First free byte
    uint32_t limit; // End of the region
    linux_log_t log;
};

// Opens rel, resolved against the config. Files over 4 GiB cannot fit.
static int open_file(struct loader* l, const char* what, const char* rel,
                     struct ext4_file** file, uint32_t* size) {
    char path[EXTLINUX_PATH_MAX];
    int err;

    if (extlinux_resolve(l->cfg_path, rel, path, sizeof(path))) {
        say(l->log, "%s: path too long", what);
        return -1;
    }
    say(l->log, "%s: %.50s", what, path);

    err = ext4_open(l->fs, path, file);
    if (err) {
        say(l->log, "  %s", ext4_strerror(err));
        return -1;
    }
    if (ext4_file_size(*file) > l->limit) {
        ext4_close(*file);
        say(l->log, "  does not fit the load region");
        return -1;
    }
    *size = (uint32_t)ext4_file_size(*file);
    return 0;
}

// Reads all n bytes of file to off and closes it.
static int read_into(struct loader* l, struct ext4_file* file, uint32_t n, uint32_t off) {
    char digest_hex[2 * SHA256_DIGEST_SIZE + 1];
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t* dst = (uint8_t*)(uintptr_t)off;
    uint32_t pos = 0;
    sha256_t sha;

    sha256_init(&sha);
    while (pos < n) {
        uint32_t want = n - pos < READ_CHUNK ? n - pos : READ_CHUNK;
        long r = ext4_read(file, pos, dst + pos, want);

        if (r <= 0) {
            ext4_close(file);
            say(l->log, "  read: %s", ext4_strerror(r ? (int)r : EXT4_EIO));
            return -1;
        }
        sha256_update(&sha, dst + pos, (size_t)r);
        pos += (uint32_t)r;
    }
    sha256_final(&sha, digest);
    ext4_close(file);

    say(l->log, "  %u bytes at 0x%08x", (unsigned)n, (unsigned)off);
    hex(digest_hex, digest, SHA256_DIGEST_SIZE / 2);
    say(l->log, "  sha256 %s", digest_hex);
    hex(digest_hex, digest + SHA256_DIGEST_SIZE / 2, SHA256_DIGEST_SIZE / 2);
    say(l->log, "         %s", digest_hex);
    return 0;
}

// Reads rel to the next boundary of align.
static int load_file(struct loader* l, const char* what, const char* rel, uint32_t align,
                     uint32_t* addr, uint32_t* size) {
    struct ext4_file* file = NULL;
    uint32_t off, n;

    if (open_file(l, what, rel, &file, &n))
        return -1;
    off = ROUNDUP(l->next, align);
    if (off < l->next || off > l->limit || n > l->limit - off) {
        ext4_close(file);
        say(l->log, "  does not fit the load region");
        return -1;
    }
    if (read_into(l, file, n, off))
        return -1;

    *addr = off;
    *size = n;
    l->next = off + n;
    return 0;
}

// The kernel goes to the next 2 MiB boundary. A gzip one is read to the
// end of the region and unpacked to that boundary.
static int load_kernel(struct loader* l, const char* rel, struct linux_image* img) {
    struct ext4_file* file = NULL;
    uint8_t magic[2];
    uint32_t base, n, at, out;
    int ret;

    if (open_file(l, "kernel", rel, &file, &n))
        return -1;
    base = ROUNDUP(l->next, SZ_2M);
    if (base < l->next || base > l->limit || n > l->limit - base) {
        ext4_close(file);
        say(l->log, "  does not fit the load region");
        return -1;
    }

    if (n < 18 || ext4_read(file, 0, magic, 2) != 2 || magic[0] != 0x1F || magic[1] != 0x8B) {
        if (read_into(l, file, n, base))
            return -1;
        img->kernel = base;
        img->kernel_size = n;
        l->next = base + n;
        return 0;
    }

    at = (l->limit - n) & ~(uint32_t)(PAGE - 1);
    if (at < base) {
        ext4_close(file);
        say(l->log, "  does not fit the load region");
        return -1;
    }
    if (read_into(l, file, n, at))
        return -1;

    out = at - base;
    ret = tinf_gzip_uncompress((void*)(uintptr_t)base, &out, (const void*)(uintptr_t)at, n);
    if (ret != TINF_OK) {
        say(l->log, "  gzip: %s",
            ret == TINF_BUF_ERROR ? "unpacked kernel does not fit" : "damaged");
        return -1;
    }
    say(l->log, "  gzip, unpacked %u bytes at 0x%08x", (unsigned)out, (unsigned)base);
    img->kernel = base;
    img->kernel_size = out;
    l->next = base + out;
    return 0;
}

static int read_config(struct loader* l, struct extlinux_config* cfg) {
    struct ext4_file* file = NULL;
    uint64_t size;
    char* buf;
    uint32_t pos = 0;
    int i, err = EXT4_ENOENT;

    for (i = 0; (l->cfg_path = extlinux_config_path(i)); i++) {
        err = ext4_open(l->fs, l->cfg_path, &file);
        if (err != EXT4_ENOENT && err != EXT4_ENOTDIR)
            break;
    }
    if (!l->cfg_path) {
        say(l->log, "no /extlinux/ or /boot/extlinux/extlinux.conf");
        return -1;
    }
    if (err) {
        say(l->log, "%s: %s", l->cfg_path, ext4_strerror(err));
        return -1;
    }

    size = ext4_file_size(file);
    if (size > EXTLINUX_FILE_MAX) {
        ext4_close(file);
        say(l->log, "extlinux.conf is larger than 64 KiB");
        return -1;
    }
    buf = malloc(size ? (size_t)size : 1);
    if (!buf) {
        ext4_close(file);
        say(l->log, "out of memory");
        return -1;
    }
    while (pos < size) {
        long r = ext4_read(file, pos, buf + pos, (uint32_t)size - pos);

        if (r <= 0) {
            free(buf);
            ext4_close(file);
            say(l->log, "cannot read extlinux.conf");
            return -1;
        }
        pos += (uint32_t)r;
    }
    ext4_close(file);

    err = extlinux_parse(buf, (size_t)size, cfg);
    free(buf);
    say(l->log, "config: %s", l->cfg_path);
    if (err) {
        if (cfg->err_line)
            say(l->log, "line %d: %s", cfg->err_line, extlinux_strerror(err));
        else
            say(l->log, "%s", extlinux_strerror(err));
        return -1;
    }
    return 0;
}

static void copy_str(char* dst, const char* src, size_t n) {
    size_t i = 0;

    for (; src[i] && i < n - 1; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int load_entry(struct loader* l, const struct extlinux_entry* e, int require_image,
                      struct linux_image* img) {
    char fdt_path[EXTLINUX_PATH_MAX];

    copy_str(img->label, e->label, sizeof(img->label));
    copy_str(img->append, e->append, sizeof(img->append));
    img->has_append = e->has_append;

    // The kernel first, on a 2 MiB boundary; its bss follows it.
    if (load_kernel(l, e->kernel, img))
        return -1;
    describe_kernel(img, l->log);
    if (require_image && !img->is_image)
        return -1;
    if (img->is_image) {
        if (img->image_size > l->limit - img->kernel) {
            say(l->log, "  image_size does not fit the load region");
            return -1;
        }
        l->next = img->kernel + img->image_size;
    }

    if (e->initrd[0] &&
        load_file(l, "initrd", e->initrd, PAGE, &img->initrd, &img->initrd_size))
        return -1;

    if (e->fdt[0]) {
        if (load_file(l, "fdt", e->fdt, PAGE, &img->fdt, &img->fdt_size))
            return -1;
    } else if (e->fdtdir[0]) {
        if (!CONFIG_BOOT_LINUX_FDTFILE[0]) {
            say(l->log, "FDTDIR needs CONFIG_BOOT_LINUX_FDTFILE");
            return -1;
        }
        if (extlinux_fdtdir_path(l->cfg_path, e->fdtdir, CONFIG_BOOT_LINUX_FDTFILE, fdt_path,
                                 sizeof(fdt_path))) {
            say(l->log, "fdt: path too long");
            return -1;
        }
        if (load_file(l, "fdt", fdt_path, PAGE, &img->fdt, &img->fdt_size))
            return -1;
    } else {
        say(l->log, "fdt: none given");
        return -1;
    }
    if (describe_fdt(img, l->log))
        return -1;

    // Room for the tree to boot with, then the devinfo page.
    img->tree = ROUNDUP(l->next, PAGE);
    img->tree_size = ROUNDUP(img->fdt_size + TREE_SLACK, PAGE);
    img->devinfo = img->tree + img->tree_size;
    if (img->tree < l->next || img->devinfo < img->tree ||
        DEVINFO_SIZE > l->limit - img->devinfo || img->devinfo > l->limit) {
        say(l->log, "no room for the tree after the files");
        return -1;
    }
    return 0;
}

int linux_load(const char* part, uint32_t base, uint32_t size, int require_image,
               struct linux_image* img, linux_log_t log) {
    struct extlinux_config* cfg = NULL;
    const struct extlinux_entry* e;
    struct ext4_dev dev;
    struct loader l;
    char label[17];
    int err, ret = -1;

    memset(img, 0, sizeof(*img));
    memset(&l, 0, sizeof(l));
    l.log = log;
    l.next = base;
    l.limit = base + size;
    if (l.limit < base) {
        say(log, "load region wraps around");
        return -1;
    }

    if (ext4_part_open(part, &dev)) {
        say(log, "partition %s not found", part);
        return -1;
    }
    err = ext4_mount(&dev, &l.fs);
    if (err) {
        say(log, "%s: %s", part, ext4_strerror(err));
        return -1;
    }
    ext4_label(l.fs, label);
    say(log, "%.12s: ext4 label '%s'", part, label);
    if (ext4_needs_recovery(l.fs))
        say(log, "warning: journal not replayed, data may be stale");

    cfg = malloc(sizeof(*cfg));
    if (!cfg) {
        say(log, "out of memory");
        goto out;
    }
    if (read_config(&l, cfg))
        goto out;

    say(log, "labels: %d, default: %d", cfg->count, cfg->default_index + 1);
    if (cfg->default_missing)
        say(log, "warning: DEFAULT names no label, using the first");
    if (cfg->unknown)
        say(log, "warning: %d unknown keyword(s) ignored", cfg->unknown);

    e = extlinux_default(cfg, &err);
    if (!e) {
        say(log, "%s", extlinux_strerror(err));
        goto out;
    }
    say(log, "label: %.50s", e->label);
    if (e->has_overlays)
        say(log, "warning: FDTOVERLAYS not supported, ignored");

    if (load_entry(&l, e, require_image, img))
        goto out;

    img->sum = linux_checksum(img);
    ret = 0;

out:
    if (cfg)
        free(cfg);
    ext4_umount(l.fs);
    return ret;
}

static uint32_t sum_range(uint32_t s, uint32_t addr, uint32_t size) {
    const volatile uint32_t* w = (const volatile uint32_t*)(uintptr_t)addr;
    const volatile uint8_t* b = (const volatile uint8_t*)(uintptr_t)addr;
    uint32_t i, n = size / 4;

    // Loads start on page boundaries, so the words are aligned.
    for (i = 0; i < n; i++)
        s = ((s << 5) | (s >> 27)) + w[i];
    for (i = n * 4; i < size; i++)
        s = ((s << 5) | (s >> 27)) + b[i];
    return s;
}

uint32_t linux_checksum(const struct linux_image* img) {
    uint32_t s = 0x6B616572; // "kaer"

    s = sum_range(s, img->kernel, img->kernel_size);
    if (img->initrd)
        s = sum_range(s, img->initrd, img->initrd_size);
    return sum_range(s, img->fdt, img->fdt_size);
}

// The fixup. Both trees give the root and /reserved-memory two address and
// two size cells.

static uint64_t cells64(const fdt32_t* p) {
    return ((uint64_t)fdt32_ld(p) << 32) | fdt32_ld(p + 1);
}

static int reg_of(const void* fdt, int node, uint64_t* start) {
    int len;
    const fdt32_t* reg = fdt_getprop(fdt, node, "reg", &len);

    if (!reg || len != 16)
        return -1;
    *start = cells64(reg);
    return 0;
}

static int memory_node(const void* fdt) {
    int node;

    fdt_for_each_subnode(node, fdt, 0) {
        const char* type = fdt_getprop(fdt, node, "device_type", NULL);

        if (type && !strcmp(type, "memory"))
            return node;
    }
    return -1;
}

// "mblock-2-dramc-rk1" and 0x1bffff000 make "dramc-rk1@1bffff000".
static void region_name(char* out, const char* lk_name, uint64_t start) {
    static const char digits[] = "0123456789abcdef";
    const char* p = lk_name;
    int i, shift;

    if (!strncmp(p, "mblock-", 7)) {
        const char* q = p + 7;

        while (*q >= '0' && *q <= '9')
            q++;
        if (q != p + 7 && *q == '-')
            p = q + 1;
    }
    for (i = 0; *p && *p != '@' && i < 40; p++)
        out[i++] = *p;
    out[i++] = '@';
    for (shift = 60; shift > 0 && !(start >> shift); shift -= 4)
        ;
    for (; shift >= 0; shift -= 4)
        out[i++] = digits[(start >> shift) & 0xF];
    out[i] = '\0';
}

// LK's devinfo words into the page, and the page into
// /reserved-memory/devinfo. Returns the number of words.
static uint32_t put_devinfo(const void* lk, void* buf, void* page) {
    const uint8_t* tag;
    uint64_t addr = (uintptr_t)page;
    fdt32_t reg[4];
    uint32_t n;
    int len, node;

    node = fdt_path_offset(lk, "/chosen");
    if (node < 0)
        return 0;
    tag = fdt_getprop(lk, node, "atag,devinfo", &len);
    if (!tag || len < 4)
        return 0;
    n = tag[0] | tag[1] << 8 | tag[2] << 16 | (uint32_t)tag[3] << 24;
    if (!n || n > (uint32_t)(len - 4) / 4 || n > DEVINFO_SIZE / 4)
        return 0;
    node = fdt_path_offset(buf, "/reserved-memory/devinfo");
    if (node < 0)
        return 0;
    memset(page, 0, DEVINFO_SIZE);
    memcpy(page, tag + 4, n * 4);
    reg[0] = cpu_to_fdt32(addr >> 32);
    reg[1] = cpu_to_fdt32(addr);
    reg[2] = cpu_to_fdt32(0);
    reg[3] = cpu_to_fdt32(DEVINFO_SIZE);
    if (fdt_setprop(buf, node, "reg", reg, sizeof(reg)))
        return 0;
    return n;
}

static int plausible_tree(const void* fdt) {
    uintptr_t p = (uintptr_t)fdt;

    return p >= 0x40000000 && !(p & 7) && !fdt_check_header(fdt);
}

// Memory node, reserved regions and devinfo from LK's tree. Returns 0 with
// stat[] filled, or -1 if a change failed and buf is half done.
static int take_from_lk(const void* lk, void* buf, void* page, fdt32_t stat[5],
                        fdt32_t* devinfo) {
    uint32_t kept = 0, resized = 0, added = 0, dropped = 0;
    uint64_t starts[MAX_REGIONS];
    int lk_nodes[MAX_REGIONS];
    int n = 0, i, node, lk_res, res;

    node = memory_node(lk);
    i = memory_node(buf);
    if (node >= 0 && i >= 0) {
        int len;
        const void* reg = fdt_getprop(lk, node, "reg", &len);

        if (reg && len > 0 && len % 16 == 0 && !fdt_setprop(buf, i, "reg", reg, len))
            stat[0] = cpu_to_fdt32(1);
    }

    lk_res = fdt_path_offset(lk, "/reserved-memory");
    res = fdt_path_offset(buf, "/reserved-memory");
    if (lk_res < 0 || res < 0)
        return 0;
    fdt_for_each_subnode(node, lk, lk_res)
        if (n < MAX_REGIONS && !reg_of(lk, node, &starts[n]))
            lk_nodes[n++] = node;

again:
    fdt_for_each_subnode(node, buf, res) {
        uint64_t start;

        if (reg_of(buf, node, &start))
            continue;
        for (i = 0; i < n && starts[i] != start; i++)
            ;
        if (i == n) {
            // Offsets after it move: start over.
            if (fdt_del_node(buf, node))
                return -1;
            dropped++;
            goto again;
        }
    }

    for (i = 0; i < n; i++) {
        const void* reg = fdt_getprop(lk, lk_nodes[i], "reg", NULL);
        uint64_t start;
        char name[64];

        fdt_for_each_subnode(node, buf, res)
            if (!reg_of(buf, node, &start) && start == starts[i])
                break;
        if (node >= 0) {
            const void* ours_reg = fdt_getprop(buf, node, "reg", NULL);

            if (memcmp(ours_reg, reg, 16)) {
                if (fdt_setprop(buf, node, "reg", reg, 16))
                    return -1;
                resized++;
            } else {
                kept++;
            }
            continue;
        }
        region_name(name, fdt_get_name(lk, lk_nodes[i], NULL), starts[i]);
        node = fdt_add_subnode(buf, res, name);
        if (node < 0 || fdt_setprop(buf, node, "reg", reg, 16))
            return -1;
        if (fdt_getprop(lk, lk_nodes[i], "no-map", NULL) && fdt_setprop_empty(buf, node, "no-map"))
            return -1;
        added++;
    }

    stat[1] = cpu_to_fdt32(kept);
    stat[2] = cpu_to_fdt32(resized);
    stat[3] = cpu_to_fdt32(added);
    stat[4] = cpu_to_fdt32(dropped);
    *devinfo = cpu_to_fdt32(put_devinfo(lk, buf, page));
    return 0;
}

int linux_fixup(const struct linux_image* img, const void* lk_fdt, linux_log_t log) {
    void* buf = (void*)(uintptr_t)img->tree;
    void* page = (void*)(uintptr_t)img->devinfo;
    const void* ours = (const void*)(uintptr_t)img->fdt;
    fdt32_t stat[5] = {0};
    fdt32_t devinfo = 0;
    int chosen, err;

    if (fdt_open_into(ours, buf, (int)img->tree_size)) {
        say(log, "cannot open the tree");
        return -1;
    }

    if (!plausible_tree(lk_fdt)) {
        say(log, "LK's tree at 0x%08x is not usable", (unsigned)(uintptr_t)lk_fdt);
    } else if (take_from_lk(lk_fdt, buf, page, stat, &devinfo)) {
        // Start again from the partition's tree, untouched.
        if (fdt_open_into(ours, buf, (int)img->tree_size)) {
            say(log, "cannot open the tree");
            return -1;
        }
        stat[0] = cpu_to_fdt32(0xFFFFFFFF);
        devinfo = 0;
    }

    chosen = fdt_path_offset(buf, "/chosen");
    if (chosen < 0)
        chosen = fdt_add_subnode(buf, 0, "chosen");
    if (chosen < 0) {
        say(log, "no /chosen");
        return -1;
    }
    err = fdt_setprop(buf, chosen, "kaeru,linux-fixup", stat, sizeof(stat));
    if (!err)
        err = fdt_setprop(buf, chosen, "kaeru,linux-devinfo", &devinfo, sizeof(devinfo));
    if (!err && img->has_append)
        err = fdt_setprop_string(buf, chosen, "bootargs", img->append);
    if (!err && img->initrd) {
        err = fdt_setprop_u64(buf, chosen, "linux,initrd-start", img->initrd);
        if (!err)
            err = fdt_setprop_u64(buf, chosen, "linux,initrd-end",
                                  (uint64_t)img->initrd + img->initrd_size);
    }
    if (err) {
        say(log, "cannot set /chosen: %d", err);
        return -1;
    }
    fdt_pack(buf);

    say(log, "tree: memory %u, regions kept %u resized %u added %u",
         (unsigned)fdt32_to_cpu(stat[0]), (unsigned)fdt32_to_cpu(stat[1]),
         (unsigned)fdt32_to_cpu(stat[2]), (unsigned)fdt32_to_cpu(stat[3]));
    say(log, "tree: dropped %u, devinfo words %u, %u bytes",
         (unsigned)fdt32_to_cpu(stat[4]), (unsigned)fdt32_to_cpu(devinfo),
         (unsigned)fdt_totalsize(buf));
    return 0;
}
