/* SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause) */
#ifndef LIBFDT_ENV_H
#define LIBFDT_ENV_H
/*
 * libfdt - Flat Device Tree manipulation
 * Copyright (C) 2006 David Gibson, IBM Corporation.
 * Copyright 2012 Kim Phillips, Freescale Semiconductor.
 *
 * kaeru's environment for libfdt, after dtc's libfdt_env.h. kaeru has no C
 * library and its memcpy() faults on unaligned pointers, which device trees
 * are full of, and libfdt also runs where LK has already turned the MMU
 * off (every access strongly ordered, so unaligned ones fault too). The
 * string and memory helpers libfdt uses are therefore byte-wise versions
 * private to it.
 */

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint16_t fdt16_t;
typedef uint32_t fdt32_t;
typedef uint64_t fdt64_t;

#define EXTRACT_BYTE(x, n)	((unsigned long long)((uint8_t *)&x)[n])
#define CPU_TO_FDT16(x) ((EXTRACT_BYTE(x, 0) << 8) | EXTRACT_BYTE(x, 1))
#define CPU_TO_FDT32(x) ((EXTRACT_BYTE(x, 0) << 24) | (EXTRACT_BYTE(x, 1) << 16) | \
			 (EXTRACT_BYTE(x, 2) << 8) | EXTRACT_BYTE(x, 3))
#define CPU_TO_FDT64(x) ((EXTRACT_BYTE(x, 0) << 56) | (EXTRACT_BYTE(x, 1) << 48) | \
			 (EXTRACT_BYTE(x, 2) << 40) | (EXTRACT_BYTE(x, 3) << 32) | \
			 (EXTRACT_BYTE(x, 4) << 24) | (EXTRACT_BYTE(x, 5) << 16) | \
			 (EXTRACT_BYTE(x, 6) << 8) | EXTRACT_BYTE(x, 7))

static inline uint16_t fdt16_to_cpu(fdt16_t x)
{
	return (uint16_t)CPU_TO_FDT16(x);
}
static inline fdt16_t cpu_to_fdt16(uint16_t x)
{
	return (fdt16_t)CPU_TO_FDT16(x);
}

static inline uint32_t fdt32_to_cpu(fdt32_t x)
{
	return (uint32_t)CPU_TO_FDT32(x);
}
static inline fdt32_t cpu_to_fdt32(uint32_t x)
{
	return (fdt32_t)CPU_TO_FDT32(x);
}

static inline uint64_t fdt64_to_cpu(fdt64_t x)
{
	return (uint64_t)CPU_TO_FDT64(x);
}
static inline fdt64_t cpu_to_fdt64(uint64_t x)
{
	return (fdt64_t)CPU_TO_FDT64(x);
}
#undef CPU_TO_FDT64
#undef CPU_TO_FDT32
#undef CPU_TO_FDT16
#undef EXTRACT_BYTE

static inline void *fdt_env_memcpy(void *d, const void *s, size_t n)
{
	unsigned char *dp = d;
	const unsigned char *sp = s;

	while (n--)
		*dp++ = *sp++;
	return d;
}

static inline void *fdt_env_memmove(void *d, const void *s, size_t n)
{
	unsigned char *dp = d;
	const unsigned char *sp = s;

	if (dp < sp)
		return fdt_env_memcpy(d, s, n);
	while (n--)
		dp[n] = sp[n];
	return d;
}

static inline void *fdt_env_memset(void *d, int c, size_t n)
{
	unsigned char *dp = d;

	while (n--)
		*dp++ = (unsigned char)c;
	return d;
}

static inline int fdt_env_memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *ap = a, *bp = b;

	for (; n; n--, ap++, bp++)
		if (*ap != *bp)
			return *ap - *bp;
	return 0;
}

static inline void *fdt_env_memchr(const void *s, int c, size_t n)
{
	const unsigned char *sp = s;

	for (; n; n--, sp++)
		if (*sp == (unsigned char)c)
			return (void *)sp;
	return NULL;
}

static inline size_t fdt_env_strnlen(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n])
		n++;
	return n;
}

static inline size_t fdt_env_strlen(const char *s)
{
	return fdt_env_strnlen(s, (size_t)-1);
}

static inline int fdt_env_strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (unsigned char)*a - (unsigned char)*b;
		if (!*a)
			return 0;
	}
	return 0;
}

static inline int fdt_env_strcmp(const char *a, const char *b)
{
	return fdt_env_strncmp(a, b, (size_t)-1);
}

static inline char *fdt_env_strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}

static inline char *fdt_env_strrchr(const char *s, int c)
{
	const char *last = NULL;

	for (;; s++) {
		if (*s == (char)c)
			last = s;
		if (!*s)
			return (char *)last;
	}
}

#define memcpy fdt_env_memcpy
#define memmove fdt_env_memmove
#define memset fdt_env_memset
#define memcmp fdt_env_memcmp
#define memchr fdt_env_memchr
#define strnlen fdt_env_strnlen
#define strlen fdt_env_strlen
#define strncmp fdt_env_strncmp
#define strcmp fdt_env_strcmp
#define strchr fdt_env_strchr
#define strrchr fdt_env_strrchr

#endif /* LIBFDT_ENV_H */
