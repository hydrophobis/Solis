// A small JSON reader and writer, enough for the language server protocol.

#ifndef SL_JSON_H
#define SL_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jkind;

typedef struct jv jv;
struct jv {
    jkind   kind;
    bool    b;
    double  num;
    char   *str;        // J_STR, NUL-terminated
    size_t  len;        // J_STR byte length
    int     n;          // J_ARR and J_OBJ: number of items
    jv    **items;
    char  **keys;       // J_OBJ only
};

// Everything parsed from one message lives in a pool and is freed together.
typedef struct { void **at; int len, cap; } jpool;

// Parse `text`. Returns NULL on a syntax error.
jv  *json_parse(jpool *pool, const char *text, size_t len);
void jpool_free(jpool *pool);

// Lookups return NULL (or the fallback) when the shape is not what was asked for.
jv         *jget(const jv *obj, const char *key);
jv         *jidx(const jv *arr, int i);
const char *jstr(const jv *v, const char *fallback);
double      jnum(const jv *v, double fallback);
bool        jbool(const jv *v, bool fallback);

// A growing string, for building output.
typedef struct { char *at; size_t len, cap; } sbuf;
void sb_puts(sbuf *b, const char *s);
void sb_putn(sbuf *b, const char *s, size_t n);
void sb_printf(sbuf *b, const char *fmt, ...);
// A JSON string literal, quotes included, with escapes.
void sb_json_string(sbuf *b, const char *s, size_t n);
void sb_free(sbuf *b);

#endif
