// The syntax tree. Every node carries a span; the checker and the
// diagnostics both need to point back at source.

#ifndef SOLIS_AST_H
#define SOLIS_AST_H

#include "common.h"

typedef struct type type;
typedef struct expr expr;
typedef struct stmt stmt;
typedef struct func func;

// A dotted name. The parser only ever produces single-segment paths in
// expression position (`a.b` is a Field node), but type positions and
// patterns do use several segments.
typedef struct {
    const char **seg;       // interned
    int          n;
    span         at;
} path;

// The dotted spelling, for diagnostics and qualified-name lookup.
const char *path_text(arena *a, const path *p);

typedef enum {
    TY_NAMED,       // int, Vec2, Map[str, int]
    TY_ARRAY,       // [T]
    TY_MAP,         // {K: V}
    TY_TUPLE,       // () or (A, B)
    TY_FN           // func(A, B): C
} type_kind;

typedef VEC(type *) type_vec;

struct type {
    type_kind kind;
    bool      is_weak;
    span      at;
    union {
        struct { path p; type_vec args; } named;
        type *array;
        struct { type *k, *v; } map;
        type_vec tuple;
        struct { type_vec params; type *ret; } fn;   // ret may be NULL
    } as;
};

typedef struct {
    const char *name;
    type       *ty;
    span        at;
} param;

typedef VEC(param) param_vec;

typedef struct {
    const char *name;
    type       *ty;
    bool        is_weak;
    span        at;
} field;

typedef VEC(field) field_vec;

typedef VEC(path) path_vec;

typedef struct {
    const char *name;
    path_vec    bounds;
    span        at;
} generic_param;

typedef VEC(generic_param) generic_vec;

typedef VEC(stmt *) stmt_vec;

typedef struct {
    stmt_vec stmts;
    span     at;
} block;

struct func {
    const char *name;
    span        name_at;
    generic_vec generics;
    param_vec   params;
    type       *ret;        // NULL when there is no declared return
    block      *body;       // NULL for an extern or a bare interface method
    bool        is_static;
    bool        is_mut;
    span        at;
};

typedef VEC(func *) func_vec;

// An entry in a `: iface_list`, e.g. `Comparable` or `Container[T]`.
typedef struct {
    path     p;
    type_vec args;
    span     at;
} iface_ref;

typedef VEC(iface_ref) iface_ref_vec;

typedef struct {
    const char   *name;
    generic_vec   generics;
    iface_ref_vec implements;
    field_vec     fields;
    func_vec      methods;
    span          at;
} struct_decl;

typedef struct {
    const char *name;
    // Payload fields are named, so patterns can bind them by name.
    param_vec   payload;
    span        at;
} variant;

typedef VEC(variant) variant_vec;

typedef struct {
    const char *name;
    generic_vec generics;
    variant_vec variants;
    span        at;
} enum_decl;

typedef struct {
    const char *name;
    generic_vec generics;
    func_vec    methods;
    span        at;
} interface_decl;

typedef enum {
    IT_FUNC, IT_STRUCT, IT_ENUM, IT_INTERFACE, IT_EXTERN, IT_LET, IT_IMPORT
} item_kind;

typedef struct {
    item_kind kind;
    union {
        func           *fn;         // IT_FUNC, IT_EXTERN
        struct_decl    *st;
        enum_decl      *en;
        interface_decl *iface;
        stmt           *let;        // IT_LET: a module-level binding
        struct { const char *name; span at; } import;
    } as;
} item;

typedef VEC(item) item_vec;

typedef struct {
    item_vec items;
} program;

typedef struct {
    const char *name;
    span        at;
} binding;

typedef VEC(binding) binding_vec;

typedef enum {
    PAT_WILDCARD, PAT_INT, PAT_BOOL, PAT_NULL, PAT_STR, PAT_VARIANT
} pattern_kind;

typedef struct {
    pattern_kind kind;
    span         at;
    union {
        int64_t     i;
        bool        b;
        const char *s;
        // `Event.Key(code)`: the bindings name the payload fields.
        struct { path p; binding_vec bindings; } variant;
    } as;
} pattern;

typedef VEC(pattern) pattern_vec;

typedef struct {
    pattern_vec patterns;
    stmt_vec    body;
    span        at;
} switch_case;

typedef VEC(switch_case) case_vec;

typedef enum {
    ASSIGN_SET, ASSIGN_ADD, ASSIGN_SUB, ASSIGN_MUL, ASSIGN_DIV, ASSIGN_REM
} assign_op;

typedef enum {
    ST_LET, ST_ASSIGN, ST_IF, ST_WHILE, ST_FOR, ST_SWITCH,
    ST_ARENA, ST_RETURN, ST_BREAK, ST_CONTINUE, ST_BLOCK, ST_EXPR
} stmt_kind;

struct stmt {
    stmt_kind kind;
    span      at;
    union {
        struct { const char *name; type *ty; expr *value; bool is_var; } let_;
        struct { expr *place; assign_op op; expr *value; } assign;
        struct { expr *cond; block *then; stmt *els; } if_;
        struct { expr *cond; block *body; } while_;
        struct { const char *var; expr *iter; block *body; } for_;
        struct { expr *scrutinee; case_vec cases; block *dflt; } switch_;
        block *blk;             // ST_ARENA, ST_BLOCK
        expr  *value;           // ST_RETURN (may be NULL), ST_EXPR
    } as;
};

typedef enum {
    OP_OR, OP_AND, OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_REM
} bin_op;

const char *bin_op_symbol(bin_op op);

typedef enum { UN_NOT, UN_NEG } un_op;

// One piece of an interpolated string.
typedef struct {
    bool        is_interp;
    const char *text;       // when !is_interp
    expr       *value;      // when is_interp
} str_seg;

typedef VEC(str_seg) str_seg_vec;

typedef VEC(expr *) expr_vec;

typedef struct {
    const char *name;
    expr       *value;
} field_init;

typedef VEC(field_init) field_init_vec;

typedef struct {
    expr *k, *v;
} map_entry;

typedef VEC(map_entry) map_entry_vec;

typedef enum {
    EX_INT, EX_FLOAT, EX_BOOL, EX_NULL, EX_STR, EX_PATH, EX_THIS,
    EX_UNARY, EX_BINARY, EX_CALL, EX_INDEX, EX_SLICE, EX_FIELD, EX_STRUCT_LIT,
    EX_ARRAY, EX_MAP
} expr_kind;

struct expr {
    expr_kind kind;
    span      at;
    // The type the checker settled on, for the one expression whose type
    // cannot be read off the expression itself: an empty array literal takes
    // its element type from whatever it is being assigned to. Codegen reads
    // it; everything else leaves it NULL. The struct tag avoids having to
    // repeat type.h's typedef, which C99 does not allow twice.
    struct ty *hint;   // `ty *`, declared in type.h
    union {
        int64_t     i;
        double      f;
        bool        b;
        str_seg_vec str;
        path        p;
        struct { un_op op; expr *rhs; } unary;
        struct { bin_op op; expr *lhs, *rhs; } binary;
        struct { expr *callee; expr_vec args; } call;
        struct { expr *base, *index; } index;
        // `a[lo:hi]`; either bound may be NULL (`a[:]`, `a[i:]`, `a[:j]`).
        struct { expr *base, *lo, *hi; } slice;
        // `a.b`, or `a?.b` when optional is set.
        struct { expr *base; const char *name; bool optional; } field;
        struct { path p; field_init_vec fields; } struct_lit;
        expr_vec      array;
        map_entry_vec map;
    } as;
};

#endif // SOLIS_AST_H
