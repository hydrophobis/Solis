#include "type.h"

#include <stdio.h>

// The primitives are static singletons: there is exactly one `int` type in a
// compile, so nothing has to allocate one.
#define PRIM(fn, k)                                                            \
    ty *fn(void) {                                                             \
        static ty t;                                                           \
        t.kind = (k);                                                          \
        return &t;                                                             \
    }

PRIM(ty_int, TK_INT)
PRIM(ty_float, TK_FLOAT)
PRIM(ty_bool, TK_BOOL)
PRIM(ty_str, TK_STR)
PRIM(ty_void, TK_VOID)
PRIM(ty_null_lit, TK_NULL_LIT)
PRIM(ty_error, TK_ERROR)

ty *ty_named(arena *a, const char *name) {
    ty *t = NEW(a, ty);
    t->kind = TK_NAMED;
    t->name = name;
    return t;
}

ty *ty_param(arena *a, const char *name) {
    ty *t = NEW(a, ty);
    t->kind = TK_PARAM;
    t->name = name;
    return t;
}

ty *ty_array(arena *a, ty *elem) {
    ty *t = NEW(a, ty);
    t->kind = TK_ARRAY;
    t->elem = elem;
    return t;
}

ty *ty_map(arena *a, ty *k, ty *v) {
    ty *t = NEW(a, ty);
    t->kind = TK_MAP;
    t->key = k;
    t->val = v;
    return t;
}

ty *ty_func(arena *a, ty_vec params, ty *ret) {
    ty *t = NEW(a, ty);
    t->kind = TK_FUNC;
    t->params = params;
    t->ret = ret ? ret : ty_void();
    return t;
}

bool ty_eq(const ty *x, const ty *y) {
    if (x == y) return true;
    if (!x || !y) return false;
    if (x->kind != y->kind) return false;
    switch (x->kind) {
        case TK_NAMED:
        case TK_PARAM:
            return strcmp(x->name, y->name) == 0;
        case TK_ARRAY:
            return ty_eq(x->elem, y->elem);
        case TK_MAP:
            return ty_eq(x->key, y->key) && ty_eq(x->val, y->val);
        case TK_FUNC: {
            if (x->params.len != y->params.len) return false;
            for (int i = 0; i < x->params.len; i++)
                if (!ty_eq(x->params.at[i], y->params.at[i])) return false;
            return ty_eq(x->ret, y->ret);
        }
        default:
            return true;    // the primitives are equal when their kinds are
    }
}

bool ty_is_reference(const ty *t) {
    switch (t->kind) {
        case TK_STR: case TK_NAMED: case TK_ARRAY: case TK_MAP: case TK_FUNC:
            return true;
        default:
            return false;
    }
}

bool ty_is_numeric(const ty *t) {
    return t->kind == TK_INT || t->kind == TK_FLOAT;
}

bool ty_assignable(const ty *from, const ty *want) {
    // Poison never cascades.
    if (from->kind == TK_ERROR || want->kind == TK_ERROR) return true;
    if (ty_eq(from, want)) return true;
    // `null` fits any reference type, and a reference fits a `null` hole.
    if (from->kind == TK_NULL_LIT && ty_is_reference(want)) return true;
    if (want->kind == TK_NULL_LIT && ty_is_reference(from)) return true;
    return false;
}

const char *ty_show(arena *a, const ty *t) {
    switch (t->kind) {
        case TK_INT: return "int";
        case TK_FLOAT: return "float";
        case TK_BOOL: return "bool";
        case TK_STR: return "str";
        case TK_VOID: return "()";
        case TK_NULL_LIT: return "null";
        case TK_ERROR: return "<error>";
        case TK_NAMED:
        case TK_PARAM: return t->name;
        case TK_ARRAY: return arena_printf(a, "[%s]", ty_show(a, t->elem));
        case TK_MAP:
            return arena_printf(a, "{%s: %s}", ty_show(a, t->key), ty_show(a, t->val));
        case TK_FUNC: {
            // Build the parameter list, then wrap it.
            const char *ps = "";
            for (int i = 0; i < t->params.len; i++) {
                ps = i == 0 ? ty_show(a, t->params.at[i])
                            : arena_printf(a, "%s, %s", ps, ty_show(a, t->params.at[i]));
            }
            if (t->ret->kind == TK_VOID) return arena_printf(a, "func(%s)", ps);
            return arena_printf(a, "func(%s): %s", ps, ty_show(a, t->ret));
        }
    }
    return "<?>";
}
