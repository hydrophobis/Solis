// Types. Immutable, arena-allocated, compared structurally; the primitives
// are singletons. TK_ERROR is poison: compatible with everything, so one
// mistake gives one diagnostic instead of a cascade.

#ifndef SOLIS_TYPE_H
#define SOLIS_TYPE_H

#include "common.h"

typedef enum {
    TK_INT, TK_FLOAT, TK_BOOL, TK_STR,
    // The type of a function with no declared return.
    TK_VOID,
    // `null` before we know what it is null of. Assignable to any
    // reference type.
    TK_NULL_LIT,
    // A declared struct or enum, by fully-qualified name.
    TK_NAMED,
    TK_ARRAY,
    TK_MAP,
    TK_FUNC,
    // A generic parameter, opaque inside the body that declares it.
    TK_PARAM,
    TK_ERROR
} ty_kind;

typedef struct ty ty;

typedef VEC(ty *) ty_vec;

struct ty {
    ty_kind     kind;
    const char *name;       // TK_NAMED, TK_PARAM
    ty         *elem;       // TK_ARRAY
    ty         *key;        // TK_MAP
    ty         *val;        // TK_MAP
    ty_vec      params;     // TK_FUNC
    ty         *ret;        // TK_FUNC
};

// The primitives, which never need allocating.
ty *ty_int(void);
ty *ty_float(void);
ty *ty_bool(void);
ty *ty_str(void);
ty *ty_void(void);
ty *ty_null_lit(void);
ty *ty_error(void);

ty *ty_named(arena *a, const char *name);
ty *ty_param(arena *a, const char *name);
ty *ty_array(arena *a, ty *elem);
ty *ty_map(arena *a, ty *k, ty *v);
ty *ty_func(arena *a, ty_vec params, ty *ret);

bool ty_eq(const ty *x, const ty *y);

// Reference types are heap values that participate in ARC and may be null.
// Value types are inline and never null.
bool ty_is_reference(const ty *t);
bool ty_is_numeric(const ty *t);

// Can a value of `from` be used where `want` is expected?
bool ty_assignable(const ty *from, const ty *want);

// How a type is spelled in a diagnostic.
const char *ty_show(arena *a, const ty *t);

#endif // SOLIS_TYPE_H
