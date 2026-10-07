#include "check.h"

#include "parse.h"

#include <stdio.h>

typedef struct {
    const char *name;
    ty         *t;
    bool        assignable;
} binding_entry;

typedef struct {
    const char       *name;
    VEC(const char *) bounds;
} type_param;

typedef struct {
    arena       *a;
    sl_interner *in;
    decls       *d;
    diag_vec    *diags;

    VEC(binding_entry) scope;
    VEC(int)           scope_marks;

    // Generic parameters in scope, with their interface bounds.
    VEC(type_param)    type_params;

    // `this` inside a method body, and whether the method may mutate it.
    ty   *this_ty;
    bool  this_mut;
    ty   *ret_ty;

    const char       *current_module;
    const char       *prelude;      // the ambient module, or NULL
    VEC(const char *) imports;

    // Generic structs/enums: collected here instead of `d->structs`/`enums`,
    // and monomorphised on demand (see instantiate_struct/instantiate_enum).
    map struct_tmpls;    // qualified name -> struct_decl*
    map enum_tmpls;      // qualified name -> enum_decl*
    map iface_tmpls;     // qualified name -> interface_decl*
    map instantiated;    // mangled qualified name -> done-or-in-progress
    // Lets check_conformance/check_cycles re-run after check_bodies without
    // double-reporting what their first pass already covered.
    map conformance_checked;
    map cycles_checked;
    VEC(named_ty) active_subst;   // generic param name -> its concrete ty here
    program *current_prog;        // the module being collect_details'd
} checker;

fn_sig *sig_lookup(const sig_vec *methods, const char *name) {
    for (int i = 0; i < methods->len; i++)
        if (strcmp(methods->at[i]->name, name) == 0) return methods->at[i];
    return NULL;
}

static void cerr(checker *C, span at, const char *msg) {
    diag_add(C->a, C->diags, at, msg);
    C->diags->at[C->diags->len - 1].module = C->current_module;
}

static void cerr_help(checker *C, span at, const char *help, const char *msg) {
    cerr(C, at, msg);
    C->diags->at[C->diags->len - 1].help = help;
}

// Written name -> the key it is stored under. `Vec2` is this module's,
// `math.Vec2` an imported module's. NULL if not visible from here.
static const char *qualify(checker *C, const char *written) {
    const char *dot = strchr(written, '.');
    if (dot) {
        size_t hn = (size_t)(dot - written);
        if (strlen(C->current_module) == hn &&
            memcmp(C->current_module, written, hn) == 0) {
            return written;
        }
        for (int i = 0; i < C->imports.len; i++)
            if (strlen(C->imports.at[i]) == hn && memcmp(C->imports.at[i], written, hn) == 0)
                return written;
        return NULL;
    }
    return arena_printf(C->a, "%s.%s", C->current_module, written);
}

// `name` as the prelude would spell it, or NULL when there is no prelude or
// the name is already qualified.
static const char *prelude_name(checker *C, const char *written) {
    if (!C->prelude || strchr(written, '.')) return NULL;
    if (strcmp(C->prelude, C->current_module) == 0) return NULL;
    return arena_printf(C->a, "%s.%s", C->prelude, written);
}

// `[]`/`{}` have no element to infer from, so they take the type they're
// checked against. Stashes the type on the node for codegen, which has the
// same problem. Returns the type the expression should now be considered.
static ty *adopt_empty_literal(expr *v, ty *got, ty *want) {
    if (!v || !want) return got;
    if (v->kind == EX_ARRAY && v->as.array.len == 0 && want->kind == TK_ARRAY) {
        v->hint = want;
        return want;
    }
    if (v->kind == EX_MAP && v->as.map.len == 0 && want->kind == TK_MAP) {
        v->hint = want;
        return want;
    }
    return got;
}

static bool is_visible_module(checker *C, const char *name) {
    for (int i = 0; i < C->imports.len; i++)
        if (strcmp(C->imports.at[i], name) == 0) return true;
    return false;
}

static void push_scope(checker *C) {
    vec_push(C->a, &C->scope_marks, C->scope.len);
}

static void pop_scope(checker *C) {
    if (C->scope_marks.len == 0) return;
    C->scope.len = C->scope_marks.at[--C->scope_marks.len];
}

static void declare(checker *C, const char *name, ty *t, bool assignable) {
    binding_entry b;
    b.name = name;
    b.t = t;
    b.assignable = assignable;
    vec_push(C->a, &C->scope, b);
}

static binding_entry *lookup(checker *C, const char *name) {
    for (int i = C->scope.len - 1; i >= 0; i--)
        if (strcmp(C->scope.at[i].name, name) == 0) return &C->scope.at[i];
    return NULL;
}

static type_param *find_type_param(checker *C, const char *name) {
    for (int i = C->type_params.len - 1; i >= 0; i--)
        if (strcmp(C->type_params.at[i].name, name) == 0) return &C->type_params.at[i];
    return NULL;
}

static ty *check_expr(checker *C, expr *e);
static void check_stmt(checker *C, stmt *s);
static void check_block(checker *C, block *b);
static ty *resolve_ty(checker *C, type *t);
static ty *field_or_method_ty(checker *C, ty *base, const char *name, span at);
static ty *check_path(checker *C, path *p, span at);

static ty *find_subst(checker *C, const char *name);
static const char *instantiate_struct(checker *C, const char *qbase, struct_decl *tmpl,
                                       ty_vec args, span at);
static const char *instantiate_enum(checker *C, const char *qbase, enum_decl *tmpl,
                                     ty_vec args, span at);
static const char *instantiate_interface(checker *C, const char *qbase, interface_decl *tmpl,
                                          ty_vec args, span at);
static void resolve_implements(checker *C, struct_info *si, iface_ref_vec *refs);

static ty *sig_as_func_ty(checker *C, const fn_sig *sig) {
    ty_vec ps;
    memset(&ps, 0, sizeof ps);
    for (int i = 0; i < sig->params.len; i++)
        vec_push(C->a, &ps, sig->params.at[i].t);
    return ty_func(C->a, ps, sig->ret);
}

static ty *resolve_ty(checker *C, type *t) {
    switch (t->kind) {
        case TY_NAMED: {
            const char *n = path_text(C->a, &t->as.named.p);

            if (t->as.named.args.len) {
                ty_vec resolved;
                memset(&resolved, 0, sizeof resolved);
                for (int i = 0; i < t->as.named.args.len; i++)
                    vec_push(C->a, &resolved, resolve_ty(C, t->as.named.args.at[i]));
                if (strcmp(n, "Map") == 0 && resolved.len == 2)
                    return ty_map(C->a, resolved.at[0], resolved.at[1]);

                const char *q = qualify(C, n);
                struct_decl *stmpl = q ? (struct_decl *)map_get(&C->struct_tmpls, q) : NULL;
                if (stmpl)
                    return ty_named(C->a, instantiate_struct(C, q, stmpl, resolved, t->at));
                enum_decl *etmpl = q ? (enum_decl *)map_get(&C->enum_tmpls, q) : NULL;
                if (etmpl)
                    return ty_named(C->a, instantiate_enum(C, q, etmpl, resolved, t->at));

                cerr_help(C, t->at,
                          "`Map` is the only built-in generic type",
                          arena_printf(C->a, "`%s` is not a generic type", n));
                return ty_error();
            }

            if (strcmp(n, "int") == 0) return ty_int();
            if (strcmp(n, "float") == 0) return ty_float();
            if (strcmp(n, "bool") == 0) return ty_bool();
            if (strcmp(n, "str") == 0) return ty_str();

            {
                ty *sub = find_subst(C, n);
                if (sub) return sub;
            }
            if (find_type_param(C, n)) return ty_param(C->a, n);

            const char *q = qualify(C, n);
            if (q && (map_has(&C->d->structs, q) || map_has(&C->d->enums, q)))
                return ty_named(C->a, intern_z(C->in, q));

            if (strchr(n, '.')) {
                const char *dot = strchr(n, '.');
                const char *head = arena_strndup(C->a, n, (size_t)(dot - n));
                cerr_help(C, t->at,
                          arena_printf(C->a,
                                       "`%s` is not imported here; add `import %s;`",
                                       head, head),
                          arena_printf(C->a, "cannot find type `%s`", n));
                return ty_error();
            }
            if (q && map_has(&C->d->interfaces, q)) {
                // Using an interface as a value type needs boxing, which the
                // runtime does not do.
                cerr_help(C, t->at,
                          arena_printf(C->a,
                                       "take a generic parameter bounded by it instead: "
                                       "`func f[T: %s](x: T)`", n),
                          arena_printf(C->a, "`%s` is an interface, not a concrete type", n));
                return ty_error();
            }
            cerr(C, t->at, arena_printf(C->a, "cannot find type `%s` in this scope", n));
            return ty_error();
        }

        case TY_ARRAY:
            return ty_array(C->a, resolve_ty(C, t->as.array));

        case TY_MAP:
            return ty_map(C->a, resolve_ty(C, t->as.map.k), resolve_ty(C, t->as.map.v));

        case TY_TUPLE:
            cerr(C, t->at, "tuple types are not implemented yet");
            return ty_error();

        case TY_FN: {
            ty_vec ps;
            memset(&ps, 0, sizeof ps);
            for (int i = 0; i < t->as.fn.params.len; i++)
                vec_push(C->a, &ps, resolve_ty(C, t->as.fn.params.at[i]));
            ty *r = t->as.fn.ret ? resolve_ty(C, t->as.fn.ret) : ty_void();
            return ty_func(C->a, ps, r);
        }
    }
    return ty_error();
}

// Bring a function's own generics into scope while resolving its signature,
// so `[T](xs: [T])` sees `T`.
static fn_sig *sig_of(checker *C, func *f) {
    int saved = C->type_params.len;
    for (int i = 0; i < f->generics.len; i++) {
        type_param tp;
        memset(&tp, 0, sizeof tp);
        tp.name = f->generics.at[i].name;
        for (int k = 0; k < f->generics.at[i].bounds.len; k++)
            vec_push(C->a, &tp.bounds, path_text(C->a, &f->generics.at[i].bounds.at[k]));
        vec_push(C->a, &C->type_params, tp);
    }

    fn_sig *sig = NEW(C->a, fn_sig);
    sig->name = f->name;
    for (int i = 0; i < f->params.len; i++) {
        named_ty nt;
        nt.name = f->params.at[i].name;
        nt.t = resolve_ty(C, f->params.at[i].ty);
        vec_push(C->a, &sig->params, nt);
    }
    sig->ret = f->ret ? resolve_ty(C, f->ret) : ty_void();
    sig->generics = f->generics;
    sig->is_static = f->is_static;
    sig->is_mut = f->is_mut;
    sig->has_body = f->body != NULL;
    sig->at = f->at;

    C->type_params.len = saved;
    return sig;
}

// --- Generic structs/enums: monomorphised on demand -------------------------
//
// A generic struct/enum is never registered into `d->structs`/`d->enums`
// directly (see collect_names); it sits in struct_tmpls/enum_tmpls instead.
// The first concrete use (`Stack[int]` in a type, or a hinted literal)
// mangles a name, substitutes the args for the template's generics while
// resolving its fields/methods/variants, and appends the result as an
// ordinary IT_STRUCT/IT_ENUM item so collect_details/check_bodies/codegen
// process it exactly as if it had been written by hand. Same-module only:
// qualify() needs `current_module` to match, which this doesn't try to swap.

static ty *find_subst(checker *C, const char *name) {
    for (int i = C->active_subst.len - 1; i >= 0; i--)
        if (strcmp(C->active_subst.at[i].name, name) == 0) return C->active_subst.at[i].t;
    return NULL;
}

static const char *flatten_dots(arena *a, const char *s) {
    size_t n = strlen(s);
    char *buf = (char *)arena_alloc(a, n + 1);
    for (size_t i = 0; i <= n; i++) buf[i] = s[i] == '.' ? '_' : s[i];
    return buf;
}

static const char *mangle(arena *a, const char *qbase, ty_vec args) {
    const char *out = arena_printf(a, "%s[", qbase);
    for (int i = 0; i < args.len; i++) {
        const char *arg = flatten_dots(a, ty_show(a, args.at[i]));
        out = i == 0 ? arena_printf(a, "%s%s", out, arg)
                     : arena_printf(a, "%s,%s", out, arg);
    }
    return arena_printf(a, "%s]", out);
}

// Does `arg` implement the interface named `bound` (written in the
// template's own module)? Primitives implement nothing today.
static bool satisfies_bound(checker *C, ty *arg, const char *bound) {
    if (arg->kind != TK_NAMED) return false;
    struct_info *si = (struct_info *)map_get(&C->d->structs, arg->name);
    if (!si) return false;
    const char *dot = strchr(arg->name, '.');
    size_t mn = dot ? (size_t)(dot - arg->name) : strlen(arg->name);
    const char *want = strchr(bound, '.') ? bound : qualify(C, bound);
    if (!want) return false;
    for (int k = 0; k < si->implements.len; k++) {
        const char *iname = si->implements.at[k].name;
        const char *key = strchr(iname, '.') ? iname
                           : arena_printf(C->a, "%.*s.%s", (int)mn, arg->name, iname);
        if (strcmp(key, want) == 0) return true;
    }
    return false;
}

// Matches `pattern` (may contain TK_PARAM named in `params`) against a fully
// resolved `concrete`, filling `bound[i]` (parallel to `params`) the first
// time each param is pinned down and checking consistency after that.
static bool unify(ty *pattern, ty *concrete, generic_vec *params, ty_vec *bound) {
    if (concrete->kind == TK_ERROR) return true;
    if (pattern->kind == TK_PARAM) {
        for (int i = 0; i < params->len; i++) {
            if (strcmp(params->at[i].name, pattern->name) != 0) continue;
            if (!bound->at[i]) { bound->at[i] = concrete; return true; }
            return ty_eq(bound->at[i], concrete);
        }
        return false;
    }
    if (concrete->kind == TK_NULL_LIT) return ty_is_reference(pattern);
    if (pattern->kind != concrete->kind) return false;
    switch (pattern->kind) {
        case TK_ARRAY: return unify(pattern->elem, concrete->elem, params, bound);
        case TK_MAP:
            return unify(pattern->key, concrete->key, params, bound) &&
                   unify(pattern->val, concrete->val, params, bound);
        case TK_FUNC: {
            if (pattern->params.len != concrete->params.len) return false;
            for (int i = 0; i < pattern->params.len; i++)
                if (!unify(pattern->params.at[i], concrete->params.at[i], params, bound))
                    return false;
            return unify(pattern->ret, concrete->ret, params, bound);
        }
        default: return ty_eq(pattern, concrete);
    }
}

// Replaces each TK_PARAM in `t` with its binding from `unify`, or ty_error()
// if a param never got pinned down (already diagnosed by the caller).
static ty *subst_call_ty(checker *C, ty *t, generic_vec *params, ty_vec *bound) {
    switch (t->kind) {
        case TK_PARAM:
            for (int i = 0; i < params->len; i++)
                if (strcmp(params->at[i].name, t->name) == 0)
                    return bound->at[i] ? bound->at[i] : ty_error();
            return t;
        case TK_ARRAY: return ty_array(C->a, subst_call_ty(C, t->elem, params, bound));
        case TK_MAP:
            return ty_map(C->a, subst_call_ty(C, t->key, params, bound),
                          subst_call_ty(C, t->val, params, bound));
        case TK_FUNC: {
            ty_vec ps;
            memset(&ps, 0, sizeof ps);
            for (int i = 0; i < t->params.len; i++)
                vec_push(C->a, &ps, subst_call_ty(C, t->params.at[i], params, bound));
            return ty_func(C->a, ps, subst_call_ty(C, t->ret, params, bound));
        }
        default: return t;
    }
}

// `f[T](x: T)` calls: unify T from the arguments, since there's no call-site
// `f[int](x)` syntax (no turbofish, by design). The body was already checked
// once with T opaque, so no per-call recompilation is needed here.
static ty *check_generic_call(checker *C, expr *e, fn_sig *sig, expr_vec *args) {
    if (args->len != sig->params.len)
        cerr(C, e->at, arena_printf(C->a, "this call takes %d argument(s) but %d were supplied",
                                     sig->params.len, args->len));

    ty_vec bound;
    memset(&bound, 0, sizeof bound);
    for (int i = 0; i < sig->generics.len; i++) vec_push(C->a, &bound, (ty *)0);

    int n = args->len < sig->params.len ? args->len : sig->params.len;
    for (int i = 0; i < n; i++) {
        ty *got = check_expr(C, args->at[i]);
        got = adopt_empty_literal(args->at[i], got, sig->params.at[i].t);
        if (!unify(sig->params.at[i].t, got, &sig->generics, &bound))
            cerr(C, args->at[i]->at,
                 arena_printf(C->a, "expected `%s`, found `%s`",
                              ty_show(C->a, sig->params.at[i].t), ty_show(C->a, got)));
    }
    for (int i = n; i < args->len; i++) check_expr(C, args->at[i]);

    for (int i = 0; i < sig->generics.len; i++) {
        if (!bound.at[i]) {
            cerr(C, e->at, arena_printf(C->a, "cannot infer type argument `%s`",
                                         sig->generics.at[i].name));
            bound.at[i] = ty_error();
            continue;
        }
        for (int b = 0; b < sig->generics.at[i].bounds.len; b++) {
            const char *bn = path_text(C->a, &sig->generics.at[i].bounds.at[b]);
            if (!satisfies_bound(C, bound.at[i], bn))
                cerr(C, e->at, arena_printf(C->a, "`%s` does not implement `%s`",
                                             ty_show(C->a, bound.at[i]), bn));
        }
    }

    return subst_call_ty(C, sig->ret, &sig->generics, &bound);
}

static const char *instantiate_struct(checker *C, const char *qbase, struct_decl *tmpl,
                                       ty_vec args, span at) {
    const char *mangled = intern_z(C->in, mangle(C->a, qbase, args));
    if (map_has(&C->instantiated, mangled)) return mangled;
    map_put(&C->instantiated, mangled, (void *)1);

    if (tmpl->generics.len != args.len) {
        cerr(C, at, arena_printf(C->a, "`%s` takes %d type argument(s), found %d",
                                  qbase, tmpl->generics.len, args.len));
        return mangled;
    }
    for (int i = 0; i < tmpl->generics.len; i++)
        for (int b = 0; b < tmpl->generics.at[i].bounds.len; b++) {
            const char *bn = path_text(C->a, &tmpl->generics.at[i].bounds.at[b]);
            if (!satisfies_bound(C, args.at[i], bn))
                cerr(C, at, arena_printf(C->a,
                     "`%s` does not implement `%s`, required by `%s`'s bound on `%s`",
                     ty_show(C->a, args.at[i]), bn, qbase, tmpl->generics.at[i].name));
        }

    struct_decl *clone = NEW(C->a, struct_decl);
    *clone = *tmpl;
    clone->name = mangled;
    memset(&clone->generics, 0, sizeof clone->generics);

    struct_info *si = NEW(C->a, struct_info);
    si->at = at;
    map_put(&C->d->structs, mangled, si);

    int mark = C->active_subst.len;
    for (int i = 0; i < tmpl->generics.len; i++) {
        named_ty nt;
        nt.name = tmpl->generics.at[i].name;
        nt.t = args.at[i];
        vec_push(C->a, &C->active_subst, nt);
    }

    for (int k = 0; k < clone->fields.len; k++) {
        field *f = &clone->fields.at[k];
        field_info fi;
        fi.name = f->name;
        fi.t = resolve_ty(C, f->ty);
        fi.is_weak = f->is_weak;
        fi.at = f->at;
        vec_push(C->a, &si->fields, fi);
    }
    for (int k = 0; k < clone->methods.len; k++)
        vec_push(C->a, &si->methods, sig_of(C, clone->methods.at[k]));
    resolve_implements(C, si, &clone->implements);

    C->active_subst.len = mark;

    item it;
    memset(&it, 0, sizeof it);
    it.kind = IT_STRUCT;
    it.as.st = clone;
    vec_push(C->a, &C->current_prog->items, it);

    return mangled;
}

static const char *instantiate_enum(checker *C, const char *qbase, enum_decl *tmpl,
                                     ty_vec args, span at) {
    const char *mangled = intern_z(C->in, mangle(C->a, qbase, args));
    if (map_has(&C->instantiated, mangled)) return mangled;
    map_put(&C->instantiated, mangled, (void *)1);

    if (tmpl->generics.len != args.len) {
        cerr(C, at, arena_printf(C->a, "`%s` takes %d type argument(s), found %d",
                                  qbase, tmpl->generics.len, args.len));
        return mangled;
    }

    enum_decl *clone = NEW(C->a, enum_decl);
    *clone = *tmpl;
    clone->name = mangled;
    memset(&clone->generics, 0, sizeof clone->generics);

    enum_info *ei = NEW(C->a, enum_info);
    ei->at = at;
    map_put(&C->d->enums, mangled, ei);

    int mark = C->active_subst.len;
    for (int i = 0; i < tmpl->generics.len; i++) {
        named_ty nt;
        nt.name = tmpl->generics.at[i].name;
        nt.t = args.at[i];
        vec_push(C->a, &C->active_subst, nt);
    }

    for (int k = 0; k < clone->variants.len; k++) {
        variant *v = &clone->variants.at[k];
        variant_info vi;
        memset(&vi, 0, sizeof vi);
        vi.name = v->name;
        for (int j = 0; j < v->payload.len; j++) {
            named_ty nt;
            nt.name = v->payload.at[j].name;
            nt.t = resolve_ty(C, v->payload.at[j].ty);
            vec_push(C->a, &vi.payload, nt);
        }
        vec_push(C->a, &ei->variants, vi);
    }

    C->active_subst.len = mark;

    item it;
    memset(&it, 0, sizeof it);
    it.kind = IT_ENUM;
    it.as.en = clone;
    vec_push(C->a, &C->current_prog->items, it);

    return mangled;
}

static const char *instantiate_interface(checker *C, const char *qbase, interface_decl *tmpl,
                                          ty_vec args, span at) {
    const char *mangled = intern_z(C->in, mangle(C->a, qbase, args));
    if (map_has(&C->instantiated, mangled)) return mangled;
    map_put(&C->instantiated, mangled, (void *)1);

    if (tmpl->generics.len != args.len) {
        cerr(C, at, arena_printf(C->a, "`%s` takes %d type argument(s), found %d",
                                  qbase, tmpl->generics.len, args.len));
        return mangled;
    }

    interface_decl *clone = NEW(C->a, interface_decl);
    *clone = *tmpl;
    clone->name = mangled;
    memset(&clone->generics, 0, sizeof clone->generics);

    interface_info *ii = NEW(C->a, interface_info);
    ii->at = at;
    map_put(&C->d->interfaces, mangled, ii);

    int mark = C->active_subst.len;
    for (int i = 0; i < tmpl->generics.len; i++) {
        named_ty nt;
        nt.name = tmpl->generics.at[i].name;
        nt.t = args.at[i];
        vec_push(C->a, &C->active_subst, nt);
    }

    for (int k = 0; k < clone->methods.len; k++)
        vec_push(C->a, &ii->methods, sig_of(C, clone->methods.at[k]));

    C->active_subst.len = mark;

    item it;
    memset(&it, 0, sizeof it);
    it.kind = IT_INTERFACE;
    it.as.iface = clone;
    vec_push(C->a, &C->current_prog->items, it);

    return mangled;
}

static void resolve_implements(checker *C, struct_info *si, iface_ref_vec *refs) {
    for (int k = 0; k < refs->len; k++) {
        iface_ref *r = &refs->at[k];
        implemented im;
        im.at = r->at;
        if (r->args.len == 0) {
            im.name = path_text(C->a, &r->p);
        } else {
            const char *base = path_text(C->a, &r->p);
            const char *q = qualify(C, base);
            interface_decl *itmpl = q ? (interface_decl *)map_get(&C->iface_tmpls, q) : NULL;
            if (!itmpl) {
                cerr(C, r->at, arena_printf(C->a, "`%s` is not a generic interface", base));
                continue;
            }
            ty_vec resolved;
            memset(&resolved, 0, sizeof resolved);
            for (int a = 0; a < r->args.len; a++)
                vec_push(C->a, &resolved, resolve_ty(C, r->args.at[a]));
            im.name = instantiate_interface(C, q, itmpl, resolved, r->at);
        }
        vec_push(C->a, &si->implements, im);
    }
}

static void collect_names(checker *C, program *p) {
    for (int i = 0; i < p->items.len; i++) {
        item *it = &p->items.at[i];
        switch (it->kind) {
            case IT_STRUCT: {
                const char *qn = qualify(C, it->as.st->name);
                if (it->as.st->generics.len > 0) {
                    map_put(&C->struct_tmpls, intern_z(C->in, qn), it->as.st);
                    break;
                }
                if (map_has(&C->d->structs, qn) || map_has(&C->d->enums, qn))
                    cerr(C, it->as.st->at,
                         arena_printf(C->a, "`%s` is declared more than once", it->as.st->name));
                struct_info *si = NEW(C->a, struct_info);
                si->at = it->as.st->at;
                map_put(&C->d->structs, intern_z(C->in, qn), si);
                break;
            }
            case IT_ENUM: {
                const char *qn = qualify(C, it->as.en->name);
                if (it->as.en->generics.len > 0) {
                    map_put(&C->enum_tmpls, intern_z(C->in, qn), it->as.en);
                    break;
                }
                if (map_has(&C->d->structs, qn) || map_has(&C->d->enums, qn))
                    cerr(C, it->as.en->at,
                         arena_printf(C->a, "`%s` is declared more than once", it->as.en->name));
                enum_info *ei = NEW(C->a, enum_info);
                ei->at = it->as.en->at;
                map_put(&C->d->enums, intern_z(C->in, qn), ei);
                break;
            }
            case IT_INTERFACE: {
                const char *qn = qualify(C, it->as.iface->name);
                if (it->as.iface->generics.len > 0) {
                    map_put(&C->iface_tmpls, intern_z(C->in, qn), it->as.iface);
                    break;
                }
                interface_info *ii = NEW(C->a, interface_info);
                ii->at = it->as.iface->at;
                map_put(&C->d->interfaces, intern_z(C->in, qn), ii);
                break;
            }
            default:
                break;
        }
    }
}

static void collect_details(checker *C, program *p) {
    C->current_prog = p;
    for (int i = 0; i < p->items.len; i++) {
        item *it = &p->items.at[i];
        switch (it->kind) {
            case IT_STRUCT: {
                struct_decl *s = it->as.st;
                const char *sq = qualify(C, s->name);
                if (sq && map_has(&C->instantiated, sq)) break;
                struct_info *si = (struct_info *)map_get(&C->d->structs, sq);
                if (!si) break;

                for (int k = 0; k < s->fields.len; k++) {
                    field *f = &s->fields.at[k];
                    for (int j = 0; j < k; j++) {
                        if (strcmp(s->fields.at[j].name, f->name) == 0) {
                            cerr(C, f->at,
                                 arena_printf(C->a, "field `%s` is declared more than once",
                                              f->name));
                            break;
                        }
                    }
                    ty *t = resolve_ty(C, f->ty);
                    if (f->is_weak && !ty_is_reference(t)) {
                        cerr_help(C, f->at,
                                  "only reference types are counted, so only they can be weak",
                                  arena_printf(C->a, "`weak` field `%s` has value type `%s`",
                                               f->name, ty_show(C->a, t)));
                    }
                    field_info fi;
                    fi.name = f->name;
                    fi.t = t;
                    fi.is_weak = f->is_weak;
                    fi.at = f->at;
                    vec_push(C->a, &si->fields, fi);
                }

                for (int k = 0; k < s->methods.len; k++) {
                    fn_sig *sig = sig_of(C, s->methods.at[k]);
                    if (sig_lookup(&si->methods, sig->name)) {
                        cerr(C, s->methods.at[k]->at,
                             arena_printf(C->a, "method `%s` is declared more than once",
                                          sig->name));
                    }
                    vec_push(C->a, &si->methods, sig);
                }

                resolve_implements(C, si, &s->implements);
                break;
            }

            case IT_ENUM: {
                enum_decl *e = it->as.en;
                const char *eq = qualify(C, e->name);
                if (eq && map_has(&C->instantiated, eq)) break;
                enum_info *ei = (enum_info *)map_get(&C->d->enums, eq);
                if (!ei) break;
                for (int k = 0; k < e->variants.len; k++) {
                    variant *v = &e->variants.at[k];
                    for (int j = 0; j < k; j++) {
                        if (strcmp(e->variants.at[j].name, v->name) == 0) {
                            cerr(C, v->at,
                                 arena_printf(C->a, "variant `%s` is declared more than once",
                                              v->name));
                            break;
                        }
                    }
                    variant_info vi;
                    memset(&vi, 0, sizeof vi);
                    vi.name = v->name;
                    for (int j = 0; j < v->payload.len; j++) {
                        named_ty nt;
                        nt.name = v->payload.at[j].name;
                        nt.t = resolve_ty(C, v->payload.at[j].ty);
                        vec_push(C->a, &vi.payload, nt);
                    }
                    vec_push(C->a, &ei->variants, vi);
                }
                break;
            }

            case IT_INTERFACE: {
                interface_decl *d = it->as.iface;
                const char *dq = qualify(C, d->name);
                if (dq && map_has(&C->instantiated, dq)) break;
                interface_info *ii = (interface_info *)map_get(&C->d->interfaces, dq);
                if (!ii) break;    // a template; only its instantiations get checked
                for (int k = 0; k < d->methods.len; k++)
                    vec_push(C->a, &ii->methods, sig_of(C, d->methods.at[k]));
                break;
            }

            case IT_FUNC:
            case IT_EXTERN: {
                func *f = it->as.fn;
                const char *q = intern_z(C->in, qualify(C, f->name));
                if (map_has(&C->d->funcs, q))
                    cerr(C, f->at,
                         arena_printf(C->a, "`%s` is declared more than once", f->name));
                map_put(&C->d->funcs, q, sig_of(C, f));
                if (it->kind == IT_EXTERN) map_put(&C->d->externs, q, (void *)1);
                break;
            }

            default:
                break;
        }
    }
}

// Record the type of each module-level `let`, so other modules can name it.
// Runs after every module's signatures are known, so a constant may be
// initialised by a call.
static void collect_consts(checker *C, program *p) {
    for (int i = 0; i < p->items.len; i++) {
        item *it = &p->items.at[i];
        if (it->kind != IT_LET) continue;
        stmt *s = it->as.let;
        if (s->kind != ST_LET) continue;
        ty *t = s->as.let_.ty ? resolve_ty(C, s->as.let_.ty)
                              : check_expr(C, s->as.let_.value);
        map_put(&C->d->consts, intern_z(C->in, qualify(C, s->as.let_.name)), t);
    }
}

static const char *sig_params_show(checker *C, const fn_sig *sig, bool with_names) {
    const char *ps = "";
    for (int i = 0; i < sig->params.len; i++) {
        const char *one = with_names
            ? arena_printf(C->a, "%s: %s", sig->params.at[i].name,
                           ty_show(C->a, sig->params.at[i].t))
            : ty_show(C->a, sig->params.at[i].t);
        ps = i == 0 ? one : arena_printf(C->a, "%s, %s", ps, one);
    }
    return ps;
}

static void check_conformance(checker *C) {
    for (int si = 0; si < C->d->structs.cap; si++) {
        const char *sname = C->d->structs.slots[si].key;
        if (!sname || map_has(&C->conformance_checked, sname)) continue;
        map_put(&C->conformance_checked, sname, (void *)1);
        struct_info *s = (struct_info *)C->d->structs.slots[si].val;

        for (int k = 0; k < s->implements.len; k++) {
            const char *iname = s->implements.at[k].name;
            span iat = s->implements.at[k].at;

            // `implements` is written unqualified inside its own module.
            const char *key;
            if (strchr(iname, '.')) {
                key = iname;
            } else {
                const char *dot = strchr(sname, '.');
                size_t mn = dot ? (size_t)(dot - sname) : strlen(sname);
                key = arena_printf(C->a, "%.*s.%s", (int)mn, sname, iname);
            }

            interface_info *iface = (interface_info *)map_get(&C->d->interfaces, key);
            if (!iface) {
                cerr(C, iat, arena_printf(C->a, "cannot find interface `%s`", iname));
                continue;
            }

            for (int m = 0; m < iface->methods.len; m++) {
                fn_sig *isig = iface->methods.at[m];
                fn_sig *ssig = sig_lookup(&s->methods, isig->name);

                const char *ret_part = isig->ret->kind == TK_VOID
                    ? "" : arena_printf(C->a, ": %s", ty_show(C->a, isig->ret));

                if (!ssig) {
                    // A default implementation satisfies the requirement.
                    if (!isig->has_body) {
                        cerr_help(C, s->at,
                                  arena_printf(C->a, "add `func %s(%s)%s`", isig->name,
                                               sig_params_show(C, isig, true), ret_part),
                                  arena_printf(C->a,
                                               "`%s` does not implement `%s` required by `%s`",
                                               sname, isig->name, iname));
                    }
                    continue;
                }

                bool same = isig->params.len == ssig->params.len &&
                            ty_eq(isig->ret, ssig->ret);
                for (int j = 0; same && j < isig->params.len; j++)
                    if (!ty_eq(isig->params.at[j].t, ssig->params.at[j].t)) same = false;

                if (!same) {
                    cerr_help(C, ssig->at,
                              arena_printf(C->a, "expected `(%s)%s`",
                                           sig_params_show(C, isig, false), ret_part),
                              arena_printf(C->a,
                                           "`%s.%s` does not match the signature required by `%s`",
                                           sname, isig->name, iname));
                }
            }
        }
    }
}

typedef VEC(const char *) name_vec;

// Struct names reachable from a type by following owned (non-weak) edges.
static void reachable_structs(arena *a, ty *t, name_vec *out) {
    switch (t->kind) {
        case TK_NAMED: vec_push(a, out, t->name); break;
        case TK_ARRAY: reachable_structs(a, t->elem, out); break;
        case TK_MAP:
            reachable_structs(a, t->key, out);
            reachable_structs(a, t->val, out);
            break;
        default: break;
    }
}

// Depth-first search for a path from `current` back to `target`. Reports the
// (struct, field) edge that closes the cycle.
static bool find_cycle(checker *C, const char *target, const char *current,
                       name_vec *seen, const char **owner, const char **fname) {
    for (int i = 0; i < seen->len; i++)
        if (strcmp(seen->at[i], current) == 0) return false;
    vec_push(C->a, seen, current);

    struct_info *info = (struct_info *)map_get(&C->d->structs, current);
    if (!info) return false;

    for (int i = 0; i < info->fields.len; i++) {
        field_info *f = &info->fields.at[i];
        if (f->is_weak) continue;       // a weak edge cannot keep a cycle alive

        name_vec reached;
        memset(&reached, 0, sizeof reached);
        reachable_structs(C->a, f->t, &reached);

        for (int k = 0; k < reached.len; k++) {
            if (strcmp(reached.at[k], target) == 0) {
                *owner = current;
                *fname = f->name;
                return true;
            }
            if (find_cycle(C, target, reached.at[k], seen, owner, fname)) return true;
        }
    }
    return false;
}

// Find reference cycles among struct fields and point at the field that closes
// the loop. This is what makes ARC's one weakness a compile-time diagnostic
// rather than a silent leak.
static void check_cycles(checker *C) {
    name_vec reported;
    memset(&reported, 0, sizeof reported);

    for (int i = 0; i < C->d->structs.cap; i++) {
        const char *start = C->d->structs.slots[i].key;
        if (!start || map_has(&C->cycles_checked, start)) continue;
        map_put(&C->cycles_checked, start, (void *)1);

        name_vec seen;
        memset(&seen, 0, sizeof seen);
        const char *owner = NULL;
        const char *fname = NULL;
        if (!find_cycle(C, start, start, &seen, &owner, &fname)) continue;

        // Report each cycle once, keyed on the edge that closes it.
        const char *key = arena_printf(C->a, "%s.%s", owner, fname);
        bool dup = false;
        for (int k = 0; k < reported.len; k++)
            if (strcmp(reported.at[k], key) == 0) { dup = true; break; }
        if (dup) continue;
        vec_push(C->a, &reported, key);

        struct_info *info = (struct_info *)map_get(&C->d->structs, owner);
        span at = info->at;
        for (int k = 0; k < info->fields.len; k++)
            if (strcmp(info->fields.at[k].name, fname) == 0) at = info->fields.at[k].at;

        cerr_help(C, at,
                  arena_printf(C->a, "mark it `weak %s: ...` to break the cycle", fname),
                  arena_printf(C->a,
                               "field `%s.%s` closes a reference cycle, which reference "
                               "counting cannot free", owner, fname));
    }
}

static void want_bool(checker *C, ty *got, span at) {
    if (!ty_assignable(got, ty_bool()))
        cerr(C, at, arena_printf(C->a, "condition must be `bool`, found `%s`",
                                 ty_show(C->a, got)));
}

static void check_func(checker *C, func *f, const char *owner) {
    if (!f->body) return;

    int saved_params = C->type_params.len;
    for (int i = 0; i < f->generics.len; i++) {
        generic_param *g = &f->generics.at[i];
        type_param tp;
        memset(&tp, 0, sizeof tp);
        tp.name = g->name;
        for (int k = 0; k < g->bounds.len; k++) {
            const char *bn = path_text(C->a, &g->bounds.at[k]);
            const char *q = qualify(C, bn);
            if (!q || !map_has(&C->d->interfaces, q))
                cerr(C, g->bounds.at[k].at,
                     arena_printf(C->a, "cannot find interface `%s`", bn));
            vec_push(C->a, &tp.bounds, bn);
        }
        vec_push(C->a, &C->type_params, tp);
    }

    // Prefer the collected signature; fall back to re-resolving it, because a
    // missing entry is not worth crashing over.
    fn_sig *sig = NULL;
    if (owner) {
        const char *q = qualify(C, owner);
        struct_info *si = q ? (struct_info *)map_get(&C->d->structs, q) : NULL;
        if (si) sig = sig_lookup(&si->methods, f->name);
    } else {
        const char *q = qualify(C, f->name);
        if (q) sig = (fn_sig *)map_get(&C->d->funcs, q);
    }
    if (!sig) sig = sig_of(C, f);

    if (owner && !f->is_static) {
        C->this_ty = ty_named(C->a, intern_z(C->in, qualify(C, owner)));
        C->this_mut = f->is_mut;
    } else {
        C->this_ty = NULL;
        C->this_mut = false;
    }
    C->ret_ty = sig->ret;

    push_scope(C);
    for (int i = 0; i < sig->params.len; i++)
        declare(C, sig->params.at[i].name, sig->params.at[i].t, false);
    for (int i = 0; i < f->body->stmts.len; i++)
        check_stmt(C, f->body->stmts.at[i]);
    pop_scope(C);

    C->this_ty = NULL;
    C->this_mut = false;
    C->ret_ty = ty_void();
    C->type_params.len = saved_params;
}

static void check_bodies(checker *C, program *p) {
    for (int i = 0; i < p->items.len; i++) {
        item *it = &p->items.at[i];
        switch (it->kind) {
            case IT_LET:
                check_stmt(C, it->as.let);
                break;
            case IT_FUNC:
                check_func(C, it->as.fn, NULL);
                break;
            case IT_STRUCT:
                if (it->as.st->generics.len > 0) break;    // template, not a real type
                for (int k = 0; k < it->as.st->methods.len; k++)
                    check_func(C, it->as.st->methods.at[k], it->as.st->name);
                break;
            case IT_INTERFACE:
                if (it->as.iface->generics.len > 0) break;
                for (int k = 0; k < it->as.iface->methods.len; k++)
                    if (it->as.iface->methods.at[k]->body)
                        check_func(C, it->as.iface->methods.at[k], NULL);
                break;
            default:
                break;
        }
    }
}

static void want_pat_ty(checker *C, ty *got, ty *want, span at) {
    if (!ty_assignable(want, got))
        cerr(C, at, arena_printf(C->a, "pattern of type `%s` cannot match `%s`",
                                 ty_show(C->a, want), ty_show(C->a, got)));
}

static bool covered_has(name_vec *v, const char *n) {
    for (int i = 0; i < v->len; i++)
        if (strcmp(v->at[i], n) == 0) return true;
    return false;
}

static void check_pattern(checker *C, pattern *pat, ty *sty,
                          const char *enum_name, name_vec *covered) {
    switch (pat->kind) {
        case PAT_WILDCARD: {
            if (enum_name) {
                enum_info *ei = (enum_info *)map_get(&C->d->enums, enum_name);
                if (ei)
                    for (int i = 0; i < ei->variants.len; i++)
                        if (!covered_has(covered, ei->variants.at[i].name))
                            vec_push(C->a, covered, ei->variants.at[i].name);
            }
            return;
        }
        case PAT_INT: want_pat_ty(C, sty, ty_int(), pat->at); return;
        case PAT_BOOL: want_pat_ty(C, sty, ty_bool(), pat->at); return;
        case PAT_STR: want_pat_ty(C, sty, ty_str(), pat->at); return;
        case PAT_NULL:
            if (!ty_is_reference(sty) && sty->kind != TK_ERROR)
                cerr(C, pat->at,
                     arena_printf(C->a, "`%s` can never be null", ty_show(C->a, sty)));
            return;

        case PAT_VARIANT: {
            path *p = &pat->as.variant.p;
            const char *en = NULL;
            const char *vname = NULL;

            if (p->n == 1) {
                // `Variant`, with the enum inferred from the scrutinee.
                if (!enum_name) {
                    cerr(C, pat->at,
                         arena_printf(C->a, "cannot find `%s` in this scope",
                                      path_text(C->a, p)));
                    return;
                }
                en = enum_name;
                vname = p->seg[0];
            } else if (p->n == 2) {
                en = p->seg[0];
                vname = p->seg[1];
            } else if (p->n == 3) {
                // `module.Enum.Variant`
                en = arena_printf(C->a, "%s.%s", p->seg[0], p->seg[1]);
                vname = p->seg[2];
            } else {
                cerr(C, pat->at,
                     arena_printf(C->a, "`%s` is not a valid pattern", path_text(C->a, p)));
                return;
            }

            const char *enq = qualify(C, en);
            if (!enq) enq = en;

            if (enum_name) {
                size_t qn = strlen(enq);
                bool is_instance_of = strncmp(enq, enum_name, qn) == 0 && enum_name[qn] == '[';
                if (strcmp(enq, enum_name) != 0 && !is_instance_of) {
                    cerr(C, pat->at,
                         arena_printf(C->a, "pattern is `%s` but the switch is over `%s`",
                                      en, enum_name));
                    return;
                }
                enq = enum_name;
            }

            enum_info *info = (enum_info *)map_get(&C->d->enums, enq);
            if (!info) {
                cerr(C, pat->at, arena_printf(C->a, "cannot find enum `%s`", en));
                return;
            }

            variant_info *vi = NULL;
            for (int i = 0; i < info->variants.len; i++)
                if (strcmp(info->variants.at[i].name, vname) == 0) vi = &info->variants.at[i];

            if (!vi) {
                const char *known = "";
                for (int i = 0; i < info->variants.len; i++)
                    known = i == 0 ? info->variants.at[i].name
                                   : arena_printf(C->a, "%s, %s", known,
                                                  info->variants.at[i].name);
                cerr_help(C, pat->at,
                          arena_printf(C->a, "known variants: %s", known),
                          arena_printf(C->a, "`%s` has no variant `%s`", en, vname));
                return;
            }
            if (!covered_has(covered, vname)) vec_push(C->a, covered, vname);

            binding_vec *bs = &pat->as.variant.bindings;
            if (bs->len && bs->len != vi->payload.len) {
                cerr(C, pat->at,
                     arena_printf(C->a,
                                  "`%s.%s` has %d payload field(s), but %d are bound",
                                  en, vname, vi->payload.len, bs->len));
            }
            for (int i = 0; i < bs->len; i++) {
                ty *t = i < vi->payload.len ? vi->payload.at[i].t : ty_error();
                declare(C, bs->at[i].name, t, false);
            }
            return;
        }
    }
}

// Exhaustiveness is the headline diagnostic: a missed variant is named.
static void check_switch(checker *C, stmt *s) {
    ty *sty = check_expr(C, s->as.switch_.scrutinee);

    const char *enum_name = NULL;
    if (sty->kind == TK_NAMED && map_has(&C->d->enums, sty->name)) enum_name = sty->name;

    name_vec covered;
    memset(&covered, 0, sizeof covered);

    case_vec *cases = &s->as.switch_.cases;
    for (int i = 0; i < cases->len; i++) {
        push_scope(C);
        for (int k = 0; k < cases->at[i].patterns.len; k++)
            check_pattern(C, &cases->at[i].patterns.at[k], sty, enum_name, &covered);
        for (int k = 0; k < cases->at[i].body.len; k++)
            check_stmt(C, cases->at[i].body.at[k]);
        pop_scope(C);
    }

    if (s->as.switch_.dflt) {
        if (enum_name) {
            cerr_help(C, s->as.switch_.dflt->at,
                      "cover every variant instead, so adding one later is a compile error",
                      "`default` is not allowed when switching on an enum");
        }
        check_block(C, s->as.switch_.dflt);
    }

    if (enum_name) {
        enum_info *ei = (enum_info *)map_get(&C->d->enums, enum_name);
        const char *list = "";
        int missing = 0;
        for (int i = 0; i < ei->variants.len; i++) {
            const char *v = ei->variants.at[i].name;
            if (covered_has(&covered, v)) continue;
            const char *one = arena_printf(C->a, "`%s.%s`", enum_name, v);
            list = missing == 0 ? one : arena_printf(C->a, "%s, %s", list, one);
            missing++;
        }
        if (missing) {
            cerr_help(C, s->at,
                      "add the missing case, or switch on something other than an enum "
                      "if you want a `default`",
                      arena_printf(C->a, "non-exhaustive switch: %s %s not handled",
                                   list, missing == 1 ? "is" : "are"));
        }
    }
}

static void check_block(checker *C, block *b) {
    push_scope(C);
    for (int i = 0; i < b->stmts.len; i++) check_stmt(C, b->stmts.at[i]);
    pop_scope(C);
}

static void check_stmt(checker *C, stmt *s) {
    switch (s->kind) {
        case ST_LET: {
            expr *val = s->as.let_.value;
            // Generic literals infer their type args from the declared type.
            bool needs_hint = val->kind == EX_STRUCT_LIT ||
                (val->kind == EX_CALL && val->as.call.callee->kind == EX_FIELD);
            ty *want = (needs_hint && s->as.let_.ty) ? resolve_ty(C, s->as.let_.ty) : NULL;
            if (want) val->hint = want;

            ty *got = check_expr(C, val);
            ty *final_ty;
            if (s->as.let_.ty) {
                if (!want) want = resolve_ty(C, s->as.let_.ty);
                got = adopt_empty_literal(s->as.let_.value, got, want);
                if (!ty_assignable(got, want))
                    cerr(C, s->as.let_.value->at,
                         arena_printf(C->a, "expected `%s`, found `%s`",
                                      ty_show(C->a, want), ty_show(C->a, got)));
                final_ty = want;
            } else if (got->kind == TK_NULL_LIT) {
                cerr_help(C, s->at,
                          "annotate the binding, for example `let x: Player = null;`",
                          "cannot infer a type from `null` alone");
                final_ty = ty_error();
            } else {
                final_ty = got;
            }
            declare(C, s->as.let_.name, final_ty, s->as.let_.is_var);
            return;
        }

        case ST_ASSIGN: {
            expr *place = s->as.assign.place;
            ty *target = check_expr(C, place);
            ty *got = check_expr(C, s->as.assign.value);

            // Immutability and receiver-mutability live here.
            if (place->kind == EX_PATH && place->as.p.n == 1) {
                binding_entry *b = lookup(C, place->as.p.seg[0]);
                if (b && !b->assignable) {
                    cerr_help(C, place->at,
                              "it is bound with `let`; use `var` to allow mutation",
                              arena_printf(C->a, "cannot assign to `%s`",
                                           path_text(C->a, &place->as.p)));
                }
            }
            if (place->kind == EX_FIELD && place->as.field.base->kind == EX_THIS) {
                if (C->this_ty && !C->this_mut) {
                    cerr_help(C, place->at,
                              "declare the method `mut func` to let it modify the receiver",
                              "cannot assign to a field of `this`");
                }
            }

            if (s->as.assign.op == ASSIGN_SET) {
                if (!ty_assignable(got, target))
                    cerr(C, s->as.assign.value->at,
                         arena_printf(C->a, "expected `%s`, found `%s`",
                                      ty_show(C->a, target), ty_show(C->a, got)));
            } else if (!ty_is_numeric(target) && target->kind != TK_ERROR) {
                cerr(C, place->at,
                     arena_printf(C->a,
                                  "compound assignment needs a numeric type, found `%s`",
                                  ty_show(C->a, target)));
            } else if (!ty_assignable(adopt_empty_literal(s->as.assign.value, got, target),
                                      target)) {
                cerr(C, s->as.assign.value->at,
                     arena_printf(C->a, "expected `%s`, found `%s`",
                                  ty_show(C->a, target), ty_show(C->a, got)));
            }
            return;
        }

        case ST_IF: {
            ty *c = check_expr(C, s->as.if_.cond);
            want_bool(C, c, s->as.if_.cond->at);
            check_block(C, s->as.if_.then);
            if (s->as.if_.els) check_stmt(C, s->as.if_.els);
            return;
        }

        case ST_WHILE: {
            ty *c = check_expr(C, s->as.while_.cond);
            want_bool(C, c, s->as.while_.cond->at);
            check_block(C, s->as.while_.body);
            return;
        }

        case ST_FOR: {
            ty *it = check_expr(C, s->as.for_.iter);
            ty *elem;
            if (it->kind == TK_ARRAY) elem = it->elem;
            else if (it->kind == TK_ERROR) elem = ty_error();
            else {
                cerr(C, s->as.for_.iter->at,
                     arena_printf(C->a, "`%s` is not iterable", ty_show(C->a, it)));
                elem = ty_error();
            }
            push_scope(C);
            declare(C, s->as.for_.var, elem, false);
            for (int i = 0; i < s->as.for_.body->stmts.len; i++)
                check_stmt(C, s->as.for_.body->stmts.at[i]);
            pop_scope(C);
            return;
        }

        case ST_SWITCH:
            check_switch(C, s);
            return;

        case ST_ARENA:
        case ST_BLOCK:
            check_block(C, s->as.blk);
            return;

        case ST_RETURN: {
            ty *got = s->as.value ? check_expr(C, s->as.value) : ty_void();
            got = adopt_empty_literal(s->as.value, got, C->ret_ty);
            if (!ty_assignable(got, C->ret_ty)) {
                span at = s->as.value ? s->as.value->at : s->at;
                cerr(C, at, arena_printf(C->a, "expected `%s`, found `%s`",
                                         ty_show(C->a, C->ret_ty), ty_show(C->a, got)));
            }
            return;
        }

        case ST_BREAK:
        case ST_CONTINUE:
            return;

        case ST_EXPR:
            check_expr(C, s->as.value);
            return;
    }
}

// Signatures for the functions the runtime provides. `TK_ERROR` in a parameter
// slot means "accepts anything", which is how the few polymorphic builtins are
// typed without a full generics system.
// `sqrt`/`abs` duplicate std/math.sl's own extern decls of the same name;
// known wart, not untangled here.
static ty *builtin_sig(checker *C, const char *name) {
    ty_vec ps;
    memset(&ps, 0, sizeof ps);
    if (strcmp(name, "print") == 0) {
        vec_push(C->a, &ps, ty_error());
        return ty_func(C->a, ps, ty_void());
    }
    if (strcmp(name, "sqrt") == 0) {
        vec_push(C->a, &ps, ty_float());
        return ty_func(C->a, ps, ty_float());
    }
    if (strcmp(name, "abs") == 0) {
        vec_push(C->a, &ps, ty_error());
        return ty_func(C->a, ps, ty_error());
    }
    if (strcmp(name, "len") == 0) {
        vec_push(C->a, &ps, ty_error());
        return ty_func(C->a, ps, ty_int());
    }
    if (strcmp(name, "push") == 0) {
        vec_push(C->a, &ps, ty_error());
        vec_push(C->a, &ps, ty_error());
        return ty_func(C->a, ps, ty_void());
    }
    if (strcmp(name, "pop") == 0) {
        vec_push(C->a, &ps, ty_error());
        return ty_func(C->a, ps, ty_error());
    }
    if (strcmp(name, "reverse") == 0) {
        vec_push(C->a, &ps, ty_error());
        return ty_func(C->a, ps, ty_void());
    }
    if (strcmp(name, "indexOf") == 0) {
        vec_push(C->a, &ps, ty_error());
        vec_push(C->a, &ps, ty_error());
        return ty_func(C->a, ps, ty_int());
    }
    if (strcmp(name, "contains") == 0) {
        vec_push(C->a, &ps, ty_error());
        vec_push(C->a, &ps, ty_error());
        return ty_func(C->a, ps, ty_bool());
    }
    if (strcmp(name, "removeAt") == 0) {
        vec_push(C->a, &ps, ty_error());
        vec_push(C->a, &ps, ty_int());
        return ty_func(C->a, ps, ty_error());
    }
    if (strcmp(name, "insertAt") == 0) {
        vec_push(C->a, &ps, ty_error());
        vec_push(C->a, &ps, ty_int());
        vec_push(C->a, &ps, ty_error());
        return ty_func(C->a, ps, ty_void());
    }
    if (strcmp(name, "sort") == 0) {
        vec_push(C->a, &ps, ty_error());
        return ty_func(C->a, ps, ty_void());
    }
    if (strcmp(name, "removeKey") == 0) {
        vec_push(C->a, &ps, ty_error());
        vec_push(C->a, &ps, ty_error());
        return ty_func(C->a, ps, ty_error());
    }
    if (strcmp(name, "keys") == 0 || strcmp(name, "values") == 0) {
        vec_push(C->a, &ps, ty_error());
        return ty_func(C->a, ps, ty_array(C->a, ty_error()));
    }
    return NULL;
}

// Flatten a chain of identifier field accesses (`math.Vec2`) into a dotted
// string, or NULL if anything in it is a real expression.
static const char *static_path(arena *a, expr *e) {
    if (e->kind == EX_PATH && e->as.p.n == 1) return e->as.p.seg[0];
    if (e->kind == EX_FIELD && !e->as.field.optional) {
        const char *base = static_path(a, e->as.field.base);
        if (!base) return NULL;
        return arena_printf(a, "%s.%s", base, e->as.field.name);
    }
    return NULL;
}

// `Enum.Variant` or `Struct.staticMethod`, if `head` names a type. Sets
// `*found` false when `head` is not a type, so the caller can fall back to
// ordinary field access.
static ty *type_member(checker *C, const char *head, const char *name, span at, bool *found) {
    *found = true;

    // `math.something` where `math` is an imported module.
    if (is_visible_module(C, head)) {
        const char *q = arena_printf(C->a, "%s.%s", head, name);
        fn_sig *sig = (fn_sig *)map_get(&C->d->funcs, q);
        if (sig) return sig_as_func_ty(C, sig);
        ty *t = (ty *)map_get(&C->d->consts, q);
        if (t) return t;
        if (map_has(&C->d->structs, q) || map_has(&C->d->enums, q)) {
            // A bare type name is not a value; only `math.T.member` is.
            *found = false;
            return NULL;
        }
        cerr_help(C, at, arena_printf(C->a, "check what `%s.sl` declares", head),
                  arena_printf(C->a, "module `%s` has no `%s`", head, name));
        return ty_error();
    }

    const char *q = qualify(C, head);
    if (!q) { *found = false; return NULL; }

    enum_info *ei = (enum_info *)map_get(&C->d->enums, q);
    if (ei) {
        for (int i = 0; i < ei->variants.len; i++) {
            variant_info *vi = &ei->variants.at[i];
            if (strcmp(vi->name, name) != 0) continue;
            if (vi->payload.len == 0) return ty_named(C->a, intern_z(C->in, q));
            ty_vec ps;
            memset(&ps, 0, sizeof ps);
            for (int k = 0; k < vi->payload.len; k++) vec_push(C->a, &ps, vi->payload.at[k].t);
            return ty_func(C->a, ps, ty_named(C->a, intern_z(C->in, q)));
        }
        const char *known = "";
        for (int i = 0; i < ei->variants.len; i++)
            known = i == 0 ? ei->variants.at[i].name
                           : arena_printf(C->a, "%s, %s", known, ei->variants.at[i].name);
        cerr_help(C, at, arena_printf(C->a, "known variants: %s", known),
                  arena_printf(C->a, "`%s` has no variant `%s`", q, name));
        return ty_error();
    }

    struct_info *si = (struct_info *)map_get(&C->d->structs, q);
    if (si) {
        fn_sig *sig = sig_lookup(&si->methods, name);
        if (sig) {
            if (!sig->is_static) {
                cerr_help(C, at, "call it on an instance instead",
                          arena_printf(C->a, "`%s` is not a static method of `%s`", name, q));
                return ty_error();
            }
            return sig_as_func_ty(C, sig);
        }
        cerr(C, at, arena_printf(C->a, "`%s` has no static method `%s`", q, name));
        return ty_error();
    }

    *found = false;
    return NULL;
}

static ty *check_path(checker *C, path *p, span at) {
    if (p->n == 1) {
        const char *n = p->seg[0];
        binding_entry *b = lookup(C, n);
        if (b) return b->t;

        // A module's own declaration wins over a global builtin of the same
        // name (e.g. strings.sl's own `indexOf` over the array builtin).
        const char *q = qualify(C, n);
        if (q) {
            ty *cst = (ty *)map_get(&C->d->consts, q);
            if (cst) return cst;
            fn_sig *sig = (fn_sig *)map_get(&C->d->funcs, q);
            if (sig) return sig_as_func_ty(C, sig);
        }

        ty *bi = builtin_sig(C, n);
        if (bi) return bi;

        // Nothing by that name here, so try the prelude. A host declares its
        // externs once there and no script has to repeat them; a name defined
        // locally still wins, so a prelude cannot capture one.
        const char *pq = prelude_name(C, n);
        if (pq) {
            ty *cst = (ty *)map_get(&C->d->consts, pq);
            if (cst) return cst;
            fn_sig *sig = (fn_sig *)map_get(&C->d->funcs, pq);
            if (sig) return sig_as_func_ty(C, sig);
        }
        cerr(C, at, arena_printf(C->a, "cannot find `%s` in this scope", n));
        return ty_error();
    }

    // `Enum.Variant` as a value, or `Type.static_method`
    const char *head = p->seg[0];
    const char *tail = p->seg[1];

    enum_info *ei = (enum_info *)map_get(&C->d->enums, head);
    if (ei) {
        for (int i = 0; i < ei->variants.len; i++) {
            variant_info *vi = &ei->variants.at[i];
            if (strcmp(vi->name, tail) != 0) continue;
            if (vi->payload.len == 0) return ty_named(C->a, head);
            ty_vec ps;
            memset(&ps, 0, sizeof ps);
            for (int k = 0; k < vi->payload.len; k++) vec_push(C->a, &ps, vi->payload.at[k].t);
            return ty_func(C->a, ps, ty_named(C->a, head));
        }
        cerr(C, at, arena_printf(C->a, "`%s` has no variant `%s`", head, tail));
        return ty_error();
    }

    struct_info *si = (struct_info *)map_get(&C->d->structs, head);
    if (si) {
        fn_sig *sig = sig_lookup(&si->methods, tail);
        if (sig) {
            if (!sig->is_static) {
                cerr_help(C, at, "call it on an instance instead",
                          arena_printf(C->a, "`%s` is not a static method of `%s`", tail, head));
                return ty_error();
            }
            return sig_as_func_ty(C, sig);
        }
        cerr(C, at, arena_printf(C->a, "`%s` has no static method `%s`", head, tail));
        return ty_error();
    }

    // Otherwise treat it as a value with field access.
    path one;
    one.seg = p->seg;
    one.n = 1;
    one.at = at;
    ty *cur = check_path(C, &one, at);
    for (int i = 1; i < p->n; i++) cur = field_or_method_ty(C, cur, p->seg[i], at);
    return cur;
}

static ty *field_or_method_ty(checker *C, ty *base, const char *name, span at) {
    switch (base->kind) {
        case TK_ERROR:
            return ty_error();

        case TK_NAMED: {
            struct_info *si = (struct_info *)map_get(&C->d->structs, base->name);
            if (si) {
                for (int i = 0; i < si->fields.len; i++)
                    if (strcmp(si->fields.at[i].name, name) == 0) return si->fields.at[i].t;
                fn_sig *sig = sig_lookup(&si->methods, name);
                if (sig) return sig_as_func_ty(C, sig);

                const char *known = "";
                int n = 0;
                for (int i = 0; i < si->fields.len; i++, n++)
                    known = n == 0 ? si->fields.at[i].name
                                   : arena_printf(C->a, "%s, %s", known, si->fields.at[i].name);
                for (int i = 0; i < si->methods.len; i++, n++)
                    known = n == 0 ? si->methods.at[i]->name
                                   : arena_printf(C->a, "%s, %s", known, si->methods.at[i]->name);
                cerr_help(C, at,
                          n == 0 ? arena_printf(C->a, "`%s` has no members", base->name)
                                 : arena_printf(C->a, "available: %s", known),
                          arena_printf(C->a, "`%s` has no field or method `%s`",
                                       base->name, name));
                return ty_error();
            }
            if (map_has(&C->d->enums, base->name)) {
                cerr(C, at, arena_printf(C->a, "`%s` is an enum; match on it with `switch`",
                                         base->name));
                return ty_error();
            }
            return ty_error();
        }

        // A generic parameter only exposes what its bounds promise. This is the
        // dictionary-passing discipline, and it is why a generic body can be
        // checked once rather than per instantiation.
        case TK_PARAM: {
            type_param *tp = find_type_param(C, base->name);
            if (tp) {
                for (int i = 0; i < tp->bounds.len; i++) {
                    const char *q = qualify(C, tp->bounds.at[i]);
                    interface_info *ii =
                        q ? (interface_info *)map_get(&C->d->interfaces, q) : NULL;
                    if (!ii) ii = (interface_info *)map_get(&C->d->interfaces, tp->bounds.at[i]);
                    if (!ii) continue;
                    fn_sig *sig = sig_lookup(&ii->methods, name);
                    if (sig) return sig_as_func_ty(C, sig);
                }
            }
            if (!tp || tp->bounds.len == 0) {
                cerr_help(C, at,
                          arena_printf(C->a,
                                       "`%s` is unbounded; add a bound such as "
                                       "`[%s: SomeInterface]`", base->name, base->name),
                          arena_printf(C->a, "type parameter `%s` has no method `%s`",
                                       base->name, name));
            } else {
                const char *bs = "";
                for (int i = 0; i < tp->bounds.len; i++)
                    bs = i == 0 ? tp->bounds.at[i]
                                : arena_printf(C->a, "%s + %s", bs, tp->bounds.at[i]);
                cerr_help(C, at,
                          arena_printf(C->a, "its bounds (%s) do not declare it", bs),
                          arena_printf(C->a, "type parameter `%s` has no method `%s`",
                                       base->name, name));
            }
            return ty_error();
        }

        default:
            if (strcmp(name, "len") == 0 &&
                (base->kind == TK_ARRAY || base->kind == TK_MAP || base->kind == TK_STR)) {
                ty_vec none;
                memset(&none, 0, sizeof none);
                return ty_func(C->a, none, ty_int());
            }
            cerr(C, at, arena_printf(C->a, "`%s` has no field or method `%s`",
                                     ty_show(C->a, base), name));
            return ty_error();
    }
}

static ty *check_struct_lit(checker *C, expr *e) {
    path *p = &e->as.struct_lit.p;
    field_init_vec *fields = &e->as.struct_lit.fields;
    const char *written = path_text(C->a, p);
    const char *name = qualify(C, written);
    if (!name) name = written;

    struct_info *si = (struct_info *)map_get(&C->d->structs, name);
    if (!si && map_has(&C->struct_tmpls, name)) {
        if (e->hint && e->hint->kind == TK_NAMED) {
            name = e->hint->name;
            si = (struct_info *)map_get(&C->d->structs, name);
        }
        if (!si) {
            cerr_help(C, e->at,
                      arena_printf(C->a, "annotate the binding, e.g. `let x: %s[..] = %s { .. };`",
                                   written, written),
                      arena_printf(C->a, "can't infer type argument(s) for `%s`", written));
            for (int i = 0; i < fields->len; i++) check_expr(C, fields->at[i].value);
            return ty_error();
        }
    }
    if (!si) {
        // `Enum.Variant { .. }` is not a thing; variants use call syntax.
        if (map_has(&C->d->enums, name)) {
            cerr_help(C, e->at, "construct a variant with call syntax, e.g. `Event.Key(1)`",
                      arena_printf(C->a, "`%s` is an enum, not a struct", written));
        } else {
            cerr(C, e->at, arena_printf(C->a, "cannot find struct `%s`", written));
        }
        for (int i = 0; i < fields->len; i++) check_expr(C, fields->at[i].value);
        return ty_error();
    }

    for (int i = 0; i < fields->len; i++) {
        ty *vt = check_expr(C, fields->at[i].value);
        field_info *fi = NULL;
        for (int k = 0; k < si->fields.len; k++)
            if (strcmp(si->fields.at[k].name, fields->at[i].name) == 0) fi = &si->fields.at[k];
        if (fi) {
            vt = adopt_empty_literal(fields->at[i].value, vt, fi->t);
            if (!ty_assignable(vt, fi->t))
                cerr(C, fields->at[i].value->at,
                     arena_printf(C->a, "field `%s` expects `%s`, found `%s`",
                                  fields->at[i].name, ty_show(C->a, fi->t),
                                  ty_show(C->a, vt)));
        } else {
            cerr(C, fields->at[i].value->at,
                 arena_printf(C->a, "`%s` has no field `%s`", written, fields->at[i].name));
        }
    }

    const char *missing = "";
    int nmissing = 0;
    for (int k = 0; k < si->fields.len; k++) {
        bool given = false;
        for (int i = 0; i < fields->len; i++)
            if (strcmp(fields->at[i].name, si->fields.at[k].name) == 0) given = true;
        if (given) continue;
        missing = nmissing == 0 ? si->fields.at[k].name
                                : arena_printf(C->a, "%s, %s", missing, si->fields.at[k].name);
        nmissing++;
    }
    if (nmissing) {
        cerr_help(C, e->at, "every field must be given a value",
                  arena_printf(C->a, "missing field(s) in `%s`: %s", written, missing));
    }

    return ty_named(C->a, intern_z(C->in, name));
}

static ty *check_call(checker *C, expr *e) {
    expr *callee = e->as.call.callee;
    expr_vec *args = &e->as.call.args;

    // `print` takes any number of values and joins them with spaces, so it is
    // the one function exempt from the arity check.
    if (callee->kind == EX_PATH && callee->as.p.n == 1 &&
        strcmp(callee->as.p.seg[0], "print") == 0 && !lookup(C, "print")) {
        for (int i = 0; i < args->len; i++) check_expr(C, args->at[i]);
        return ty_void();
    }

    // Generic function, bare or qualified: `identity(x)`, `mod.identity(x)`.
    {
        const char *chain = static_path(C->a, callee);
        const char *q = chain ? qualify(C, chain) : NULL;
        fn_sig *sig = q ? (fn_sig *)map_get(&C->d->funcs, q) : NULL;
        if (sig && sig->generics.len > 0) return check_generic_call(C, e, sig, args);
    }

    // Generic enum variant, e.g. `Option.Some(x)` or `opt.Option.Some(x)`.
    if (callee->kind == EX_FIELD && !callee->as.field.optional &&
        static_path(C->a, callee->as.field.base)) {
        const char *head = static_path(C->a, callee->as.field.base);
        const char *q = qualify(C, head);
        if (q && !map_has(&C->d->enums, q) && map_has(&C->enum_tmpls, q)) {
            if (!e->hint || e->hint->kind != TK_NAMED) {
                cerr_help(C, e->at,
                          arena_printf(C->a, "annotate the binding, e.g. `let x: %s[..] = ..;`",
                                       head),
                          arena_printf(C->a, "can't infer type argument(s) for `%s`", head));
                for (int i = 0; i < args->len; i++) check_expr(C, args->at[i]);
                return ty_error();
            }
            const char *mangled = e->hint->name;
            enum_info *ei = (enum_info *)map_get(&C->d->enums, mangled);
            const char *vname = callee->as.field.name;
            variant_info *vi = NULL;
            if (ei)
                for (int i = 0; i < ei->variants.len; i++)
                    if (strcmp(ei->variants.at[i].name, vname) == 0) vi = &ei->variants.at[i];
            if (!vi) {
                cerr(C, callee->at,
                     arena_printf(C->a, "`%s` has no variant `%s`", mangled, vname));
                for (int i = 0; i < args->len; i++) check_expr(C, args->at[i]);
                return ty_error();
            }
            if (args->len != vi->payload.len)
                cerr(C, e->at,
                     arena_printf(C->a, "this variant takes %d argument(s) but %d were supplied",
                                  vi->payload.len, args->len));
            for (int i = 0; i < args->len; i++) {
                ty *got = check_expr(C, args->at[i]);
                if (i < vi->payload.len) {
                    got = adopt_empty_literal(args->at[i], got, vi->payload.at[i].t);
                    if (!ty_assignable(got, vi->payload.at[i].t))
                        cerr(C, args->at[i]->at,
                             arena_printf(C->a, "expected `%s`, found `%s`",
                                          ty_show(C->a, vi->payload.at[i].t), ty_show(C->a, got)));
                }
            }
            return ty_named(C->a, intern_z(C->in, mangled));
        }
    }

    // A bound-method call carries its receiver implicitly, so just type the
    // callee and use its function type.
    ty *cty = check_expr(C, callee);

    if (cty->kind == TK_ERROR) {
        for (int i = 0; i < args->len; i++) check_expr(C, args->at[i]);
        return ty_error();
    }
    if (cty->kind != TK_FUNC) {
        for (int i = 0; i < args->len; i++) check_expr(C, args->at[i]);
        cerr(C, e->at, arena_printf(C->a, "`%s` is not callable", ty_show(C->a, cty)));
        return ty_error();
    }

    if (args->len != cty->params.len) {
        cerr(C, e->at,
             arena_printf(C->a, "this call takes %d argument(s) but %d were supplied",
                          cty->params.len, args->len));
    }
    for (int i = 0; i < args->len; i++) {
        ty *got = check_expr(C, args->at[i]);
        if (i < cty->params.len)
            got = adopt_empty_literal(args->at[i], got, cty->params.at[i]);
        if (i < cty->params.len && !ty_assignable(got, cty->params.at[i])) {
            cerr(C, args->at[i]->at,
                 arena_printf(C->a, "expected `%s`, found `%s`",
                              ty_show(C->a, cty->params.at[i]), ty_show(C->a, got)));
        }
    }
    return cty->ret;
}

static ty *check_binary(checker *C, expr *e) {
    bin_op op = e->as.binary.op;
    expr *lhs = e->as.binary.lhs;
    expr *rhs = e->as.binary.rhs;
    ty *l = check_expr(C, lhs);
    ty *r = check_expr(C, rhs);

    switch (op) {
        case OP_AND:
        case OP_OR:
            want_bool(C, l, lhs->at);
            want_bool(C, r, rhs->at);
            return ty_bool();

        case OP_EQ:
        case OP_NE: {
            if (!ty_assignable(l, r) && !ty_assignable(r, l)) {
                span both;
                both.start = lhs->at.start < rhs->at.start ? lhs->at.start : rhs->at.start;
                both.end = lhs->at.end > rhs->at.end ? lhs->at.end : rhs->at.end;
                cerr(C, both, arena_printf(C->a, "cannot compare `%s` with `%s`",
                                           ty_show(C->a, l), ty_show(C->a, r)));
            }
            return ty_bool();
        }

        case OP_LT:
        case OP_LE:
        case OP_GT:
        case OP_GE: {
            if (!ty_is_numeric(l) && l->kind != TK_ERROR && l->kind != TK_STR) {
                cerr(C, lhs->at, arena_printf(C->a, "`%s` is not ordered", ty_show(C->a, l)));
            } else if (!ty_eq(l, r) && l->kind != TK_ERROR && r->kind != TK_ERROR) {
                cerr(C, rhs->at, arena_printf(C->a, "expected `%s`, found `%s`",
                                              ty_show(C->a, l), ty_show(C->a, r)));
            }
            return ty_bool();
        }

        default: {
            if (op == OP_ADD && l->kind == TK_STR) {
                if (!ty_assignable(r, ty_str()))
                    cerr(C, rhs->at, arena_printf(C->a, "expected `str`, found `%s`",
                                                  ty_show(C->a, r)));
                return ty_str();
            }
            if (l->kind == TK_ERROR || r->kind == TK_ERROR) return ty_error();
            if (!ty_is_numeric(l)) {
                cerr(C, lhs->at,
                     arena_printf(C->a, "`%s` needs a numeric type, found `%s`",
                                  bin_op_symbol(op), ty_show(C->a, l)));
                return ty_error();
            }
            if (!ty_eq(l, r)) {
                cerr_help(C, rhs->at,
                          "Solis does not convert between int and float implicitly",
                          arena_printf(C->a, "expected `%s`, found `%s`",
                                       ty_show(C->a, l), ty_show(C->a, r)));
                return ty_error();
            }
            return l;
        }
    }
}

static ty *check_expr(checker *C, expr *e) {
    switch (e->kind) {
        case EX_INT: return ty_int();
        case EX_FLOAT: return ty_float();
        case EX_BOOL: return ty_bool();
        case EX_NULL: return ty_null_lit();

        case EX_STR:
            for (int i = 0; i < e->as.str.len; i++)
                if (e->as.str.at[i].is_interp) check_expr(C, e->as.str.at[i].value);
            return ty_str();

        case EX_THIS:
            if (C->this_ty) return C->this_ty;
            cerr_help(C, e->at, "a `static func` has no receiver",
                      "`this` is only valid inside a method");
            return ty_error();

        case EX_PATH:
            return check_path(C, &e->as.p, e->at);

        case EX_UNARY: {
            ty *t = check_expr(C, e->as.unary.rhs);
            if (e->as.unary.op == UN_NOT) {
                want_bool(C, t, e->as.unary.rhs->at);
                return ty_bool();
            }
            if (!ty_is_numeric(t) && t->kind != TK_ERROR) {
                cerr(C, e->as.unary.rhs->at,
                     arena_printf(C->a, "cannot negate `%s`", ty_show(C->a, t)));
                return ty_error();
            }
            return t;
        }

        case EX_BINARY:
            return check_binary(C, e);

        case EX_ARRAY: {
            ty *elem = ty_error();
            for (int i = 0; i < e->as.array.len; i++) {
                ty *t = check_expr(C, e->as.array.at[i]);
                if (i == 0) elem = t;
                else if (!ty_assignable(t, elem)) {
                    cerr(C, e->as.array.at[i]->at,
                         arena_printf(C->a,
                                      "array elements must agree: expected `%s`, found `%s`",
                                      ty_show(C->a, elem), ty_show(C->a, t)));
                }
            }
            return ty_array(C->a, elem);
        }

        case EX_MAP: {
            ty *kt = ty_error();
            ty *vt = ty_error();
            for (int i = 0; i < e->as.map.len; i++) {
                ty *k2 = check_expr(C, e->as.map.at[i].k);
                ty *v2 = check_expr(C, e->as.map.at[i].v);
                if (i == 0) { kt = k2; vt = v2; continue; }
                if (!ty_assignable(k2, kt))
                    cerr(C, e->as.map.at[i].k->at,
                         arena_printf(C->a, "expected key `%s`, found `%s`",
                                      ty_show(C->a, kt), ty_show(C->a, k2)));
                if (!ty_assignable(v2, vt))
                    cerr(C, e->as.map.at[i].v->at,
                         arena_printf(C->a, "expected value `%s`, found `%s`",
                                      ty_show(C->a, vt), ty_show(C->a, v2)));
            }
            return ty_map(C->a, kt, vt);
        }

        case EX_INDEX: {
            ty *b = check_expr(C, e->as.index.base);
            ty *i = check_expr(C, e->as.index.index);
            if (b->kind == TK_ARRAY) {
                if (!ty_assignable(i, ty_int()))
                    cerr(C, e->as.index.index->at,
                         arena_printf(C->a, "array index must be `int`, found `%s`",
                                      ty_show(C->a, i)));
                return b->elem;
            }
            if (b->kind == TK_MAP) {
                if (!ty_assignable(i, b->key))
                    cerr(C, e->as.index.index->at,
                         arena_printf(C->a, "expected key `%s`, found `%s`",
                                      ty_show(C->a, b->key), ty_show(C->a, i)));
                return b->val;
            }
            if (b->kind == TK_ERROR) return ty_error();
            cerr(C, e->as.index.base->at,
                 arena_printf(C->a, "`%s` cannot be indexed", ty_show(C->a, b)));
            return ty_error();
        }

        case EX_SLICE: {
            ty *b = check_expr(C, e->as.slice.base);
            if (e->as.slice.lo) {
                ty *lo = check_expr(C, e->as.slice.lo);
                if (!ty_assignable(lo, ty_int()))
                    cerr(C, e->as.slice.lo->at,
                         arena_printf(C->a, "slice bound must be `int`, found `%s`",
                                      ty_show(C->a, lo)));
            }
            if (e->as.slice.hi) {
                ty *hi = check_expr(C, e->as.slice.hi);
                if (!ty_assignable(hi, ty_int()))
                    cerr(C, e->as.slice.hi->at,
                         arena_printf(C->a, "slice bound must be `int`, found `%s`",
                                      ty_show(C->a, hi)));
            }
            if (b->kind == TK_ERROR) return ty_error();
            if (b->kind != TK_ARRAY) {
                cerr(C, e->as.slice.base->at,
                     arena_printf(C->a, "`%s` cannot be sliced", ty_show(C->a, b)));
                return ty_error();
            }
            return b;
        }

        case EX_FIELD: {
            // `Enum.Variant` and `Type.staticMethod` look identical to field
            // access after parsing, so try the type interpretation first, but
            // only when no local shadows the name.
            const char *chain = static_path(C->a, e->as.field.base);
            if (chain) {
                const char *dot = strchr(chain, '.');
                const char *root = dot ? arena_strndup(C->a, chain, (size_t)(dot - chain))
                                       : chain;
                if (!lookup(C, root)) {
                    bool found = false;
                    ty *t = type_member(C, chain, e->as.field.name, e->at, &found);
                    if (found) return t;
                }
            }
            ty *b = check_expr(C, e->as.field.base);
            return field_or_method_ty(C, b, e->as.field.name, e->at);
        }

        case EX_STRUCT_LIT:
            return check_struct_lit(C, e);

        case EX_CALL:
            return check_call(C, e);
    }
    return ty_error();
}

static void enter_module(checker *C, module *m) {
    C->current_module = m->name;
    C->imports.len = 0;
    // The prelude is in every module's import list already. The loader put
    // it there, so its types are named `prelude.Thing` from anywhere with no
    // special case here. Only bare names need one, in prelude_name.
    for (int i = 0; i < m->imports.len; i++)
        vec_push(C->a, &C->imports, m->imports.at[i]);
}

decls *check_modules(arena *a, sl_interner *in, loaded *l, diag_vec *diags) {
    decls *d = NEW(a, decls);
    map_init(&d->structs, a);
    map_init(&d->enums, a);
    map_init(&d->interfaces, a);
    map_init(&d->funcs, a);
    map_init(&d->consts, a);
    map_init(&d->externs, a);

    checker C;
    memset(&C, 0, sizeof C);
    C.a = a;
    C.in = in;
    C.d = d;
    C.diags = diags;
    C.ret_ty = ty_void();
    C.current_module = "";
    C.prelude = l->prelude;
    map_init(&C.struct_tmpls, a);
    map_init(&C.enum_tmpls, a);
    map_init(&C.iface_tmpls, a);
    map_init(&C.conformance_checked, a);
    map_init(&C.cycles_checked, a);
    map_init(&C.instantiated, a);
    push_scope(&C);

    // Two passes over every module: all type names first, then all details.
    // Without this split, a module whose name sorts earlier could not refer to
    // a type in one that sorts later.
    for (int i = 0; i < l->modules.len; i++) {
        enter_module(&C, l->modules.at[i]);
        collect_names(&C, l->modules.at[i]->prog);
    }
    for (int i = 0; i < l->modules.len; i++) {
        enter_module(&C, l->modules.at[i]);
        collect_details(&C, l->modules.at[i]->prog);
    }
    for (int i = 0; i < l->modules.len; i++) {
        enter_module(&C, l->modules.at[i]);
        collect_consts(&C, l->modules.at[i]->prog);
    }

    check_conformance(&C);
    check_cycles(&C);

    for (int i = 0; i < l->modules.len; i++) {
        enter_module(&C, l->modules.at[i]);
        check_bodies(&C, l->modules.at[i]->prog);
    }

    // Catches body-only instantiations the first pass missed.
    check_conformance(&C);
    check_cycles(&C);

    return d;
}
