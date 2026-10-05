// Arena allocation, vectors, string interning, diagnostics.
//
// The arena is the important one: nothing in the compiler is freed
// individually, so AST nodes and types are plain structs joined by plain
// pointers and nobody has to track ownership.

#ifndef SOLIS_COMMON_H
#define SOLIS_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct sl_chunk sl_chunk;

typedef struct {
    sl_chunk *head;
    size_t    used;     // bytes handed out, for reporting
} arena;

void  arena_init(arena *a);
void  arena_free(arena *a);
void *arena_alloc(arena *a, size_t n);

// Zeroed, which is what AST nodes want.
#define NEW(a, T) ((T *)arena_zalloc((a), sizeof(T)))
void *arena_zalloc(arena *a, size_t n);

// Copy a string into the arena, NUL-terminated.
char *arena_strndup(arena *a, const char *p, size_t n);
char *arena_strdup(arena *a, const char *z);

// printf into the arena. Used constantly for qualified names and messages.
char *arena_printf(arena *a, const char *fmt, ...);

// { T *at; int len; int cap; }, declared with VEC(T). Arena-backed, so a
// vector is abandoned rather than freed.

#define VEC(T) struct { T *at; int len; int cap; }

void *vec_grow_(arena *a, void *at, int *cap, int len, size_t elem);

#define vec_push(A, V, X)                                                      \
    do {                                                                       \
        if ((V)->len == (V)->cap)                                              \
            (V)->at = vec_grow_((A), (V)->at, &(V)->cap, (V)->len,             \
                                sizeof(*(V)->at));                             \
        (V)->at[(V)->len++] = (X);                                             \
    } while (0)

// Reserve one slot and return a pointer to it, for nodes big enough that
// building in place beats copying.
#define vec_add(A, V)                                                          \
    (((V)->len == (V)->cap                                                     \
          ? (V)->at = vec_grow_((A), (V)->at, &(V)->cap, (V)->len,             \
                                sizeof(*(V)->at))                              \
          : 0),                                                                \
     &(V)->at[(V)->len++])

// Identifiers become single pointers, so comparison is ==. Qualified names
// ("geom.Vec2.length") go in here too.
typedef struct sl_interner sl_interner;

sl_interner *intern_new(arena *a);
const char  *intern(sl_interner *t, const char *p, size_t n);
const char  *intern_z(sl_interner *t, const char *z);

// String key to void*, open addressing. Keys compare by content, so an
// interned key and a freshly built one both work.

typedef struct {
    const char *key;
    void       *val;
} map_slot;

typedef struct {
    map_slot *slots;
    int       cap;      // always a power of two
    int       len;
    arena    *a;
} map;

void  map_init(map *m, arena *a);
void *map_get(const map *m, const char *key);
bool  map_has(const map *m, const char *key);
void  map_put(map *m, const char *key, void *val);

typedef struct {
    uint32_t start;
    uint32_t end;
} span;

static inline span span_make(size_t start, size_t end) {
    span s;
    s.start = (uint32_t)start;
    s.end = (uint32_t)end;
    return s;
}

typedef struct {
    uint32_t line;
    uint32_t col;
} line_col;

typedef struct {
    span        at;
    const char *message;
    const char *help;       // NULL when there is nothing useful to add
    // Which module this came from, so a multi-module build can render it
    // against the right source. NULL in single-file use.
    const char *module;
} diag;

typedef VEC(diag) diag_vec;

void diag_add(arena *a, diag_vec *v, span at, const char *msg);
void diag_addf(arena *a, diag_vec *v, span at, const char *fmt, ...);
// Same, with a help line.
void diag_help(arena *a, diag_vec *v, span at, const char *help, const char *fmt, ...);

// Byte offset to 1-based line/column. Columns count characters, not bytes,
// so carets land correctly in non-ASCII source.
line_col line_col_of(const char *src, size_t len, uint32_t offset);

// Render one diagnostic with the offending line and a caret.
void diag_render(const char *src, size_t len, const char *path, const diag *d, FILE *out);

#endif // SOLIS_COMMON_H
