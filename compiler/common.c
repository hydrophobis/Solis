#include "common.h"

#include <stdio.h>

#define CHUNK_MIN (64 * 1024)

struct sl_chunk {
    sl_chunk *next;
    size_t    cap;
    size_t    len;
    char      data[1];
};

void arena_init(arena *a) {
    a->head = NULL;
    a->used = 0;
}

void arena_free(arena *a) {
    sl_chunk *c = a->head;
    while (c) {
        sl_chunk *next = c->next;
        free(c);
        c = next;
    }
    a->head = NULL;
    a->used = 0;
}

void *arena_alloc(arena *a, size_t n) {
    // Align every allocation, because AST nodes contain pointers and doubles.
    n = (n + 15) & ~(size_t)15;

    if (!a->head || a->head->len + n > a->head->cap) {
        size_t cap = n > CHUNK_MIN ? n : CHUNK_MIN;
        sl_chunk *c = (sl_chunk *)malloc(sizeof(sl_chunk) + cap);
        if (!c) {
            fprintf(stderr, "solis: out of memory\n");
            exit(70);
        }
        c->next = a->head;
        c->cap = cap;
        c->len = 0;
        a->head = c;
    }
    void *p = a->head->data + a->head->len;
    a->head->len += n;
    a->used += n;
    return p;
}

void *arena_zalloc(arena *a, size_t n) {
    void *p = arena_alloc(a, n);
    memset(p, 0, n);
    return p;
}

char *arena_strndup(arena *a, const char *p, size_t n) {
    char *out = (char *)arena_alloc(a, n + 1);
    if (n) memcpy(out, p, n);
    out[n] = 0;
    return out;
}

char *arena_strdup(arena *a, const char *z) {
    return arena_strndup(a, z, strlen(z));
}

char *arena_printf(arena *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return arena_strdup(a, "");

    char *out = (char *)arena_alloc(a, (size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(out, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return out;
}

void *vec_grow_(arena *a, void *at, int *cap, int len, size_t elem) {
    int n = *cap < 8 ? 8 : *cap * 2;
    void *out = arena_alloc(a, elem * (size_t)n);
    if (len) memcpy(out, at, elem * (size_t)len);
    *cap = n;
    return out;
}

typedef struct intern_slot {
    const char *str;
    size_t      len;
    uint32_t    hash;
} intern_slot;

struct sl_interner {
    arena       *a;
    intern_slot *slots;
    int          cap;
    int          len;
};

static uint32_t hash_bytes(const char *p, size_t n) {
    // FNV-1a: short, fast enough, and good enough for identifiers.
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 16777619u;
    }
    return h;
}

sl_interner *intern_new(arena *a) {
    sl_interner *t = NEW(a, sl_interner);
    t->a = a;
    t->cap = 1024;
    t->len = 0;
    t->slots = (intern_slot *)arena_zalloc(a, sizeof(intern_slot) * (size_t)t->cap);
    return t;
}

static void intern_grow(sl_interner *t) {
    int old_cap = t->cap;
    intern_slot *old = t->slots;
    t->cap *= 2;
    t->slots = (intern_slot *)arena_zalloc(t->a, sizeof(intern_slot) * (size_t)t->cap);
    for (int i = 0; i < old_cap; i++) {
        if (!old[i].str) continue;
        uint32_t j = old[i].hash & (uint32_t)(t->cap - 1);
        while (t->slots[j].str) j = (j + 1) & (uint32_t)(t->cap - 1);
        t->slots[j] = old[i];
    }
}

const char *intern(sl_interner *t, const char *p, size_t n) {
    uint32_t h = hash_bytes(p, n);
    uint32_t i = h & (uint32_t)(t->cap - 1);
    while (t->slots[i].str) {
        if (t->slots[i].hash == h && t->slots[i].len == n &&
            memcmp(t->slots[i].str, p, n) == 0) {
            return t->slots[i].str;
        }
        i = (i + 1) & (uint32_t)(t->cap - 1);
    }
    const char *copy = arena_strndup(t->a, p, n);
    t->slots[i].str = copy;
    t->slots[i].len = n;
    t->slots[i].hash = h;
    t->len++;
    if (t->len * 4 > t->cap * 3) intern_grow(t);
    return copy;
}

const char *intern_z(sl_interner *t, const char *z) {
    return intern(t, z, strlen(z));
}

void map_init(map *m, arena *a) {
    m->a = a;
    m->cap = 64;
    m->len = 0;
    m->slots = (map_slot *)arena_zalloc(a, sizeof(map_slot) * (size_t)m->cap);
}

static int map_find(const map *m, const char *key) {
    uint32_t h = hash_bytes(key, strlen(key));
    uint32_t i = h & (uint32_t)(m->cap - 1);
    while (m->slots[i].key) {
        if (strcmp(m->slots[i].key, key) == 0) return (int)i;
        i = (i + 1) & (uint32_t)(m->cap - 1);
    }
    return -(int)i - 1;     // the empty slot it would go in
}

void *map_get(const map *m, const char *key) {
    int i = map_find(m, key);
    return i >= 0 ? m->slots[i].val : NULL;
}

bool map_has(const map *m, const char *key) {
    return map_find(m, key) >= 0;
}

static void map_grow(map *m) {
    int old_cap = m->cap;
    map_slot *old = m->slots;
    m->cap *= 2;
    m->slots = (map_slot *)arena_zalloc(m->a, sizeof(map_slot) * (size_t)m->cap);
    for (int k = 0; k < old_cap; k++) {
        if (!old[k].key) continue;
        uint32_t h = hash_bytes(old[k].key, strlen(old[k].key));
        uint32_t i = h & (uint32_t)(m->cap - 1);
        while (m->slots[i].key) i = (i + 1) & (uint32_t)(m->cap - 1);
        m->slots[i] = old[k];
    }
}

void map_put(map *m, const char *key, void *val) {
    int i = map_find(m, key);
    if (i >= 0) {
        m->slots[i].val = val;
        return;
    }
    i = -i - 1;
    m->slots[i].key = key;
    m->slots[i].val = val;
    m->len++;
    if (m->len * 4 > m->cap * 3) map_grow(m);
}

void diag_add(arena *a, diag_vec *v, span at, const char *msg) {
    diag d;
    d.at = at;
    d.message = msg;
    d.help = NULL;
    d.module = NULL;
    vec_push(a, v, d);
}

void diag_addf(arena *a, diag_vec *v, span at, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    char *msg = (char *)arena_alloc(a, (size_t)(n < 0 ? 0 : n) + 1);
    va_start(ap, fmt);
    vsnprintf(msg, (size_t)(n < 0 ? 0 : n) + 1, fmt, ap);
    va_end(ap);

    diag_add(a, v, at, msg);
}

void diag_help(arena *a, diag_vec *v, span at, const char *help, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    char *msg = (char *)arena_alloc(a, (size_t)(n < 0 ? 0 : n) + 1);
    va_start(ap, fmt);
    vsnprintf(msg, (size_t)(n < 0 ? 0 : n) + 1, fmt, ap);
    va_end(ap);

    diag_add(a, v, at, msg);
    v->at[v->len - 1].help = help;
}

line_col line_col_of(const char *src, size_t len, uint32_t offset) {
    size_t off = offset < len ? offset : len;
    line_col lc;
    lc.line = 1;
    size_t line_start = 0;
    for (size_t i = 0; i < off; i++) {
        if (src[i] == '\n') {
            lc.line++;
            line_start = i + 1;
        }
    }
    // Count characters, not bytes.
    uint32_t col = 1;
    for (size_t i = line_start; i < off; i++)
        if (((unsigned char)src[i] & 0xC0) != 0x80) col++;
    lc.col = col;
    return lc;
}

void diag_render(const char *src, size_t len, const char *path, const diag *d, FILE *out) {
    line_col lc = line_col_of(src, len, d->at.start);

    size_t ls = 0, le = len;
    uint32_t line = 1;
    for (size_t i = 0; i < len; i++) {
        if (line == lc.line) { ls = i; break; }
        if (src[i] == '\n') line++;
    }
    if (lc.line == 1) ls = 0;
    for (size_t i = ls; i < len; i++) {
        if (src[i] == '\n') { le = i; break; }
    }
    if (le > len) le = len;
    size_t line_len = le > ls ? le - ls : 0;
    // Trim a trailing CR so Windows line endings do not shift the caret.
    if (line_len && src[ls + line_len - 1] == '\r') line_len--;

    char gutter[16];
    snprintf(gutter, sizeof gutter, "%u", lc.line);
    size_t gw = strlen(gutter);

    fprintf(out, "error: %s\n", d->message);
    fprintf(out, "%*s--> %s:%u:%u\n", (int)gw, "", path, lc.line, lc.col);
    fprintf(out, "%*s |\n", (int)gw, "");
    fprintf(out, "%s | %.*s\n", gutter, (int)line_len, src + ls);

    // The caret spans the token, clamped to what is left of the line, so a
    // span that runs past the end does not print a wall of carets.
    uint32_t width = d->at.end > d->at.start ? d->at.end - d->at.start : 1;
    size_t room = line_len > (size_t)(lc.col - 1) ? line_len - (lc.col - 1) : 1;
    if (room < 1) room = 1;
    if (width > room) width = (uint32_t)room;

    fprintf(out, "%*s | %*s", (int)gw, "", (int)(lc.col - 1), "");
    for (uint32_t i = 0; i < width; i++) fputc('^', out);
    fputc('\n', out);

    if (d->help) fprintf(out, "%*s = help: %s\n", (int)gw, "", d->help);
}
