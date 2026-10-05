// Standard library, native half.
//
// `math` and `strings` are ordinary Solis modules; whatever they can't say in
// Solis they declare extern, and this supplies it. Registered under qualified
// names ("math.sqrt", "strings.upper") so they can't collide with the host's,
// which are bare.
//
// Strings are UTF-8 and indexed by character. Case mapping covers ASCII,
// Latin-1 Supplement, Latin Extended-A, Greek and Cyrillic by range
// arithmetic (code rather than tables), and passes anything else through
// unchanged.

#include "solis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double want_f(sl_vm *vm, int argc, sl_value *argv, int i, const char *who) {
    if (i >= argc) { sl_fail(vm, who); return 0.0; }
    if (argv[i].type == SL_FLOAT) return argv[i].as.f;
    if (argv[i].type == SL_INT) return (double)argv[i].as.i;
    sl_fail(vm, who);
    return 0.0;
}

static int64_t want_i(sl_vm *vm, int argc, sl_value *argv, int i, const char *who) {
    if (i >= argc || argv[i].type != SL_INT) { sl_fail(vm, who); return 0; }
    return argv[i].as.i;
}

static const char *want_s(sl_vm *vm, int argc, sl_value *argv, int i, const char *who,
                          size_t *n) {
    if (i >= argc) { sl_fail(vm, who); *n = 0; return NULL; }
    if (argv[i].type != SL_OBJ || !argv[i].as.o) { sl_fail(vm, who); *n = 0; return NULL; }
    return sl_as_str(argv[i], n);
}

// Byte offset of character `ci`, clamped to the ends.
static size_t char_offset(const char *p, size_t n, int64_t ci) {
    if (ci <= 0) return 0;
    int64_t seen = 0;
    for (size_t k = 0; k < n; k++) {
        if ((p[k] & 0xC0) != 0x80) {
            if (seen == ci) return k;
            seen++;
        }
    }
    return n;
}

static int64_t char_count(const char *p, size_t n) {
    int64_t c = 0;
    for (size_t k = 0; k < n; k++) if ((p[k] & 0xC0) != 0x80) c++;
    return c;
}

// The substring from character `a` up to character `b`.
static sl_value char_slice(sl_vm *vm, const char *p, size_t n, int64_t a, int64_t b) {
    if (b <= a) return sl_cstr(vm, "");
    size_t s0 = char_offset(p, n, a);
    size_t s1 = char_offset(p, n, b);
    if (s1 < s0) s1 = s0;
    return sl_str(vm, p + s0, s1 - s0);
}

static const char *mem_find(const char *hay, size_t hn, const char *nd, size_t nn) {
    if (nn == 0) return hay;
    if (nn > hn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(hay + i, nd, nn) == 0) return hay + i;
    return NULL;
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Decode one code point, `w` gets its byte width. Invalid sequences decode as
// their lead byte, so bad input survives rather than being mangled.
static uint32_t utf8_next(const char *p, size_t n, size_t i, size_t *w) {
    unsigned char c = (unsigned char)p[i];
    if (c < 0x80) { *w = 1; return c; }
    if ((c & 0xE0) == 0xC0 && i + 1 < n) { *w = 2; return ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)p[i+1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && i + 2 < n) { *w = 3; return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)((unsigned char)p[i+1] & 0x3F) << 6) | ((unsigned char)p[i+2] & 0x3F); }
    if ((c & 0xF8) == 0xF0 && i + 3 < n) { *w = 4; return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)((unsigned char)p[i+1] & 0x3F) << 12) | ((uint32_t)((unsigned char)p[i+2] & 0x3F) << 6) | ((unsigned char)p[i+3] & 0x3F); }
    *w = 1;
    return c;
}

static size_t utf8_put(char *out, uint32_t cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// Has a case, enough for the final-sigma rule.
static bool is_cased(uint32_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= 0x00C0 && c <= 0x024F && c != 0x00D7 && c != 0x00F7) ||
           (c >= 0x0370 && c <= 0x03FF) ||
           (c >= 0x0400 && c <= 0x04FF);
}

static uint32_t to_upper_cp(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;

    // Latin-1 Supplement.
    if (c >= 0x00E0 && c <= 0x00FE && c != 0x00F7) return c - 32;
    if (c == 0x00FF) return 0x0178;         // y diaeresis
    if (c == 0x00B5) return 0x039C;         // micro sign -> capital mu

    // Latin Extended-A, which alternates upper/lower in pairs. The pairing
    // flips partway through the block, and a few points are exceptions.
    if (c >= 0x0100 && c <= 0x0137) return (c & 1) ? c - 1 : c;
    if (c == 0x0138) return c;              // kra has no uppercase
    if (c >= 0x0139 && c <= 0x0148) return (c & 1) ? c : c - 1;
    if (c == 0x0149) return c;              // handled by the expansion table
    if (c >= 0x014A && c <= 0x0177) return (c & 1) ? c - 1 : c;
    if (c >= 0x0179 && c <= 0x017E) return (c & 1) ? c : c - 1;
    if (c == 0x017F) return 'S';            // long s
    if (c == 0x0131) return 'I';            // dotless i

    // Greek.
    if (c >= 0x03B1 && c <= 0x03C9 && c != 0x03C2) return c - 32;
    if (c == 0x03C2) return 0x03A3;         // final sigma -> capital sigma

    // Cyrillic.
    if (c >= 0x0430 && c <= 0x044F) return c - 32;
    if (c >= 0x0450 && c <= 0x045F) return c - 80;

    return c;
}

static uint32_t to_lower_cp(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;

    if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) return c + 32;
    if (c == 0x0178) return 0x00FF;

    if (c >= 0x0100 && c <= 0x0137) return (c & 1) ? c : c + 1;
    if (c >= 0x0139 && c <= 0x0148) return (c & 1) ? c + 1 : c;
    if (c >= 0x014A && c <= 0x0177) return (c & 1) ? c : c + 1;
    if (c >= 0x0179 && c <= 0x017E) return (c & 1) ? c + 1 : c;

    if (c >= 0x0391 && c <= 0x03A9) return c + 32;

    if (c >= 0x0410 && c <= 0x042F) return c + 32;
    if (c >= 0x0400 && c <= 0x040F) return c + 80;

    return c;
}

// A handful of code points uppercase into several, so this can't be done in
// place. NULL if there's no expansion.
static const char *upper_expansion(uint32_t c) {
    switch (c) {
        case 0x00DF: return "SS";           // sharp s
        case 0x0149: return "\xCA\xBCN";    // n preceded by apostrophe
        case 0xFB00: return "FF";
        case 0xFB01: return "FI";
        case 0xFB02: return "FL";
        default: return NULL;
    }
}

static sl_value m_to_float(sl_vm *vm, int argc, sl_value *argv) {
    return sl_float(want_f(vm, argc, argv, 0, "toFloat expects a float"));
}

static sl_value m_to_int(sl_vm *vm, int argc, sl_value *argv) {
    // Rust `as i64` truncates toward zero.
    return sl_int((int64_t)want_f(vm, argc, argv, 0, "toInt expects a float"));
}

static sl_value m_floor(sl_vm *vm, int argc, sl_value *argv) {
    return sl_int((int64_t)floor(want_f(vm, argc, argv, 0, "floor expects a float")));
}

static sl_value m_ceil(sl_vm *vm, int argc, sl_value *argv) {
    return sl_int((int64_t)ceil(want_f(vm, argc, argv, 0, "ceil expects a float")));
}

static sl_value m_round(sl_vm *vm, int argc, sl_value *argv) {
    // Rust rounds half away from zero, which is what round() does.
    return sl_int((int64_t)round(want_f(vm, argc, argv, 0, "round expects a float")));
}

static sl_value m_sqrt(sl_vm *vm, int argc, sl_value *argv) {
    double x = want_f(vm, argc, argv, 0, "sqrt expects a float");
    if (x < 0) { sl_fail(vm, "sqrt of a negative number"); return sl_void(); }
    return sl_float(sqrt(x));
}

static sl_value m_pow(sl_vm *vm, int argc, sl_value *argv) {
    double b = want_f(vm, argc, argv, 0, "pow expects a float");
    double e = want_f(vm, argc, argv, 1, "pow expects a float");
    return sl_float(pow(b, e));
}

static sl_value m_exp(sl_vm *vm, int argc, sl_value *argv) {
    return sl_float(exp(want_f(vm, argc, argv, 0, "exp expects a float")));
}

static sl_value m_ln(sl_vm *vm, int argc, sl_value *argv) {
    double x = want_f(vm, argc, argv, 0, "ln expects a float");
    if (x <= 0) { sl_fail(vm, "ln of a non-positive number"); return sl_void(); }
    return sl_float(log(x));
}

static sl_value m_log10(sl_vm *vm, int argc, sl_value *argv) {
    double x = want_f(vm, argc, argv, 0, "log10 expects a float");
    if (x <= 0) { sl_fail(vm, "log10 of a non-positive number"); return sl_void(); }
    return sl_float(log10(x));
}

static sl_value m_sin(sl_vm *vm, int argc, sl_value *argv) {
    return sl_float(sin(want_f(vm, argc, argv, 0, "sin expects a float")));
}

static sl_value m_cos(sl_vm *vm, int argc, sl_value *argv) {
    return sl_float(cos(want_f(vm, argc, argv, 0, "cos expects a float")));
}

static sl_value m_tan(sl_vm *vm, int argc, sl_value *argv) {
    return sl_float(tan(want_f(vm, argc, argv, 0, "tan expects a float")));
}

static sl_value m_atan2(sl_vm *vm, int argc, sl_value *argv) {
    double y = want_f(vm, argc, argv, 0, "atan2 expects a float");
    double x = want_f(vm, argc, argv, 1, "atan2 expects a float");
    return sl_float(atan2(y, x));
}

static sl_value m_abs(sl_vm *vm, int argc, sl_value *argv) {
    return sl_float(fabs(want_f(vm, argc, argv, 0, "abs expects a float")));
}

static sl_value m_abs_int(sl_vm *vm, int argc, sl_value *argv) {
    int64_t n = want_i(vm, argc, argv, 0, "absInt expects an int");
    return sl_int(n < 0 ? -n : n);
}

static sl_value s_len(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "len expects a string", &n);
    return sl_int(p ? char_count(p, n) : 0);
}

static sl_value s_sub(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "sub expects a string", &n);
    if (!p) return sl_void();
    int64_t a = want_i(vm, argc, argv, 1, "sub expects an int");
    int64_t b = want_i(vm, argc, argv, 2, "sub expects an int");
    int64_t cn = char_count(p, n);
    if (a < 0) a = 0;
    if (a > cn) a = cn;
    if (b < 0) b = 0;
    if (b > cn) b = cn;
    return char_slice(vm, p, n, a, b);
}

static sl_value s_at(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "at expects a string", &n);
    if (!p) return sl_void();
    int64_t i = want_i(vm, argc, argv, 1, "at expects an int");
    int64_t cn = char_count(p, n);
    if (i < 0 || i >= cn) return sl_cstr(vm, "");
    return char_slice(vm, p, n, i, i + 1);
}

static sl_value s_index_of(sl_vm *vm, int argc, sl_value *argv) {
    size_t hn, nn;
    const char *h = want_s(vm, argc, argv, 0, "indexOf expects a string", &hn);
    const char *d = want_s(vm, argc, argv, 1, "indexOf expects a string", &nn);
    if (!h || !d) return sl_void();
    const char *at = mem_find(h, hn, d, nn);
    if (!at) return sl_int(-1);
    return sl_int(char_count(h, (size_t)(at - h)));
}

static sl_value s_contains(sl_vm *vm, int argc, sl_value *argv) {
    size_t hn, nn;
    const char *h = want_s(vm, argc, argv, 0, "contains expects a string", &hn);
    const char *d = want_s(vm, argc, argv, 1, "contains expects a string", &nn);
    if (!h || !d) return sl_void();
    return sl_bool(mem_find(h, hn, d, nn) != NULL);
}

static sl_value s_starts_with(sl_vm *vm, int argc, sl_value *argv) {
    size_t hn, nn;
    const char *h = want_s(vm, argc, argv, 0, "startsWith expects a string", &hn);
    const char *d = want_s(vm, argc, argv, 1, "startsWith expects a string", &nn);
    if (!h || !d) return sl_void();
    return sl_bool(nn <= hn && memcmp(h, d, nn) == 0);
}

static sl_value s_ends_with(sl_vm *vm, int argc, sl_value *argv) {
    size_t hn, nn;
    const char *h = want_s(vm, argc, argv, 0, "endsWith expects a string", &hn);
    const char *d = want_s(vm, argc, argv, 1, "endsWith expects a string", &nn);
    if (!h || !d) return sl_void();
    return sl_bool(nn <= hn && memcmp(h + hn - nn, d, nn) == 0);
}

static sl_value map_case(sl_vm *vm, const char *p, size_t n, bool up) {
    // No mapping here grows a code point past 3 bytes.
    size_t cap = n * 3 + 4;
    char *buf = (char *)malloc(cap);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }

    size_t out = 0, i = 0;
    while (i < n) {
        size_t w;
        uint32_t c = utf8_next(p, n, i, &w);

        if (up) {
            const char *ex = upper_expansion(c);
            if (ex) {
                size_t en = strlen(ex);
                memcpy(buf + out, ex, en);
                out += en;
                i += w;
                continue;
            }
            out += utf8_put(buf + out, to_upper_cp(c));
        } else if (c == 0x03A3) {
            // Final sigma: preceded by a cased letter and not followed by
            // one. The only context-sensitive rule in the blocks covered
            // here. A standalone sigma is the non-final form.
            size_t aw, bw;
            bool after = false;
            if (i > 0) {
                size_t j = i - 1;
                while (j > 0 && ((unsigned char)p[j] & 0xC0) == 0x80) j--;
                after = is_cased(utf8_next(p, n, j, &aw));
            }
            bool before = i + w < n && is_cased(utf8_next(p, n, i + w, &bw));
            out += utf8_put(buf + out, (after && !before) ? 0x03C2 : 0x03C3);
        } else {
            out += utf8_put(buf + out, to_lower_cp(c));
        }
        i += w;
    }

    sl_value r = sl_str(vm, buf, out);
    free(buf);
    return r;
}

static sl_value s_upper(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "upper expects a string", &n);
    return p ? map_case(vm, p, n, true) : sl_void();
}

static sl_value s_lower(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "lower expects a string", &n);
    return p ? map_case(vm, p, n, false) : sl_void();
}

static sl_value s_trim(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "trim expects a string", &n);
    if (!p) return sl_void();
    size_t a = 0, b = n;
    while (a < b && is_space(p[a])) a++;
    while (b > a && is_space(p[b - 1])) b--;
    return sl_str(vm, p + a, b - a);
}

static sl_value s_repeat(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "repeat expects a string", &n);
    if (!p) return sl_void();
    int64_t times = want_i(vm, argc, argv, 1, "repeat expects an int");
    if (times < 0) times = 0;
    // A guard, because repeat(s, 1e9) would otherwise take the host down.
    if (char_count(p, n) * times > 10000000) {
        sl_fail(vm, "repeat would produce more than 10 million characters");
        return sl_void();
    }
    size_t total = n * (size_t)times;
    char *buf = (char *)malloc(total ? total : 1);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }
    for (int64_t i = 0; i < times; i++) memcpy(buf + (size_t)i * n, p, n);
    sl_value out = sl_str(vm, buf, total);
    free(buf);
    return out;
}

static sl_value s_replace(sl_vm *vm, int argc, sl_value *argv) {
    size_t hn, fn, tn;
    const char *h = want_s(vm, argc, argv, 0, "replace expects a string", &hn);
    const char *f = want_s(vm, argc, argv, 1, "replace expects a string", &fn);
    const char *t = want_s(vm, argc, argv, 2, "replace expects a string", &tn);
    if (!h || !f || !t) return sl_void();
    if (fn == 0) { sl_fail(vm, "replace needs a non-empty pattern"); return sl_void(); }

    size_t cap = hn + 16, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }
    size_t i = 0;
    while (i < hn) {
        if (i + fn <= hn && memcmp(h + i, f, fn) == 0) {
            if (len + tn > cap) {
                cap = (len + tn) * 2;
                char *nb = (char *)realloc(buf, cap);
                if (!nb) { free(buf); sl_fail(vm, "out of memory"); return sl_void(); }
                buf = nb;
            }
            memcpy(buf + len, t, tn);
            len += tn;
            i += fn;
        } else {
            if (len + 1 > cap) {
                cap *= 2;
                char *nb = (char *)realloc(buf, cap);
                if (!nb) { free(buf); sl_fail(vm, "out of memory"); return sl_void(); }
                buf = nb;
            }
            buf[len++] = h[i++];
        }
    }
    sl_value out = sl_str(vm, buf, len);
    free(buf);
    return out;
}

static sl_value s_reverse(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "reverse expects a string", &n);
    if (!p) return sl_void();
    char *buf = (char *)malloc(n ? n : 1);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }
    // Reverse by character, not by byte, so UTF-8 survives the trip.
    size_t out = 0, i = n;
    while (i > 0) {
        size_t start = i - 1;
        while (start > 0 && (p[start] & 0xC0) == 0x80) start--;
        size_t w = i - start;
        memcpy(buf + out, p + start, w);
        out += w;
        i = start;
    }
    sl_value r = sl_str(vm, buf, out);
    free(buf);
    return r;
}

static sl_value s_split(sl_vm *vm, int argc, sl_value *argv) {
    size_t hn, sn;
    const char *h = want_s(vm, argc, argv, 0, "split expects a string", &hn);
    const char *sep = want_s(vm, argc, argv, 1, "split expects a string", &sn);
    if (!h || !sep) return sl_void();

    sl_value arr = sl_array(vm, 4);
    if (arr.type != SL_OBJ) return sl_void();

    if (sn == 0) {
        // An empty separator splits into characters.
        size_t i = 0;
        while (i < hn) {
            size_t w = 1;
            while (i + w < hn && (h[i + w] & 0xC0) == 0x80) w++;
            if (!sl_array_push(vm, arr, sl_str(vm, h + i, w))) { sl_release(vm, arr); return sl_void(); }
            i += w;
        }
        return arr;
    }

    size_t start = 0;
    for (;;) {
        const char *at = mem_find(h + start, hn - start, sep, sn);
        if (!at) {
            if (!sl_array_push(vm, arr, sl_str(vm, h + start, hn - start))) {
                sl_release(vm, arr); return sl_void();
            }
            break;
        }
        size_t off = (size_t)(at - h);
        if (!sl_array_push(vm, arr, sl_str(vm, h + start, off - start))) {
            sl_release(vm, arr); return sl_void();
        }
        start = off + sn;
    }
    return arr;
}

static sl_value s_join(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 1 || argv[0].type != SL_OBJ) { sl_fail(vm, "join expects an array"); return sl_void(); }
    size_t sn;
    const char *sep = want_s(vm, argc, argv, 1, "join expects a string", &sn);
    if (!sep) return sl_void();

    int64_t count = sl_array_len(argv[0]);
    size_t cap = 64, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }

    for (int64_t i = 0; i < count; i++) {
        // Elements render the way `print` renders them, so joining an array
        // of non-strings behaves like the Rust runtime.
        sl_value part = sl_to_string(vm, sl_array_get(argv[0], i));
        size_t pn;
        const char *pp = sl_as_str(part, &pn);
        size_t add = pn + (i ? sn : 0);
        if (len + add > cap) {
            while (len + add > cap) cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); sl_release(vm, part); sl_fail(vm, "out of memory"); return sl_void(); }
            buf = nb;
        }
        if (i) { memcpy(buf + len, sep, sn); len += sn; }
        memcpy(buf + len, pp, pn);
        len += pn;
        sl_release(vm, part);
    }
    sl_value out = sl_str(vm, buf, len);
    free(buf);
    return out;
}

static sl_value s_chars(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "chars expects a string", &n);
    if (!p) return sl_void();
    sl_value arr = sl_array(vm, char_count(p, n));
    if (arr.type != SL_OBJ) return sl_void();
    size_t i = 0;
    while (i < n) {
        size_t w = 1;
        while (i + w < n && (p[i + w] & 0xC0) == 0x80) w++;
        if (!sl_array_push(vm, arr, sl_str(vm, p + i, w))) { sl_release(vm, arr); return sl_void(); }
        i += w;
    }
    return arr;
}

static sl_value s_from_int(sl_vm *vm, int argc, sl_value *argv) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld",
                     (long long)want_i(vm, argc, argv, 0, "fromInt expects an int"));
    return sl_str(vm, buf, n > 0 ? (size_t)n : 0);
}

static sl_value s_from_float(sl_vm *vm, int argc, sl_value *argv) {
    // sl_to_string already matches the Rust runtime, trailing ".0" included.
    return sl_to_string(vm, sl_float(want_f(vm, argc, argv, 0, "fromFloat expects a float")));
}

static sl_value s_from_bool(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 1 || argv[0].type != SL_BOOL) {
        sl_fail(vm, "fromBool expects a bool");
        return sl_void();
    }
    return sl_cstr(vm, argv[0].as.b ? "true" : "false");
}

// Trimmed decimal integer. `ok` says whether the whole string was one, which
// is the difference between toInt and isInt.
static int64_t parse_int(const char *p, size_t n, bool *ok) {
    size_t a = 0, b = n;
    while (a < b && is_space(p[a])) a++;
    while (b > a && is_space(p[b - 1])) b--;
    *ok = false;
    if (a >= b) return 0;
    bool neg = false;
    if (p[a] == '+' || p[a] == '-') { neg = p[a] == '-'; a++; }
    if (a >= b) return 0;
    int64_t v = 0;
    for (size_t i = a; i < b; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        v = v * 10 + (p[i] - '0');
    }
    *ok = true;
    return neg ? -v : v;
}

static sl_value s_to_int(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "toInt expects a string", &n);
    if (!p) return sl_void();
    bool ok;
    int64_t v = parse_int(p, n, &ok);
    return sl_int(ok ? v : 0);   // unparseable reads as 0, as in Rust
}

static sl_value s_is_int(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "isInt expects a string", &n);
    if (!p) return sl_void();
    bool ok;
    parse_int(p, n, &ok);
    return sl_bool(ok);
}

void sl_open_std(sl_vm *vm) {
    struct { const char *name; sl_native_fn fn; } table[] = {
        { "math.toFloat",  m_to_float },
        { "math.toInt",    m_to_int },
        { "math.floor",     m_floor },
        { "math.ceil",      m_ceil },
        { "math.round",     m_round },
        { "math.sqrt",      m_sqrt },
        { "math.pow",       m_pow },
        { "math.exp",       m_exp },
        { "math.ln",        m_ln },
        { "math.log10",     m_log10 },
        { "math.sin",       m_sin },
        { "math.cos",       m_cos },
        { "math.tan",       m_tan },
        { "math.atan2",     m_atan2 },
        { "math.abs",       m_abs },
        { "math.absInt",   m_abs_int },

        { "strings.len",         s_len },
        { "strings.sub",         s_sub },
        { "strings.at",          s_at },
        { "strings.indexOf",    s_index_of },
        { "strings.contains",    s_contains },
        { "strings.startsWith", s_starts_with },
        { "strings.endsWith",   s_ends_with },
        { "strings.upper",       s_upper },
        { "strings.lower",       s_lower },
        { "strings.trim",        s_trim },
        { "strings.repeat",      s_repeat },
        { "strings.replace",     s_replace },
        { "strings.reverse",     s_reverse },
        { "strings.split",       s_split },
        { "strings.join",        s_join },
        { "strings.chars",       s_chars },
        { "strings.fromInt",    s_from_int },
        { "strings.fromFloat",  s_from_float },
        { "strings.fromBool",   s_from_bool },
        { "strings.toInt",      s_to_int },
        { "strings.isInt",      s_is_int },
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        sl_register(vm, table[i].name, table[i].fn);
}
