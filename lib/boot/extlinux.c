//
// SPDX-FileCopyrightText: 2026 Betterr <51264180+Star-caorui@users.noreply.github.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// extlinux.conf parser.
//
// Follows what U-Boot's pxe_utils accepts for sysboot, which is what
// distributions write for it:
//
//   DEFAULT <label>           TIMEOUT <tenths of a second>
//   LABEL <name>              MENU LABEL <text>       MENU DEFAULT
//   LINUX|KERNEL <path>       INITRD <path>
//   FDT|DEVICETREE <path>     FDTDIR|DEVICETREEDIR <dir>
//   APPEND <command line>     FDTOVERLAYS <paths>     LOCALBOOT <n>
//
// Keywords are case-insensitive, label names are not (U-Boot compares them
// with strcmp). A line whose first non-blank character is '#' is a comment;
// '#' elsewhere is kept, so it can appear in APPEND. MENU TITLE, PROMPT,
// TEXT HELP ... ENDTEXT and other menu keywords are skipped silently,
// anything else unknown is counted. INCLUDE is not followed.
//
// Nothing here allocates or keeps pointers in static data: kaeru runs
// relocated and only its GOT is fixed up.
//

#include <lib/boot/extlinux.h>

#ifdef EXTLINUX_HOST_TEST
#include <string.h>
#include <strings.h>
#else
#include <lib/string.h>
#endif

// Byte at a time: kaeru's optimised memcpy faults on unaligned pointers,
// and these strings start wherever the file put them.
static void copy_bytes(char* dst, const char* src, size_t n) {
    for (size_t i = 0; i < n; i++)
        dst[i] = src[i];
}

static int is_blank(char c) {
    return c == ' ' || c == '\t';
}

// Copies [s, s + n) into out as a NUL-terminated string.
static int set_field(char* out, size_t out_len, const char* s, size_t n) {
    if (n >= out_len)
        return EXTLINUX_ETOOLONG;
    copy_bytes(out, s, n);
    out[n] = '\0';
    return EXTLINUX_OK;
}

// The first blank-separated word of [s, s + n).
static size_t word_len(const char* s, size_t n) {
    size_t i = 0;

    while (i < n && !is_blank(s[i]))
        i++;
    return i;
}

static int keyword_is(const char* kw, size_t kw_len, const char* name) {
    size_t n = strlen(name);

    return kw_len == n && strncasecmp(kw, name, n) == 0;
}

static int parse_uint(const char* s, size_t n, int* out) {
    int v = 0;

    if (n == 0)
        return EXTLINUX_EBADVAL;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9' || v > 100000000)
            return EXTLINUX_EBADVAL;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return EXTLINUX_OK;
}

int extlinux_parse(const char* buf, size_t len, struct extlinux_config* cfg) {
    char default_name[EXTLINUX_NAME_MAX];
    char global_append[EXTLINUX_APPEND_MAX];
    int has_default = 0, has_global_append = 0, menu_default = -1, in_text = 0;
    struct extlinux_entry* cur = NULL;
    size_t pos = 0;
    int line = 0;
    int err;

    memset(cfg, 0, sizeof(*cfg));
    default_name[0] = '\0';
    global_append[0] = '\0';

    while (pos < len) {
        const char* s = buf + pos;
        size_t n = 0, kw_len;
        const char* val;
        size_t val_len;

        while (pos + n < len && s[n] != '\n')
            n++;
        pos += n + 1;
        line++;

        // Trim both ends; '\r' covers CRLF files.
        while (n && (is_blank(s[n - 1]) || s[n - 1] == '\r'))
            n--;
        while (n && is_blank(*s)) {
            s++;
            n--;
        }
        if (!n || *s == '#')
            continue;

        kw_len = word_len(s, n);

        // TEXT HELP ... ENDTEXT holds free text, not keywords.
        if (in_text) {
            if (keyword_is(s, kw_len, "endtext"))
                in_text = 0;
            continue;
        }
        val = s + kw_len;
        val_len = n - kw_len;
        while (val_len && is_blank(*val)) {
            val++;
            val_len--;
        }

        err = EXTLINUX_OK;
        if (keyword_is(s, kw_len, "label")) {
            if (cfg->count == EXTLINUX_MAX_ENTRIES) {
                err = EXTLINUX_ETOOMANY;
            } else if (!val_len) {
                err = EXTLINUX_EBADVAL;
            } else {
                cur = &cfg->entries[cfg->count++];
                err = set_field(cur->label, sizeof(cur->label), val, val_len);
            }
        } else if (keyword_is(s, kw_len, "menu")) {
            size_t sub = word_len(val, val_len);
            const char* arg = val + sub;
            size_t arg_len = val_len - sub;

            while (arg_len && is_blank(*arg)) {
                arg++;
                arg_len--;
            }
            if (cur && keyword_is(val, sub, "label")) {
                // Display only: a long one is cut short, at a UTF-8
                // character boundary, rather than failing the file.
                if (arg_len >= sizeof(cur->menu_label)) {
                    arg_len = sizeof(cur->menu_label) - 1;
                    while (arg_len && (arg[arg_len] & 0xc0) == 0x80)
                        arg_len--;
                }
                err = set_field(cur->menu_label, sizeof(cur->menu_label), arg, arg_len);
            }
            else if (cur && keyword_is(val, sub, "default"))
                menu_default = (int)(cur - cfg->entries);
            // MENU TITLE, MENU BACKGROUND, ...: display only.
        } else if (keyword_is(s, kw_len, "linux") || keyword_is(s, kw_len, "kernel")) {
            if (cur)
                err = set_field(cur->kernel, sizeof(cur->kernel), val, val_len);
            else
                cfg->unknown++;
        } else if (keyword_is(s, kw_len, "initrd")) {
            if (cur)
                err = set_field(cur->initrd, sizeof(cur->initrd), val, val_len);
            else
                cfg->unknown++;
        } else if (keyword_is(s, kw_len, "fdt") || keyword_is(s, kw_len, "devicetree")) {
            if (cur)
                err = set_field(cur->fdt, sizeof(cur->fdt), val, val_len);
            else
                cfg->unknown++;
        } else if (keyword_is(s, kw_len, "fdtdir") || keyword_is(s, kw_len, "devicetreedir")) {
            if (cur)
                err = set_field(cur->fdtdir, sizeof(cur->fdtdir), val, val_len);
            else
                cfg->unknown++;
        } else if (keyword_is(s, kw_len, "fdtoverlays") ||
                   keyword_is(s, kw_len, "devicetree-overlay")) {
            if (cur)
                cur->has_overlays = 1;
            else
                cfg->unknown++;
        } else if (keyword_is(s, kw_len, "localboot")) {
            if (cur)
                cur->localboot = 1;
            else
                cfg->unknown++;
        } else if (keyword_is(s, kw_len, "append")) {
            if (cur) {
                err = set_field(cur->append, sizeof(cur->append), val, val_len);
                cur->has_append = 1;
            } else {
                err = set_field(global_append, sizeof(global_append), val, val_len);
                has_global_append = 1;
            }
        } else if (keyword_is(s, kw_len, "default")) {
            // Syslinux allows "DEFAULT <label> <options>"; the label is
            // the first word.
            size_t w = word_len(val, val_len);

            if (!w)
                err = EXTLINUX_EBADVAL;
            else
                err = set_field(default_name, sizeof(default_name), val, w);
            has_default = 1;
        } else if (keyword_is(s, kw_len, "timeout")) {
            err = parse_uint(val, val_len, &cfg->timeout);
        } else if (keyword_is(s, kw_len, "text")) {
            in_text = 1;
        } else if (keyword_is(s, kw_len, "prompt")) {
            // Menu display only.
        } else {
            cfg->unknown++;
        }

        if (err) {
            cfg->err_line = line;
            return err;
        }
    }

    if (!cfg->count)
        return EXTLINUX_ENOLABEL;

    if (has_global_append) {
        for (int i = 0; i < cfg->count; i++) {
            struct extlinux_entry* e = &cfg->entries[i];

            if (!e->has_append) {
                copy_bytes(e->append, global_append, sizeof(e->append));
                e->has_append = 1;
            }
        }
    }

    cfg->default_index = 0;
    if (menu_default >= 0) {
        cfg->default_index = menu_default;
    } else if (has_default) {
        int i;

        for (i = 0; i < cfg->count; i++)
            if (!strcmp(cfg->entries[i].label, default_name))
                break;
        if (i < cfg->count)
            cfg->default_index = i;
        else
            cfg->default_missing = 1;
    }

    return EXTLINUX_OK;
}

const struct extlinux_entry* extlinux_default(const struct extlinux_config* cfg, int* err) {
    const struct extlinux_entry* e = &cfg->entries[cfg->default_index];

    if (e->localboot || !e->kernel[0]) {
        *err = EXTLINUX_ENOKERNEL;
        return NULL;
    }
    *err = EXTLINUX_OK;
    return e;
}

int extlinux_resolve(const char* cfg_path, const char* file, char* out, size_t out_len) {
    size_t dir_len = 0, file_len = strlen(file);

    if (file[0] != '/') {
        const char* slash = NULL;

        for (const char* p = cfg_path; *p; p++)
            if (*p == '/')
                slash = p;
        if (slash)
            dir_len = (size_t)(slash - cfg_path) + 1;
    }
    if (dir_len + file_len >= out_len)
        return EXTLINUX_ETOOLONG;
    copy_bytes(out, cfg_path, dir_len);
    copy_bytes(out + dir_len, file, file_len + 1);
    return EXTLINUX_OK;
}

int extlinux_fdtdir_path(const char* cfg_path, const char* fdtdir, const char* fdtfile, char* out,
                         size_t out_len) {
    size_t n, f = strlen(fdtfile);
    int err = extlinux_resolve(cfg_path, fdtdir, out, out_len);

    if (err)
        return err;
    n = strlen(out);
    if (n && out[n - 1] != '/') {
        if (n + 1 >= out_len)
            return EXTLINUX_ETOOLONG;
        out[n++] = '/';
    }
    if (n + f >= out_len)
        return EXTLINUX_ETOOLONG;
    copy_bytes(out + n, fdtfile, f + 1);
    return EXTLINUX_OK;
}

const char* extlinux_config_path(int index) {
    switch (index) {
        case 0:
            return "/extlinux/extlinux.conf";
        case 1:
            return "/boot/extlinux/extlinux.conf";
        default:
            return NULL;
    }
}

const char* extlinux_strerror(int err) {
    switch (err) {
        case EXTLINUX_OK:
            return "success";
        case EXTLINUX_ETOOLONG:
            return "value too long";
        case EXTLINUX_ETOOMANY:
            return "too many labels";
        case EXTLINUX_ENOLABEL:
            return "no LABEL";
        case EXTLINUX_ENOKERNEL:
            return "default label has no kernel";
        case EXTLINUX_EBADVAL:
            return "bad value";
        default:
            return "unknown error";
    }
}
