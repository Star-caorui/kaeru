//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Finding LK functions by the strings they use.
//
// Hooking LK needs the addresses of a few of its functions, and those move
// with every build. Most of them name themselves somewhere, in a log line
// or an assert, so they are found the way one does by hand: find the
// string, find the code that loads its address, and walk back to the start
// of that function.
//
// MediaTek's LKs are position independent Thumb-2: a string's address is
// built as "ldr rN, [pc, #x]" (16 or 32-bit) loading an offset, then
// "add rN, pc", and the address is the offset plus that add's PC. Older
// LKs load the absolute address instead; both are recognised. The start
// of the function is the closest instruction before the reference that
// saves LR (push {..., lr} or stmdb sp!, {..., lr}) and that some bl
// calls, which keeps literal data from passing for code.
//

#include <lib/boot/lk_find.h>

#ifdef LK_FIND_HOST_TEST
#include <string.h>
#else
#include <arch/arm.h>
#include <lib/common.h>
#include <lib/string.h>
#endif

// How far after the ldr the add may come, and how far a reference may be
// from the start of its function.
#define ADD_WINDOW 16
#define FUNC_WINDOW 0x2000

static uint16_t rd16(const struct lk_code* lk, uint32_t off) {
    return (uint16_t)(lk->p[off] | lk->p[off + 1] << 8);
}

static uint32_t rd32(const struct lk_code* lk, uint32_t off) {
    return (uint32_t)lk->p[off] | (uint32_t)lk->p[off + 1] << 8 | (uint32_t)lk->p[off + 2] << 16 |
           (uint32_t)lk->p[off + 3] << 24;
}

// Where the string is, as an address, or 0. It has to start a string: the
// byte before it is a NUL.
static uint32_t find_string(const struct lk_code* lk, const char* str, int prefix) {
    uint32_t n = 0, off;

    while (str[n])
        n++;
    if (!n)
        return 0;
    for (off = 1; off + n + 1 <= lk->size; off++) {
        uint32_t i;

        if (lk->p[off - 1] != 0 || lk->p[off] != (uint8_t)str[0])
            continue;
        for (i = 1; i < n && lk->p[off + i] == (uint8_t)str[i]; i++)
            ;
        if (i == n && (prefix || lk->p[off + n] == 0))
            return lk->base + off;
    }
    return 0;
}

// "ldr rt, [pc, #imm]" at off: 1 with *rt and *lit (the literal's offset)
// and *len (2 or 4), else 0.
static int ldr_literal(const struct lk_code* lk, uint32_t off, int* rt, uint32_t* lit, int* len) {
    uint16_t h = rd16(lk, off);
    uint32_t pc = (lk->base + off + 4) & ~3u, addr;

    if ((h & 0xF800) == 0x4800) {
        *rt = (h >> 8) & 7;
        addr = pc + (uint32_t)(h & 0xFF) * 4;
        *len = 2;
    } else if ((h & 0xFF7F) == 0xF85F && off + 4 <= lk->size) {
        uint16_t h2 = rd16(lk, off + 2);
        uint32_t imm = h2 & 0xFFF;

        *rt = h2 >> 12;
        addr = (h & 0x80) ? pc + imm : pc - imm;
        *len = 4;
    } else {
        return 0;
    }
    if (addr < lk->base || addr - lk->base + 4 > lk->size)
        return 0;
    *lit = addr - lk->base;
    return 1;
}

// Whether the code at off refers to target.
static int refers_to(const struct lk_code* lk, uint32_t off, uint32_t target) {
    uint32_t lit, val, a;
    int rt, len;

    if (!ldr_literal(lk, off, &rt, &lit, &len))
        return 0;
    val = rd32(lk, lit);
    if (val == target)
        return 1;
    // "add rt, pc": 0100 0100 D 1111 ddd, rt = D:ddd.
    for (a = off + len; a < off + len + 2 * ADD_WINDOW && a + 2 <= lk->size; a += 2) {
        uint16_t h = rd16(lk, a);

        if ((h & 0xFF78) == 0x4478 && (int)(((h >> 4) & 8) | (h & 7)) == rt)
            return val + lk->base + a + 4 == target;
    }
    return 0;
}

static int saves_lr(const struct lk_code* lk, uint32_t off) {
    uint16_t h = rd16(lk, off);

    if ((h & 0xFF00) == 0xB500)
        return 1; // push {..., lr}
    return h == 0xE92D && off + 4 <= lk->size && (rd16(lk, off + 2) & 0x4000);
}

// Whether a bl (encoding T1) at some halfword calls addr.
static int is_bl_target(const struct lk_code* lk, uint32_t addr) {
    for (uint32_t off = 0; off + 4 <= lk->size; off += 2) {
        uint16_t hi = rd16(lk, off), lo;
        uint32_t s, j1, j2, i1, i2;
        int32_t imm;

        if ((hi & 0xF800) != 0xF000)
            continue;
        lo = rd16(lk, off + 2);
        if ((lo & 0xD000) != 0xD000)
            continue;
        s = (hi >> 10) & 1;
        j1 = (lo >> 13) & 1;
        j2 = (lo >> 11) & 1;
        i1 = !(j1 ^ s);
        i2 = !(j2 ^ s);
        imm = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) | ((uint32_t)(hi & 0x3FF) << 12) |
                        ((uint32_t)(lo & 0x7FF) << 1));
        if (s)
            imm |= (int32_t)0xFE000000;
        if (lk->base + off + 4 + (uint32_t)imm == addr)
            return 1;
    }
    return 0;
}

int lk_is_function_start(const struct lk_code* lk, uint32_t addr) {
    uint32_t off = addr - lk->base;

    if (addr < lk->base || (addr & 1) || off + 4 > lk->size)
        return 0;
    return saves_lr(lk, off) && is_bl_target(lk, addr);
}

static uint32_t function_of(const struct lk_code* lk, uint32_t ref) {
    for (uint32_t back = 0; back <= FUNC_WINDOW && back <= ref; back += 2)
        if (lk_is_function_start(lk, lk->base + ref - back))
            return lk->base + ref - back;
    return 0;
}

uint32_t lk_find_function(const struct lk_code* lk, const char* str, int prefix) {
    uint32_t target = find_string(lk, str, prefix), func = 0;

    if (!target)
        return 0;
    for (uint32_t off = 0; off + 2 <= lk->size; off += 2) {
        uint32_t f;

        if (!refers_to(lk, off, target))
            continue;
        f = function_of(lk, off);
        if (!f || (func && f != func))
            return 0;
        func = f;
    }
    return func;
}

#ifndef LK_FIND_HOST_TEST
void lk_code_running(struct lk_code* lk) {
    lk->p = (const uint8_t*)(uintptr_t)LK_START;
    lk->base = LK_START;
    lk->size = LK_END - LK_START;
}
#endif
