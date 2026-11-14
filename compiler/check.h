// Name resolution and type checking, in four passes so declaration order
// never matters. With modules each of the first two runs over every module
// before the next starts, or a module that sorts earlier couldn't refer to a
// type in one that sorts later.
//
//   1. declaration names
//   2. field, parameter and return types
//   3. interface conformance, reference cycles
//   4. function bodies
//
// Declarations are keyed by fully-qualified name ("geom.Vec2"). A written
// name resolves against the current module plus its imports, so a module on
// disk but not imported is genuinely out of scope.
//
// The tables here are what codegen needs (field order, variant order,
// signatures), so they're handed on rather than thrown away.

#ifndef SOLIS_CHECK_H
#define SOLIS_CHECK_H

#include "ast.h"
#include "module.h"
#include "type.h"

typedef struct {
    const char *name;
    ty         *t;
    bool        is_weak;
    span        at;
} field_info;

typedef struct {
    const char *name;
    ty         *t;
} named_ty;

// Named vector types, because every use of the VEC macro declares a distinct
// anonymous struct: fine inside one function, useless in a header.
typedef VEC(field_info) field_info_vec;
typedef VEC(named_ty)   named_ty_vec;

typedef struct {
    const char    *name;
    named_ty_vec   params;
    ty            *ret;
    generic_vec    generics;
    bool           is_static;
    bool           is_mut;
    bool           has_body;
    span           at;
} fn_sig;

typedef VEC(fn_sig *) sig_vec;

typedef struct {
    const char *name;
    span        at;
} implemented;

typedef VEC(implemented) implemented_vec;

typedef struct {
    field_info_vec  fields;
    // Declaration order, which keeps diagnostics deterministic. Lookup is
    // linear; a struct has a handful of methods, not a thousand.
    sig_vec         methods;
    implemented_vec implements;
    span            at;
} struct_info;

typedef struct {
    const char   *name;
    named_ty_vec  payload;
} variant_info;

typedef VEC(variant_info) variant_info_vec;

typedef struct {
    variant_info_vec variants;
    span             at;
} enum_info;

typedef struct {
    sig_vec methods;
    span    at;
} interface_info;

// Everything the checker learned, for code generation to reuse.
typedef struct {
    map structs;        // qualified name -> struct_info*
    map enums;          // qualified name -> enum_info*
    map interfaces;     // qualified name -> interface_info*
    map funcs;          // qualified name -> fn_sig*
    map consts;         // qualified name -> ty*
    map externs;        // qualified name -> non-NULL marker
} decls;

// What a name resolved to. The checker records one of these for every use it
// resolves when it is given a recorder, so tools can answer "what is this" and
// "where is it declared" without redoing name resolution.
typedef enum {
    REF_LOCAL, REF_PARAM, REF_FUNC, REF_CONST, REF_TYPE, REF_FIELD, REF_METHOD,
    REF_VARIANT, REF_MODULE, REF_BUILTIN
} ref_kind;

typedef struct {
    ref_kind    kind;
    const char *module;       // the module the use is in
    span        use;          // the identifier as written
    bool        has_def;      // false for built-ins and anything without a declaration
    const char *def_module;
    span        def;          // the declared name
    const char *name;
    ty         *t;            // its type; the function type for a function
    const char *detail;       // one-line declaration, e.g. "func area(r: float): float"
} ref_info;

typedef VEC(ref_info) ref_vec;

// A local binding and the part of the source where it can be named.
typedef struct {
    const char *name;
    ref_kind    kind;         // REF_LOCAL or REF_PARAM
    ty         *t;
    const char *module;
    span        decl;
    uint32_t    from, to;     // visible at offsets in [from, to)
} scope_info;

typedef VEC(scope_info) scope_vec;

typedef struct {
    ref_vec   refs;
    scope_vec scopes;
} check_record;

// Check every module together. Diagnostics are appended to `diags`, each
// tagged with the module that produced it.
decls *check_modules(arena *a, sl_interner *in, loaded *l, diag_vec *diags);

// Same, and fills `rec` (if not NULL) with what each name resolved to.
decls *check_modules_recorded(arena *a, sl_interner *in, loaded *l, diag_vec *diags, check_record *rec);

// One-line declaration of a function, e.g. "func area(r: float): float".
const char *sig_describe(arena *a, const char *name, const fn_sig *sig);

// Find a method by name in declaration order, or NULL.
fn_sig *sig_lookup(const sig_vec *methods, const char *name);

#endif // SOLIS_CHECK_H
