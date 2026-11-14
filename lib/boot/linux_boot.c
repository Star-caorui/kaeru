//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Booting Linux instead of Android.
//
// At kaeru's late init, before LK starts its boot app, the boot target
// decides: a pending 'oem boot-once' first (cleared before anything else,
// so a Linux that does not come up costs one boot, not every boot), then
// 'oem boot-target'. Only a normal boot is ever diverted; recovery,
// fastboot, charging and factory modes go to LK untouched. 'oem
// boot-target linux' also leaves alone a slot Android has not confirmed
// yet (see slot_allows_linux()); 'oem boot-once linux' is someone's
// explicit choice and boots Linux there too, with a warning.
//
// For Linux, the load region is mapped with LK's arch_mmu_map() (LK maps
// only the RAM it uses), the default extlinux.conf label on the Linux
// partition (or the one 'oem boot-once linux <label>' names) is loaded into
// it while storage is up and written back from the caches, and LK's
// lk_jump64() is taken over. LK then boots Android as it always does: boot
// and vendor_boot, its dtbo, its own fixups, all of which its asserts
// insist on. When it is about to enter the 64-bit kernel, kaeru checks the
// loaded files are intact, builds the tree for Linux from the one LK made
// (lib/boot/linux.c) and enters Linux through the same ATF call LK uses.
// Anything that fails on the way lets Android boot.
//

#include <arch/arm.h>
#include <arch/cache.h>
#include <lib/boot/ab.h>
#include <lib/boot/linux.h>
#include <lib/boot/lk_find.h>
#include <lib/boot/target.h>
#include <lib/bootmode.h>
#include <lib/common.h>
#include <lib/debug.h>
#include <lib/environment.h>
#include <lib/string.h>
#include <stage1/memory.h>

// LK's lk_jump64(entry, x0, x1, mode): logs, then enters the kernel through
// ATF, and asserts if that returns. LK passes (entry, tree, 0, 1) for a
// 64-bit kernel and (entry, machine, tags, 0) for a 32-bit one.
//
// It and arch_mmu_map() are taken from the configuration when it names
// them, and otherwise found by the function names their asserts carry
// (lib/boot/lk_find.c), so a new LK build needs no new addresses. The
// ATF call itself is made here, the way LK makes it, so it needs none.

// LK's arch_mmu_map(paddr, vaddr, flags, size): 0 on success. LK maps
// only the RAM it uses; every caller that maps DRAM passes flags 28.
typedef int (*mmu_map_t)(uint64_t paddr, uint32_t vaddr, uint32_t flags, uint32_t size);
#define LK_MMU_FLAGS_DRAM 28

static struct linux_image* pending;

static void log_line(const char* line) {
    printf("kaeru: %s\n", line);
}

// Enters a kernel through ATF's SiP call CONFIG_BOOT_LINUX_SMC_ID, with the
// registers LK sets for it: r0 the call, r1 the entry, r2 x0 (the tree),
// r3 x1, r4 the mode (1 for AArch64), and r5-r7 zero (LK's SMC wrapper
// keeps result pointers there, all NULL for this call). It only returns
// if ATF refuses.
static void jump(uint32_t entry, uint32_t x0, uint32_t x1, uint32_t mode) {
    register uint32_t r0 __asm__("r0") = CONFIG_BOOT_LINUX_SMC_ID;
    register uint32_t r1 __asm__("r1") = entry;
    register uint32_t r2 __asm__("r2") = x0;
    register uint32_t r3 __asm__("r3") = x1;
    register uint32_t r4 __asm__("r4") = mode;
    register uint32_t r5 __asm__("r5") = 0;
    register uint32_t r6 __asm__("r6") = 0;

    // r7 may be the frame pointer, so it is zeroed and restored around the
    // call rather than handed to the compiler.
    __asm__ volatile(".arch_extension sec\n"
                     "push {r7}\n"
                     "movs r7, #0\n"
                     "smc #0\n"
                     "pop {r7}\n"
                     : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6)
                     :
                     : "memory");
}

static int overlaps(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size) {
    return a < b + b_size && b < a + a_size;
}

// Replaces LK's lk_jump64().
static void kaeru_jump64(uint32_t entry, uint32_t x0, uint32_t x1, uint32_t mode) {
    struct linux_image* img = pending;

    printf("kaeru: LK enters 0x%08x with tree 0x%08x\n", (unsigned)entry, (unsigned)x0);

    if (img && mode == 1) {
        uint32_t region = CONFIG_BOOT_LINUX_LOAD_ADDRESS, size = CONFIG_BOOT_LINUX_LOAD_SIZE;
        uint32_t sum = linux_checksum(img);

        if (sum != img->sum) {
            printf("kaeru: loaded files changed (%08x, was %08x), booting Android\n",
                   (unsigned)sum, (unsigned)img->sum);
        } else if (overlaps(x0, 0x10000, region, size)) {
            printf("kaeru: LK's tree is in the load region, booting Android\n");
        } else if (linux_fixup(img, (const void*)(uintptr_t)x0, log_line)) {
            printf("kaeru: no tree for Linux, booting Android\n");
        } else {
            // Whatever LK left in the caches has to reach memory before the
            // kernel starts with them off.
            arch_clean_invalidate_cache_range(region, size);
            printf("kaeru: entering Linux at 0x%08x, tree 0x%08x\n", (unsigned)img->kernel,
                   (unsigned)img->tree);
            jump(img->kernel, img->tree, 0, 1);
            printf("kaeru: ATF did not enter Linux, booting Android\n");
        }
    }

    jump(entry, x0, x1, mode);
    printf("kaeru: ATF did not enter the kernel\n");
    for (;;)
        ;
}

// An LK function: the configured address after checking it looks like the
// start of one, else the one found by name. 0 if neither works out.
static uint32_t lk_function(const char* name, uint32_t configured) {
    struct lk_code lk;
    uint32_t addr;

    lk_code_running(&lk);
    if (configured) {
        if (!lk_is_function_start(&lk, configured)) {
            printf("kaeru: %s at 0x%08x (configured) is not a function start\n", name,
                   (unsigned)configured);
            return 0;
        }
        printf("kaeru: %s at 0x%08x (configured)\n", name, (unsigned)configured);
        return configured;
    }
    addr = lk_find_function(&lk, name, 0);
    if (addr)
        printf("kaeru: %s at 0x%08x (found)\n", name, (unsigned)addr);
    else
        printf("kaeru: %s not found\n", name);
    return addr;
}

// Turns the start of lk_jump64() into a veneer to kaeru_jump64():
// "ldr.w ip, [pc, #4]; bx ip; nop; .word kaeru_jump64", after a nop when
// the function is not word aligned, as the literal has to be.
static void take_over_jump64(uint32_t addr) {
    uint32_t start = addr;

    if (addr & 2) {
        WRITE16(addr, 0xBF00u);
        addr += 2;
    }
    WRITE32(addr + 0, 0xC004F8DFu);
    WRITE16(addr + 4, 0x4760u);
    WRITE16(addr + 6, 0xBF00u);
    WRITE32(addr + 8, (uint32_t)kaeru_jump64 | 1u);
    arch_sync_cache_range(start, addr + 12 - start);
}

#ifdef CONFIG_BOOT_LINUX_AB_CHECK
// Whether Linux may boot on this slot. A slot Android has not confirmed
// yet (after an update, or 'fastboot --set-active') loses a try on every
// boot until Android marks it successful, and Linux never will: booting
// Linux there runs the tries out and rolls the update back. The slot is
// taken as the highest priority one, as the bootloader picks it; on its
// last try it may already show no tries left, so tries are not looked
// at. Unreadable metadata counts as unconfirmed.
//
// forced is a boot-once: someone asked for Linux this time, and gets it,
// with a warning ('oem boot-once' warns when it is set, too).
static int slot_allows_linux(int forced) {
    struct ab_state st;
    const struct ab_slot* s;

    if (ab_read(&st)) {
        printf("kaeru: no valid A/B metadata in misc, booting %s\n",
               forced ? "Linux as asked (boot-once)" : "Android");
        return forced;
    }
    s = &st.slot[st.current];
    printf("kaeru: slot %c: priority %u, tries %u, successful %u\n", 'a' + st.current,
           (unsigned)s->priority, (unsigned)s->tries_remaining, (unsigned)s->successful_boot);
    if (s->successful_boot)
        return 1;
    if (forced) {
        printf("kaeru: slot %c not confirmed by Android, booting Linux anyway (boot-once)\n",
               'a' + st.current);
        video_printf(" => slot %c not confirmed, Linux anyway\n", 'a' + st.current);
        return 1;
    }
    printf("kaeru: slot %c not confirmed by Android yet, booting Android\n", 'a' + st.current);
    video_printf(" => slot not confirmed yet, Android...\n");
    return 0;
}
#endif

void linux_boot_init(void) {
    enum boot_target t, once;
    uint32_t jump64, mmu_map;
    struct linux_image* img;
    char part[40];
    char label[BOOT_ONCE_LABEL_MAX + 1];
    bootmode_t mode = get_bootmode();

    printf("kaeru: boot mode %s\n", bootmode2str(mode));
    if (mode != BOOTMODE_NORMAL)
        return;

    once = boot_once_get();
    boot_once_label(label);
    t = once != BOOT_TARGET_NONE ? once : boot_target_get();

#ifdef CONFIG_BOOT_LINUX_AB_CHECK
    if (t == BOOT_TARGET_LINUX && !slot_allows_linux(once == BOOT_TARGET_LINUX))
        return;
#endif

    if (once != BOOT_TARGET_NONE) {
        if (boot_once_set(BOOT_TARGET_NONE)) {
            printf("kaeru: cannot clear kaeru_boot_once, booting Android\n");
            return;
        }
        printf("kaeru: boot-once %s%s%s\n", boot_target_name(once), label[0] ? " " : "",
               label);
    }
    if (t != BOOT_TARGET_LINUX)
        return;

    linux_partition(part, sizeof(part));
    printf("kaeru: booting Linux from %s\n", part);
    video_printf(" => Linux (%s)...\n", part);

    jump64 = lk_function("lk_jump64", CONFIG_BOOT_LINUX_JUMP64_ADDRESS);
    mmu_map = lk_function("arch_mmu_map", CONFIG_BOOT_LINUX_MMU_MAP_ADDRESS);
    if (!jump64 || !mmu_map) {
        printf("kaeru: LK functions missing, booting Android\n");
        video_printf(" => LK not supported, Android...\n");
        return;
    }

    // Identity-map the load region; LK has not mapped it.
    if (((mmu_map_t)(mmu_map | 1))(CONFIG_BOOT_LINUX_LOAD_ADDRESS, CONFIG_BOOT_LINUX_LOAD_ADDRESS,
                                   LK_MMU_FLAGS_DRAM, CONFIG_BOOT_LINUX_LOAD_SIZE)) {
        printf("kaeru: cannot map the load region, booting Android\n");
        return;
    }

    img = malloc(sizeof(*img));
    if (!img) {
        printf("kaeru: out of memory, booting Android\n");
        return;
    }
    if (linux_load(part, label, CONFIG_BOOT_LINUX_LOAD_ADDRESS, CONFIG_BOOT_LINUX_LOAD_SIZE, 1,
                   img, log_line)) {
        free(img);
        printf("kaeru: cannot load Linux, booting Android\n");
        video_printf(" => cannot load Linux, Android...\n");
        return;
    }
    // Put the files in RAM now: LK may turn the caches off before the
    // jump without writing them back.
    arch_clean_invalidate_cache_range(CONFIG_BOOT_LINUX_LOAD_ADDRESS,
                                      img->devinfo + 0x1000 - CONFIG_BOOT_LINUX_LOAD_ADDRESS);
    take_over_jump64(jump64);
    pending = img;
}
