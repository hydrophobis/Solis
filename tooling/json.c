#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *pool_alloc(jpool *p, size_t n) {
    void *m = calloc(1, n ? n : 1);
    if (!m) return NULL;
    if (p->len == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 64;
        p->at = (void **)realloc(p->at, (size_t)p->cap * sizeof(void *));
        if (!p->at) { free(m); return NULL; }
    }
    p->at[p->len++] = m;
    return m;
}

void jpool_free(jpool *p) {
    for (int i = 0; i < p->len; i++) free(p->at[i]);
    free(p->at);
    p->at = NULL;
    p->len = p->cap = 0;
}

typedef struct { jpool *pool; const char *s; size_t n, i; int depth; } reader;

static void skip_ws(reader *r) {
    while (r->i < r->n && (r->s[r->i] == ' ' || r->s[r->i] == '\t' || r->s[r->i] == '\n' || r->s[r->i] == '\r')) r->i++;
}

static jv *value(reader *r);

static int hex4(const char *p) {
    int v = 0;
    for (int k = 0; k < 4; k++) {
        char c = p[k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static size_t put_utf8(char *out, unsigned cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// A string literal starting at the opening quote. Returns a pool copy, or NULL.
static char *string_lit(reader *r, size_t *len_out) {
    if (r->i >= r->n || r->s[r->i] != '"') return NULL;
    r->i++;
    char *buf = (char *)pool_alloc(r->pool, r->n - r->i + 1);
    if (!buf) return NULL;
    size_t w = 0;
    while (r->i < r->n && r->s[r->i] != '"') {
        char c = r->s[r->i++];
        if (c != '\\') { buf[w++] = c; continue; }
        if (r->i >= r->n) return NULL;
        char e = r->s[r->i++];
        switch (e) {
            case 'n': buf[w++] = '\n'; break;
            case 't': buf[w++] = '\t'; break;
            case 'r': buf[w++] = '\r'; break;
            case 'b': buf[w++] = '\b'; break;
            case 'f': buf[w++] = '\f'; break;
            case '/': case '\\': case '"': buf[w++] = e; break;
            case 'u': {
                if (r->i + 4 > r->n) return NULL;
                int hi = hex4(r->s + r->i);
                if (hi < 0) return NULL;
                r->i += 4;
                unsigned cp = (unsigned)hi;
                if (hi >= 0xD800 && hi < 0xDC00 && r->i + 6 <= r->n && r->s[r->i] == '\\' && r->s[r->i + 1] == 'u') {
                    int lo = hex4(r->s + r->i + 2);
                    if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + (((unsigned)hi - 0xD800) << 10) + ((unsigned)lo - 0xDC00); r->i += 6; }
                }
                w += put_utf8(buf + w, cp);
                break;
            }
            default: return NULL;
        }
    }
    if (r->i >= r->n) return NULL;
    r->i++;
    buf[w] = 0;
    if (len_out) *len_out = w;
    return buf;
}

static jv *make(reader *r, jkind k) {
    jv *v = (jv *)pool_alloc(r->pool, sizeof(jv));
    if (v) v->kind = k;
    return v;
}

static jv *value(reader *r) {
    if (r->depth > 64) return NULL;
    skip_ws(r);
    if (r->i >= r->n) return NULL;
    char c = r->s[r->i];
    if (c == '"') {
        jv *v = make(r, J_STR);
        if (!v) return NULL;
        v->str = string_lit(r, &v->len);
        return v->str ? v : NULL;
    }
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        char close = obj ? '}' : ']';
        r->i++;
        r->depth++;
        jv *v = make(r, obj ? J_OBJ : J_ARR);
        if (!v) return NULL;
        int cap = 0;
        skip_ws(r);
        if (r->i < r->n && r->s[r->i] == close) { r->i++; r->depth--; return v; }
        for (;;) {
            skip_ws(r);
            char *key = NULL;
            if (obj) {
                key = string_lit(r, NULL);
                if (!key) return NULL;
                skip_ws(r);
                if (r->i >= r->n || r->s[r->i] != ':') return NULL;
                r->i++;
            }
            jv *item = value(r);
            if (!item) return NULL;
            if (v->n == cap) {
                int ncap = cap ? cap * 2 : 8;
                jv **ni = (jv **)pool_alloc(r->pool, (size_t)ncap * sizeof(jv *));
                char **nk = obj ? (char **)pool_alloc(r->pool, (size_t)ncap * sizeof(char *)) : NULL;
                if (!ni || (obj && !nk)) return NULL;
                if (v->n) memcpy(ni, v->items, (size_t)v->n * sizeof(jv *));
                if (obj && v->n) memcpy(nk, v->keys, (size_t)v->n * sizeof(char *));
                v->items = ni; v->keys = nk; cap = ncap;
            }
            v->items[v->n] = item;
            if (obj) v->keys[v->n] = key;
            v->n++;
            skip_ws(r);
            if (r->i < r->n && r->s[r->i] == ',') { r->i++; continue; }
            if (r->i < r->n && r->s[r->i] == close) { r->i++; break; }
            return NULL;
        }
        r->depth--;
        return v;
    }
    if (strncmp(r->s + r->i, "true", r->n - r->i >= 4 ? 4 : r->n - r->i) == 0 && r->n - r->i >= 4) {
        r->i += 4; jv *v = make(r, J_BOOL); if (v) v->b = true; return v;
    }
    if (r->n - r->i >= 5 && strncmp(r->s + r->i, "false", 5) == 0) { r->i += 5; return make(r, J_BOOL); }
    if (r->n - r->i >= 4 && strncmp(r->s + r->i, "null", 4) == 0) { r->i += 4; return make(r, J_NULL); }
    char *end;
    char tmp[64];
    size_t k = 0;
    while (r->i + k < r->n && k < sizeof tmp - 1 && (strchr("+-.eE", r->s[r->i + k]) || (r->s[r->i + k] >= '0' && r->s[r->i + k] <= '9'))) { tmp[k] = r->s[r->i + k]; k++; }
    tmp[k] = 0;
    if (k == 0) return NULL;
    double d = strtod(tmp, &end);
    if (end == tmp) return NULL;
    r->i += k;
    jv *v = make(r, J_NUM);
    if (v) v->num = d;
    return v;
}

jv *json_parse(jpool *pool, const char *text, size_t len) {
    reader r = { pool, text, len, 0, 0 };
    jv *v = value(&r);
    if (!v) return NULL;
    skip_ws(&r);
    return r.i == r.n ? v : NULL;
}

jv *jget(const jv *obj, const char *key) {
    if (!obj || obj->kind != J_OBJ) return NULL;
    for (int i = 0; i < obj->n; i++) if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
    return NULL;
}
jv *jidx(const jv *arr, int i) { return arr && arr->kind == J_ARR && i >= 0 && i < arr->n ? arr->items[i] : NULL; }
const char *jstr(const jv *v, const char *fallback) { return v && v->kind == J_STR ? v->str : fallback; }
double jnum(const jv *v, double fallback) { return v && v->kind == J_NUM ? v->num : fallback; }
bool jbool(const jv *v, bool fallback) { return v && v->kind == J_BOOL ? v->b : fallback; }

static void sb_grow(sbuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->len + extra + 1) cap *= 2;
    b->at = (char *)realloc(b->at, cap);
    if (!b->at) { fprintf(stderr, "sl: out of memory\n"); exit(1); }
    b->cap = cap;
}
void sb_putn(sbuf *b, const char *s, size_t n) { sb_grow(b, n); memcpy(b->at + b->len, s, n); b->len += n; b->at[b->len] = 0; }
void sb_puts(sbuf *b, const char *s) { sb_putn(b, s, strlen(s)); }
void sb_printf(sbuf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) { sb_grow(b, (size_t)n); vsnprintf(b->at + b->len, (size_t)n + 1, fmt, ap2); b->len += (size_t)n; }
    va_end(ap2);
}
void sb_json_string(sbuf *b, const char *s, size_t n) {
    sb_putn(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; sb_putn(b, e, 2); }
        else if (c == '\n') sb_puts(b, "\\n");
        else if (c == '\r') sb_puts(b, "\\r");
        else if (c == '\t') sb_puts(b, "\\t");
        else if (c < 0x20) sb_printf(b, "\\u%04x", c);
        else sb_putn(b, (const char *)&c, 1);
    }
    sb_putn(b, "\"", 1);
}
void sb_free(sbuf *b) { free(b->at); b->at = NULL; b->len = b->cap = 0; }
