#include "gen.h"

#include "parse.h"

#include <stdio.h>

// Ordered: the index is the field address and the variant tag.
// Types ride along so field arithmetic still picks the typed opcode.
typedef struct {
    const char *name;
    ty         *t;
} slot;

typedef VEC(slot) slot_vec;

typedef struct {
    slot_vec fields;
} layout;

typedef struct {
    const char *name;
    slot_vec    payload;
} vlayout;

typedef VEC(vlayout) vlayout_vec;

typedef struct {
    vlayout_vec variants;
} elayout;

typedef struct {
    const char *name;
    ty         *t;
} local;

// `continue` has no single target: the condition in a `while`, the index
// bump in a desugared `for`. Neither position is known until the body is
// out, so both kinds of jump get collected and patched.
typedef struct {
    VEC(int) breaks;
    VEC(int) continues;
} loop_ctx;

typedef struct {
    arena       *a;
    sl_interner *in;
    decls       *d;
    loaded      *l;
    diag_vec    *diags;
    bc_program  *prog;

    map layouts;        // qualified name -> layout*
    map elayouts;       // qualified name -> elayout*
    map func_ids;       // qualified name -> id+1
    map globals;        // qualified name -> global*
    map sigs;           // qualified name -> fn_sig*

    VEC(local) locals;
    VEC(int)   scopes;
    uint16_t   nslots;
    uint32_t   temp;

    VEC(loop_ctx) loops;

    const char       *current;
    VEC(const char *) imports;

    // The receiver's type inside a method body.
    ty *this_ty;
} gen;

typedef struct {
    uint16_t index;
    ty      *t;
} global_info;

#define SLOT(x) ((void *)(uintptr_t)((x) + 1))
#define UNSLOT(v) ((int)((uintptr_t)(v) - 1))

static void gerr(gen *G, span at, const char *help, const char *msg) {
    diag_add(G->a, G->diags, at, msg);
    G->diags->at[G->diags->len - 1].help = help;
    G->diags->at[G->diags->len - 1].module = G->current;
}

static const char *qualify(gen *G, const char *name) {
    const char *dot = strchr(name, '.');
    if (dot) {
        size_t hn = (size_t)(dot - name);
        if (strlen(G->current) == hn && memcmp(G->current, name, hn) == 0) return name;
        for (int i = 0; i < G->imports.len; i++)
            if (strlen(G->imports.at[i]) == hn && memcmp(G->imports.at[i], name, hn) == 0)
                return name;
    }
    return arena_printf(G->a, "%s.%s", G->current, name);
}

// `name` as the prelude spells it, or NULL when there is no prelude to fall
// back to. Mirrors prelude_name in check.c: the checker decided a bare name
// resolves there, so codegen has to agree.
static const char *prelude_name(gen *G, const char *written) {
    if (!G->l->prelude || strchr(written, '.')) return NULL;
    if (strcmp(G->l->prelude, G->current) == 0) return NULL;
    return arena_printf(G->a, "%s.%s", G->l->prelude, written);
}

static void set_imports(gen *G, module *m) {
    G->current = m->name;
    G->imports.len = 0;
    for (int i = 0; i < m->imports.len; i++) vec_push(G->a, &G->imports, m->imports.at[i]);
}

static bool is_module(gen *G, const char *name) {
    for (int i = 0; i < G->l->modules.len; i++)
        if (strcmp(G->l->modules.at[i]->name, name) == 0) return true;
    return false;
}

static uint8_t declare(gen *G, const char *name, ty *t) {
    uint8_t s = (uint8_t)G->locals.len;
    local lv;
    lv.name = name;
    lv.t = t;
    vec_push(G->a, &G->locals, lv);
    if ((uint16_t)G->locals.len > G->nslots) G->nslots = (uint16_t)G->locals.len;
    return s;
}

// Leading space makes it unspellable in source, so it can't shadow.
static uint8_t fresh(gen *G, ty *t) {
    G->temp++;
    return declare(G, arena_printf(G->a, " tmp%u", G->temp), t);
}

static bool resolve_local(gen *G, const char *name, uint8_t *slot, ty **t) {
    for (int i = G->locals.len - 1; i >= 0; i--) {
        if (strcmp(G->locals.at[i].name, name) != 0) continue;
        *slot = (uint8_t)i;
        *t = G->locals.at[i].t;
        return true;
    }
    return false;
}

static void begin_scope(gen *G) { vec_push(G->a, &G->scopes, G->locals.len); }

static void end_scope(gen *G) {
    if (G->scopes.len) G->locals.len = G->scopes.at[--G->scopes.len];
}

static ty *gen_expr(gen *G, expr *x, emitter *e);
static void gen_stmt(gen *G, stmt *s, emitter *e);
static void gen_block(gen *G, block *b, emitter *e);
static void gen_arith(gen *G, bin_op op, ty *t, span at, emitter *e);

// Flatten a chain of identifier field accesses into a dotted string.
static const char *static_path(arena *a, expr *e) {
    if (e->kind == EX_PATH && e->as.p.n == 1) return e->as.p.seg[0];
    if (e->kind == EX_FIELD && !e->as.field.optional) {
        const char *base = static_path(a, e->as.field.base);
        if (!base) return NULL;
        return arena_printf(a, "%s.%s", base, e->as.field.name);
    }
    return NULL;
}

static bin_op bin_of(assign_op op) {
    switch (op) {
        case ASSIGN_ADD: return OP_ADD;
        case ASSIGN_SUB: return OP_SUB;
        case ASSIGN_MUL: return OP_MUL;
        case ASSIGN_DIV: return OP_DIV;
        case ASSIGN_REM: return OP_REM;
        case ASSIGN_SET: return OP_ADD;
    }
    return OP_ADD;
}

// Structs are value types; arrays and strings are not. We know the static
// type here, so COPY is emitted only where a struct could actually be.
static void maybe_copy(gen *G, ty *t, emitter *e) {
    bool could_be_struct;
    if (t->kind == TK_NAMED) could_be_struct = map_has(&G->layouts, t->name);
    // An unknown type is conservatively treated as copyable; COPY is a no-op
    // on anything that is not a struct.
    else if (t->kind == TK_ERROR) could_be_struct = true;
    else could_be_struct = false;

    if (could_be_struct) em_op(e, OP_COPY);
}

static layout *layout_of(gen *G, ty *t) {
    if (t->kind != TK_NAMED) return NULL;
    return (layout *)map_get(&G->layouts, t->name);
}

static bool field_index(gen *G, ty *t, const char *name, uint8_t *out) {
    layout *l = layout_of(G, t);
    if (!l) return false;
    for (int i = 0; i < l->fields.len; i++) {
        if (strcmp(l->fields.at[i].name, name) != 0) continue;
        *out = (uint8_t)i;
        return true;
    }
    return false;
}

static ty *field_ty(gen *G, ty *t, const char *name) {
    layout *l = layout_of(G, t);
    if (!l) return NULL;
    for (int i = 0; i < l->fields.len; i++)
        if (strcmp(l->fields.at[i].name, name) == 0) return l->fields.at[i].t;
    return NULL;
}

static ty *sig_ret(gen *G, const char *q) {
    fn_sig *sig = (fn_sig *)map_get(&G->sigs, q);
    return sig ? sig->ret : ty_error();
}

static ty *gen_call(gen *G, expr *x, emitter *e) {
    expr *callee = x->as.call.callee;
    expr_vec *args = &x->as.call.args;
    span at = x->at;

    if (callee->kind == EX_FIELD) {
        expr *base = callee->as.field.base;
        const char *name = callee->as.field.name;
        const char *chain = static_path(G->a, base);

        // Generic enum variant, e.g. `Option.Some(x)`.
        if (chain && x->hint && x->hint->kind == TK_NAMED) {
            elayout *el = (elayout *)map_get(&G->elayouts, x->hint->name);
            if (el) {
                for (int tag = 0; tag < el->variants.len; tag++) {
                    if (strcmp(el->variants.at[tag].name, name) != 0) continue;
                    for (int i = 0; i < args->len; i++) {
                        ty *t = gen_expr(G, args->at[i], e);
                        maybe_copy(G, t, e);
                    }
                    em_op(e, OP_NEW_VARIANT);
                    em_u16(e, (uint16_t)tag);
                    em_u8(e, (uint8_t)args->len);
                    return x->hint;
                }
            }
        }

        // `module.func(..)` is a plain call to a qualified name.
        if (chain && is_module(G, chain)) {
            const char *q = arena_printf(G->a, "%s.%s", chain, name);
            if (map_has(&G->d->externs, q)) {
                for (int i = 0; i < args->len; i++) gen_expr(G, args->at[i], e);
                // A stdlib primitive keeps its module prefix because the
                // runtime registers it that way; a host extern does not.
                const char *nn = stdlib_source(chain) ? q : name;
                em_op(e, OP_NATIVE);
                em_u16(e, bc_add_native(G->prog, intern_z(G->in, nn)));
                em_u8(e, (uint8_t)args->len);
                return sig_ret(G, q);
            }
            void *id = map_get(&G->func_ids, q);
            if (id) {
                for (int i = 0; i < args->len; i++) gen_expr(G, args->at[i], e);
                em_op(e, OP_CALL);
                em_u16(e, (uint16_t)UNSLOT(id));
                em_u8(e, (uint8_t)args->len);
                return sig_ret(G, q);
            }
        }

        bool is_type_path = false;
        if (chain) {
            const char *q = qualify(G, chain);
            is_type_path = map_has(&G->elayouts, q) || map_has(&G->layouts, q);
        }

        if (!is_type_path) {
            // Real methods win over the len/push builtins below.
            ty *bt = gen_expr(G, base, e);
            if (bt->kind == TK_NAMED) {
                const char *q = arena_printf(G->a, "%s.%s", bt->name, name);
                void *id = map_get(&G->func_ids, q);
                if (id) {
                    for (int i = 0; i < args->len; i++) gen_expr(G, args->at[i], e);
                    em_op(e, OP_CALL);
                    em_u16(e, (uint16_t)UNSLOT(id));
                    em_u8(e, (uint8_t)(args->len + 1));
                    return sig_ret(G, q);
                }
            }

            // Builtins that act on an array value.
            if (bt->kind == TK_ARRAY &&
                (strcmp(name, "len") == 0 || strcmp(name, "push") == 0)) {
                for (int i = 0; i < args->len; i++) {
                    ty *t = gen_expr(G, args->at[i], e);
                    if (strcmp(name, "push") == 0) maybe_copy(G, t, e);
                }
                em_op(e, strcmp(name, "len") == 0 ? OP_ALEN : OP_APUSH);
                return strcmp(name, "len") == 0 ? ty_int() : ty_void();
            }

            gerr(G, at, NULL, arena_printf(G->a, "unknown method `%s`", name));
            return ty_error();
        }

        // `Type.static(..)` or `Enum.Variant(..)`
        const char *q = qualify(G, chain);
        elayout *el = (elayout *)map_get(&G->elayouts, q);
        if (el) {
            for (int tag = 0; tag < el->variants.len; tag++) {
                if (strcmp(el->variants.at[tag].name, name) != 0) continue;
                for (int i = 0; i < args->len; i++) {
                    ty *t = gen_expr(G, args->at[i], e);
                    maybe_copy(G, t, e);
                }
                em_op(e, OP_NEW_VARIANT);
                em_u16(e, (uint16_t)tag);
                em_u8(e, (uint8_t)args->len);
                return ty_named(G->a, intern_z(G->in, q));
            }
        }
        const char *fq = arena_printf(G->a, "%s.%s", q, name);
        void *id = map_get(&G->func_ids, fq);
        if (id) {
            for (int i = 0; i < args->len; i++) gen_expr(G, args->at[i], e);
            em_op(e, OP_CALL);
            em_u16(e, (uint16_t)UNSLOT(id));
            em_u8(e, (uint8_t)args->len);
            return sig_ret(G, fq);
        }
        gerr(G, at, NULL, arena_printf(G->a, "unknown `%s.%s`", chain, name));
        return ty_error();
    }

    if (callee->kind != EX_PATH || callee->as.p.n != 1) {
        gerr(G, at, NULL, "unsupported call");
        return ty_error();
    }
    const char *name = callee->as.p.seg[0];
    const char *q = qualify(G, name);
    bool has_local = q && (map_has(&G->d->externs, q) || map_has(&G->func_ids, q));

    // Global builtins. `len` and `push` are instructions; the rest are
    // natives, which the runtime pre-registers for pure maths and leaves to
    // the host for anything that touches the world. A module's own function
    // of the same name wins (e.g. strings.sl's own `indexOf`).
    if (!has_local && (strcmp(name, "len") == 0 || strcmp(name, "push") == 0)) {
        for (int i = 0; i < args->len; i++) {
            ty *t = gen_expr(G, args->at[i], e);
            if (strcmp(name, "push") == 0) maybe_copy(G, t, e);
        }
        em_op(e, strcmp(name, "len") == 0 ? OP_ALEN : OP_APUSH);
        return strcmp(name, "len") == 0 ? ty_int() : ty_void();
    }
    if (!has_local &&
        (strcmp(name, "print") == 0 || strcmp(name, "sqrt") == 0 ||
         strcmp(name, "abs") == 0 || strcmp(name, "pop") == 0 ||
         strcmp(name, "indexOf") == 0 || strcmp(name, "contains") == 0 ||
         strcmp(name, "reverse") == 0 || strcmp(name, "removeAt") == 0 ||
         strcmp(name, "insertAt") == 0 || strcmp(name, "sort") == 0 ||
         strcmp(name, "removeKey") == 0 || strcmp(name, "keys") == 0 ||
         strcmp(name, "values") == 0)) {
        ty *arg_ty = ty_error();
        for (int i = 0; i < args->len; i++) {
            ty *t = gen_expr(G, args->at[i], e);
            if (i == 0) arg_ty = t;
        }
        em_op(e, OP_NATIVE);
        em_u16(e, bc_add_native(G->prog, intern_z(G->in, name)));
        em_u8(e, (uint8_t)args->len);
        if (strcmp(name, "print") == 0 || strcmp(name, "reverse") == 0 ||
            strcmp(name, "insertAt") == 0 || strcmp(name, "sort") == 0) return ty_void();
        if (strcmp(name, "sqrt") == 0) return ty_float();
        if (strcmp(name, "indexOf") == 0) return ty_int();
        if (strcmp(name, "contains") == 0) return ty_bool();
        if (strcmp(name, "pop") == 0 || strcmp(name, "removeAt") == 0)
            return arg_ty->kind == TK_ARRAY ? arg_ty->elem : ty_error();
        if (strcmp(name, "removeKey") == 0)
            return arg_ty->kind == TK_MAP ? arg_ty->val : ty_error();
        if (strcmp(name, "keys") == 0)
            return ty_array(G->a, arg_ty->kind == TK_MAP ? arg_ty->key : ty_error());
        if (strcmp(name, "values") == 0)
            return ty_array(G->a, arg_ty->kind == TK_MAP ? arg_ty->val : ty_error());
        return arg_ty;      // `abs` keeps its argument's type
    }

    for (int i = 0; i < args->len; i++) gen_expr(G, args->at[i], e);

    // Not declared here, but the prelude has it: the checker already resolved
    // the call that way, so use the same name.
    if (!map_has(&G->d->externs, q) && !map_has(&G->func_ids, q)) {
        const char *pq = prelude_name(G, name);
        if (pq && (map_has(&G->d->externs, pq) || map_has(&G->func_ids, pq))) q = pq;
    }

    if (map_has(&G->d->externs, q)) {
        const char *dot = strchr(q, '.');
        const char *home = dot ? arena_strndup(G->a, q, (size_t)(dot - q)) : NULL;
        const char *nn = (home && stdlib_source(home)) ? q : name;
        em_op(e, OP_NATIVE);
        em_u16(e, bc_add_native(G->prog, intern_z(G->in, nn)));
        em_u8(e, (uint8_t)args->len);
        return sig_ret(G, q);
    }
    void *id = map_get(&G->func_ids, q);
    if (id) {
        em_op(e, OP_CALL);
        em_u16(e, (uint16_t)UNSLOT(id));
        em_u8(e, (uint8_t)args->len);
        return sig_ret(G, q);
    }
    gerr(G, at, NULL, arena_printf(G->a, "`%s` cannot be called", name));
    return ty_error();
}

static void gen_arith(gen *G, bin_op op, ty *t, span at, emitter *e) {
    if (t->kind == TK_STR && op == OP_ADD) {
        em_op(e, OP_CONCAT);
        return;
    }
    bool f = t->kind == TK_FLOAT;
    opcode o;
    switch (op) {
        case OP_ADD: o = f ? OP_ADD_F : OP_ADD_I; break;
        case OP_SUB: o = f ? OP_SUB_F : OP_SUB_I; break;
        case OP_MUL: o = f ? OP_MUL_F : OP_MUL_I; break;
        case OP_DIV: o = f ? OP_DIV_F : OP_DIV_I; break;
        case OP_REM: o = f ? OP_REM_F : OP_REM_I; break;
        default:
            gerr(G, at, NULL, "unsupported operator");
            o = OP_ADD_I;
            break;
    }
    em_op(e, o);
}

static ty *gen_binary(gen *G, expr *x, emitter *e) {
    bin_op op = x->as.binary.op;
    expr *lhs = x->as.binary.lhs;
    expr *rhs = x->as.binary.rhs;

    // Short-circuiting, which needs jumps rather than an opcode.
    if (op == OP_AND || op == OP_OR) {
        gen_expr(G, lhs, e);
        if (op == OP_AND) {
            int skip = em_jump(e, OP_JUMP_IF_FALSE);
            gen_expr(G, rhs, e);
            int end = em_jump(e, OP_JUMP);
            em_patch(e, skip);
            em_op(e, OP_FALSE);
            em_patch(e, end);
        } else {
            em_op(e, OP_NOT);
            int skip = em_jump(e, OP_JUMP_IF_FALSE);
            gen_expr(G, rhs, e);
            int end = em_jump(e, OP_JUMP);
            em_patch(e, skip);
            em_op(e, OP_TRUE);
            em_patch(e, end);
        }
        return ty_bool();
    }

    ty *lt = gen_expr(G, lhs, e);
    ty *rt = gen_expr(G, rhs, e);
    ty *t = lt->kind == TK_ERROR ? rt : lt;

    if (op == OP_ADD && t->kind == TK_STR) {
        em_op(e, OP_CONCAT);
        return ty_str();
    }
    if (op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_DIV || op == OP_REM) {
        gen_arith(G, op, t, x->at, e);
        return t;
    }

    // A bare type param: the static type can't pick a typed opcode here, so
    // fall back to runtime-tag equality instead of silently defaulting wrong.
    if (t->kind == TK_PARAM && (op == OP_EQ || op == OP_NE)) {
        em_op(e, OP_NATIVE);
        em_u16(e, bc_add_native(G->prog, intern_z(G->in, " eq")));
        em_u8(e, 2);
        if (op == OP_NE) em_op(e, OP_NOT);
        return ty_bool();
    }

    opcode o = OP_EQ_I;
    bool fl = t->kind == TK_FLOAT;
    bool st = t->kind == TK_STR;
    bool bl = t->kind == TK_BOOL;
    switch (op) {
        case OP_EQ: o = fl ? OP_EQ_F : st ? OP_EQ_S : bl ? OP_EQ_B : OP_EQ_I; break;
        case OP_NE: o = fl ? OP_NE_F : st ? OP_NE_S : bl ? OP_NE_B : OP_NE_I; break;
        case OP_LT: o = fl ? OP_LT_F : OP_LT_I; break;
        case OP_LE: o = fl ? OP_LE_F : OP_LE_I; break;
        case OP_GT: o = fl ? OP_GT_F : OP_GT_I; break;
        case OP_GE: o = fl ? OP_GE_F : OP_GE_I; break;
        default:
            gerr(G, x->at, NULL, "unsupported comparison");
            break;
    }
    em_op(e, o);
    return ty_bool();
}

static ty *gen_expr(gen *G, expr *x, emitter *e) {
    switch (x->kind) {
        case EX_INT:
            em_const_int(e, x->as.i, G->prog);
            return ty_int();

        case EX_FLOAT:
            em_op(e, OP_CONST);
            em_u16(e, bc_add_float(G->prog, x->as.f));
            return ty_float();

        case EX_BOOL:
            em_op(e, x->as.b ? OP_TRUE : OP_FALSE);
            return ty_bool();

        case EX_STR: {
            // Interpolation lowers to a chain of concatenations.
            str_seg_vec *segs = &x->as.str;
            if (segs->len == 0) {
                em_op(e, OP_CONST_STR);
                em_u16(e, bc_add_str(G->prog, "", 0));
                return ty_str();
            }
            for (int i = 0; i < segs->len; i++) {
                if (!segs->at[i].is_interp) {
                    em_op(e, OP_CONST_STR);
                    em_u16(e, bc_add_str(G->prog, segs->at[i].text,
                                         strlen(segs->at[i].text)));
                } else {
                    ty *t = gen_expr(G, segs->at[i].value, e);
                    // Primitives get the typed conversion; anything with a
                    // runtime shape gets the generic one.
                    if (t->kind == TK_STR) {
                        // already a string
                    }
                    else if (t->kind == TK_INT) em_op(e, OP_STR_I);
                    else if (t->kind == TK_FLOAT) em_op(e, OP_STR_F);
                    else if (t->kind == TK_BOOL) em_op(e, OP_STR_B);
                    else em_op(e, OP_STR_OBJ);
                }
                if (i > 0) em_op(e, OP_CONCAT);
            }
            return ty_str();
        }

        case EX_THIS:
            if (!G->this_ty) {
                gerr(G, x->at, NULL, "`this` outside a method");
                return ty_error();
            }
            em_op(e, OP_LOAD);
            em_u8(e, 0);
            return G->this_ty;

        case EX_PATH: {
            if (x->as.p.n != 1) {
                gerr(G, x->at, NULL, "unsupported path expression");
                return ty_error();
            }
            const char *n = x->as.p.seg[0];
            uint8_t slot;
            ty *t;
            if (resolve_local(G, n, &slot, &t)) {
                em_op(e, OP_LOAD);
                em_u8(e, slot);
                return t;
            }
            global_info *gi = (global_info *)map_get(&G->globals, qualify(G, n));
            if (!gi) {
                const char *pq = prelude_name(G, n);
                if (pq) gi = (global_info *)map_get(&G->globals, pq);
            }
            if (gi) {
                em_op(e, OP_LOADG);
                em_u16(e, gi->index);
                return gi->t;
            }
            gerr(G, x->at, NULL, arena_printf(G->a, "`%s` is not in scope", n));
            return ty_error();
        }

        case EX_ARRAY: {
            ty *elem = ty_error();
            for (int i = 0; i < x->as.array.len; i++) {
                ty *t = gen_expr(G, x->as.array.at[i], e);
                maybe_copy(G, t, e);
                if (i == 0) elem = t;
            }
            em_op(e, OP_NEW_ARRAY);
            em_u16(e, (uint16_t)x->as.array.len);
            // `[]` has no element to take a type from, so the checker left
            // the type it was assigned to on the node.
            if (x->as.array.len == 0 && x->hint) return x->hint;
            return ty_array(G->a, elem);
        }

        case EX_INDEX: {
            ty *bt = gen_expr(G, x->as.index.base, e);
            gen_expr(G, x->as.index.index, e);
            if (bt->kind == TK_MAP) {
                em_op(e, OP_MGET);
                return bt->val;
            }
            em_op(e, OP_AGET);
            return bt->kind == TK_ARRAY ? bt->elem : ty_error();
        }

        case EX_SLICE: {
            ty *bt = gen_expr(G, x->as.slice.base, e);
            uint8_t slot = fresh(G, bt);
            em_op(e, OP_STORE);
            em_u8(e, slot);

            em_op(e, OP_LOAD);
            em_u8(e, slot);
            if (x->as.slice.lo) {
                gen_expr(G, x->as.slice.lo, e);
            } else {
                em_op(e, OP_CONST_I8);
                em_u8(e, 0);
            }
            if (x->as.slice.hi) {
                gen_expr(G, x->as.slice.hi, e);
            } else {
                em_op(e, OP_LOAD);
                em_u8(e, slot);
                em_op(e, OP_ALEN);
            }
            em_op(e, OP_SLICE);
            return bt;
        }

        case EX_STRUCT_LIT: {
            const char *name = x->hint && x->hint->kind == TK_NAMED
                ? x->hint->name
                : qualify(G, path_text(G->a, &x->as.struct_lit.p));
            layout *l = (layout *)map_get(&G->layouts, name);
            if (!l) {
                gerr(G, x->at, NULL,
                     arena_printf(G->a, "unknown struct `%s`",
                                  path_text(G->a, &x->as.struct_lit.p)));
                return ty_error();
            }
            // Emit in declaration order regardless of how they were written,
            // because the runtime addresses fields by index.
            for (int i = 0; i < l->fields.len; i++) {
                const char *fname = l->fields.at[i].name;
                expr *v = NULL;
                for (int k = 0; k < x->as.struct_lit.fields.len; k++)
                    if (strcmp(x->as.struct_lit.fields.at[k].name, fname) == 0)
                        v = x->as.struct_lit.fields.at[k].value;
                if (v) {
                    ty *t = gen_expr(G, v, e);
                    maybe_copy(G, t, e);
                } else {
                    gerr(G, x->at, NULL,
                         arena_printf(G->a, "missing field `%s`", fname));
                    em_op(e, OP_FALSE);
                }
            }
            em_op(e, OP_NEW_STRUCT);
            em_u16(e, (uint16_t)l->fields.len);
            return ty_named(G->a, intern_z(G->in, name));
        }

        case EX_FIELD: {
            const char *chain = static_path(G->a, x->as.field.base);

            // `module.CONST`
            if (chain && is_module(G, chain)) {
                global_info *gi = (global_info *)map_get(
                    &G->globals, arena_printf(G->a, "%s.%s", chain, x->as.field.name));
                if (gi) {
                    em_op(e, OP_LOADG);
                    em_u16(e, gi->index);
                    return gi->t;
                }
            }

            // `Enum.Variant` with no payload.
            if (chain) {
                const char *q = qualify(G, chain);
                elayout *el = (elayout *)map_get(&G->elayouts, q);
                if (el) {
                    for (int tag = 0; tag < el->variants.len; tag++) {
                        if (strcmp(el->variants.at[tag].name, x->as.field.name) != 0) continue;
                        em_op(e, OP_NEW_VARIANT);
                        em_u16(e, (uint16_t)tag);
                        em_u8(e, 0);
                        return ty_named(G->a, intern_z(G->in, q));
                    }
                }
            }

            ty *bt = gen_expr(G, x->as.field.base, e);
            uint8_t idx;
            if (!field_index(G, bt, x->as.field.name, &idx)) {
                gerr(G, x->at, NULL,
                     arena_printf(G->a, "no field `%s`", x->as.field.name));
                return ty_error();
            }
            em_op(e, OP_FGET);
            em_u8(e, idx);
            ty *ft = field_ty(G, bt, x->as.field.name);
            return ft ? ft : ty_error();
        }

        case EX_UNARY: {
            ty *t = gen_expr(G, x->as.unary.rhs, e);
            if (x->as.unary.op == UN_NOT) {
                em_op(e, OP_NOT);
                return ty_bool();
            }
            em_op(e, t->kind == TK_FLOAT ? OP_NEG_F : OP_NEG_I);
            return t;
        }

        case EX_BINARY:
            return gen_binary(G, x, e);

        case EX_CALL:
            return gen_call(G, x, e);

        case EX_NULL:
            em_op(e, OP_NULL);
            return ty_null_lit();

        case EX_MAP: {
            ty *kt = ty_error(), *vt = ty_error();
            for (int i = 0; i < x->as.map.len; i++) {
                ty *k = gen_expr(G, x->as.map.at[i].k, e);
                maybe_copy(G, k, e);
                ty *v = gen_expr(G, x->as.map.at[i].v, e);
                maybe_copy(G, v, e);
                if (i == 0) { kt = k; vt = v; }
            }
            em_op(e, OP_NEW_MAP);
            em_u16(e, (uint16_t)x->as.map.len);
            if (x->as.map.len == 0 && x->hint) return x->hint;
            return ty_map(G->a, kt, vt);
        }
    }
    return ty_error();
}

static void gen_assign(gen *G, stmt *s, emitter *e) {
    expr *place = s->as.assign.place;
    assign_op op = s->as.assign.op;
    expr *value = s->as.assign.value;
    span at = s->at;

    if (place->kind == EX_PATH && place->as.p.n == 1) {
        const char *n = place->as.p.seg[0];
        uint8_t slot;
        ty *t;
        if (resolve_local(G, n, &slot, &t)) {
            if (op == ASSIGN_SET && value->kind == EX_PATH && value->as.p.n == 1 &&
                (t->kind == TK_INT || t->kind == TK_FLOAT || t->kind == TK_BOOL)) {
                uint8_t src; ty *st;
                if (resolve_local(G, value->as.p.seg[0], &src, &st) && src != slot) {
                    em_op(e, OP_MOVE);
                    em_u8(e, slot); em_u8(e, src);
                    return;
                }
            }
            if ((op == ASSIGN_ADD || op == ASSIGN_SUB) && value->kind == EX_INT &&
                t->kind == TK_INT) {
                int64_t imm = op == ASSIGN_SUB ? -value->as.i : value->as.i;
                if (imm >= -128 && imm <= 127) {
                    em_op(e, OP_INCR_I);
                    em_u8(e, slot);
                    em_u8(e, (uint8_t)(int8_t)imm);
                    return;
                }
            }
            if (op == ASSIGN_SET) {
                ty *vt = gen_expr(G, value, e);
                maybe_copy(G, vt, e);
            } else {
                em_op(e, OP_LOAD);
                em_u8(e, slot);
                gen_expr(G, value, e);
                gen_arith(G, bin_of(op), t, at, e);
            }
            em_op(e, OP_STORE);
            em_u8(e, slot);
            return;
        }
        // A module-level binding.
        global_info *gi = (global_info *)map_get(&G->globals, qualify(G, n));
        if (gi) {
            if (op == ASSIGN_SET) {
                ty *vt = gen_expr(G, value, e);
                maybe_copy(G, vt, e);
            } else {
                em_op(e, OP_LOADG);
                em_u16(e, gi->index);
                gen_expr(G, value, e);
                gen_arith(G, bin_of(op), gi->t, at, e);
            }
            em_op(e, OP_STOREG);
            em_u16(e, gi->index);
            return;
        }
        gerr(G, place->at, NULL, arena_printf(G->a, "`%s` is not in scope", n));
        return;
    }

    if (place->kind == EX_FIELD) {
        ty *bt = gen_expr(G, place->as.field.base, e);
        uint8_t idx;
        if (!field_index(G, bt, place->as.field.name, &idx)) {
            gerr(G, place->at, NULL,
                 arena_printf(G->a, "no field `%s`", place->as.field.name));
            return;
        }
        if (op == ASSIGN_SET) {
            ty *vt = gen_expr(G, value, e);
            maybe_copy(G, vt, e);
        } else {
            // Re-read the field to combine with it.
            ty *ft = field_ty(G, bt, place->as.field.name);
            em_op(e, OP_DUP);
            em_op(e, OP_FGET);
            em_u8(e, idx);
            gen_expr(G, value, e);
            gen_arith(G, bin_of(op), ft ? ft : ty_error(), at, e);
        }
        em_op(e, OP_FSET);
        em_u8(e, idx);
        return;
    }

    if (place->kind == EX_INDEX) {
        ty *bt = gen_expr(G, place->as.index.base, e);
        gen_expr(G, place->as.index.index, e);
        bool is_map = bt->kind == TK_MAP;
        if (op == ASSIGN_SET) {
            ty *vt = gen_expr(G, value, e);
            maybe_copy(G, vt, e);
        } else {
            ty *elem = is_map ? bt->val : bt->kind == TK_ARRAY ? bt->elem : ty_error();
            // Keep the array/map and index/key for the store, and take a
            // second copy of each to do the read.
            em_op(e, OP_DUP2);
            em_op(e, is_map ? OP_MGET : OP_AGET);
            gen_expr(G, value, e);
            gen_arith(G, bin_of(op), elem, at, e);
        }
        em_op(e, is_map ? OP_MSET : OP_ASET);
        return;
    }

    gerr(G, place->at, NULL, "unsupported assignment target");
}

// `switch` lowers to a chain of tag comparisons; the VM has no such op.
static void gen_switch(gen *G, stmt *s, emitter *e) {
    begin_scope(G);
    ty *st = gen_expr(G, s->as.switch_.scrutinee, e);
    uint8_t subject = fresh(G, st);
    em_op(e, OP_STORE);
    em_u8(e, subject);

    elayout *el = st->kind == TK_NAMED ? (elayout *)map_get(&G->elayouts, st->name) : NULL;
    if (!el) {
        gerr(G, s->at, NULL, "the C backend can only switch on an enum");
        end_scope(G);
        return;
    }

    VEC(int) ends;
    memset(&ends, 0, sizeof ends);

    case_vec *cases = &s->as.switch_.cases;
    for (int ci = 0; ci < cases->len; ci++) {
        switch_case *c = &cases->at[ci];

        // Several patterns, any of which enters the body: each test jumps
        // to the body (hits) or falls through to the next, and only the last
        // jumps onward to the next case (misses).
        VEC(int) hits;
        VEC(int) misses;
        memset(&hits, 0, sizeof hits);
        memset(&misses, 0, sizeof misses);

        typedef struct { const char *name; int idx; ty *t; } bound_entry;
        VEC(bound_entry) bound;
        memset(&bound, 0, sizeof bound);

        bool single = c->patterns.len == 1;

        for (int k = 0; k < c->patterns.len; k++) {
            pattern *pat = &c->patterns.at[k];
            bool last = k + 1 == c->patterns.len;

            if (pat->kind == PAT_WILDCARD) continue;   // always matches

            if (pat->kind != PAT_VARIANT) {
                gerr(G, pat->at, NULL, "the C backend only matches enum variants");
                continue;
            }

            const char *vname = pat->as.variant.p.seg[pat->as.variant.p.n - 1];
            int tag = -1;
            for (int t = 0; t < el->variants.len; t++)
                if (strcmp(el->variants.at[t].name, vname) == 0) tag = t;
            if (tag < 0) {
                gerr(G, pat->at, NULL, arena_printf(G->a, "no variant `%s`", vname));
                continue;
            }

            em_op(e, OP_LOAD);
            em_u8(e, subject);
            em_op(e, OP_VTAG);
            em_const_int(e, tag, G->prog);
            em_op(e, OP_EQ_I);
            if (last) {
                // False means no pattern matched: try the next case.
                vec_push(G->a, &misses, em_jump(e, OP_JUMP_IF_FALSE));
            } else {
                // There is no JUMP_IF_TRUE, so invert and reuse
                // JUMP_IF_FALSE: it now fires when the tag matched.
                em_op(e, OP_NOT);
                vec_push(G->a, &hits, em_jump(e, OP_JUMP_IF_FALSE));
            }

            binding_vec *bs = &pat->as.variant.bindings;
            if (bs->len == 0) continue;
            if (!single) {
                gerr(G, pat->at, "give each variant its own case",
                     "a case that lists several patterns cannot bind a payload");
                continue;
            }
            for (int bi = 0; bi < bs->len; bi++) {
                bound_entry be;
                be.name = bs->at[bi].name;
                be.idx = bi;
                be.t = bi < el->variants.at[tag].payload.len
                           ? el->variants.at[tag].payload.at[bi].t
                           : ty_error();
                vec_push(G->a, &bound, be);
            }
        }

        for (int h = 0; h < hits.len; h++) em_patch(e, hits.at[h]);

        begin_scope(G);
        // Bind the payload fields this case names.
        for (int b = 0; b < bound.len; b++) {
            em_op(e, OP_LOAD);
            em_u8(e, subject);
            em_op(e, OP_VGET);
            em_u8(e, (uint8_t)bound.at[b].idx);
            uint8_t slot = declare(G, bound.at[b].name, bound.at[b].t);
            em_op(e, OP_STORE);
            em_u8(e, slot);
        }
        for (int k = 0; k < c->body.len; k++) gen_stmt(G, c->body.at[k], e);
        end_scope(G);

        vec_push(G->a, &ends, em_jump(e, OP_JUMP));
        for (int m = 0; m < misses.len; m++) em_patch(e, misses.at[m]);
    }

    if (s->as.switch_.dflt) gen_block(G, s->as.switch_.dflt, e);
    for (int j = 0; j < ends.len; j++) em_patch(e, ends.at[j]);
    end_scope(G);
}

static void gen_block(gen *G, block *b, emitter *e) {
    begin_scope(G);
    for (int i = 0; i < b->stmts.len; i++) gen_stmt(G, b->stmts.at[i], e);
    end_scope(G);
}

static void gen_stmt(gen *G, stmt *s, emitter *e) {
    switch (s->kind) {
        case ST_LET: {
            expr *val = s->as.let_.value;
            if (val->kind == EX_BINARY && val->as.binary.op == OP_ADD) {
                expr *lhs = val->as.binary.lhs, *rhs = val->as.binary.rhs;
                uint8_t s1, s2; ty *t1, *t2;
                if (lhs->kind == EX_PATH && lhs->as.p.n == 1 &&
                    rhs->kind == EX_PATH && rhs->as.p.n == 1 &&
                    resolve_local(G, lhs->as.p.seg[0], &s1, &t1) &&
                    resolve_local(G, rhs->as.p.seg[0], &s2, &t2) &&
                    t1->kind == TK_INT && t2->kind == TK_INT) {
                    uint8_t slot = declare(G, s->as.let_.name, t1);
                    em_op(e, OP_ADD_RR_I);
                    em_u8(e, slot); em_u8(e, s1); em_u8(e, s2);
                    return;
                }
            }
            ty *t = gen_expr(G, val, e);
            maybe_copy(G, t, e);
            uint8_t slot = declare(G, s->as.let_.name, t);
            em_op(e, OP_STORE);
            em_u8(e, slot);
            return;
        }

        case ST_ASSIGN:
            gen_assign(G, s, e);
            return;

        case ST_IF: {
            gen_expr(G, s->as.if_.cond, e);
            int else_jump = em_jump(e, OP_JUMP_IF_FALSE);
            gen_block(G, s->as.if_.then, e);
            if (s->as.if_.els) {
                int end = em_jump(e, OP_JUMP);
                em_patch(e, else_jump);
                gen_stmt(G, s->as.if_.els, e);
                em_patch(e, end);
            } else {
                em_patch(e, else_jump);
            }
            return;
        }

        case ST_WHILE: {
            int top = em_here(e);
            gen_expr(G, s->as.while_.cond, e);
            int exit = em_jump(e, OP_JUMP_IF_FALSE);

            loop_ctx ctx;
            memset(&ctx, 0, sizeof ctx);
            vec_push(G->a, &G->loops, ctx);
            gen_block(G, s->as.while_.body, e);
            loop_ctx done = G->loops.at[--G->loops.len];

            for (int i = 0; i < done.continues.len; i++)
                em_patch_to(e, done.continues.at[i], top);
            em_jump_back(e, OP_JUMP, top);
            em_patch(e, exit);
            for (int i = 0; i < done.breaks.len; i++) em_patch(e, done.breaks.at[i]);
            return;
        }

        // `for x in xs { .. }` becomes an index-based loop, so the VM needs no
        // iterator machinery.
        case ST_FOR: {
            begin_scope(G);
            ty *seq_t = gen_expr(G, s->as.for_.iter, e);
            ty *elem_t = seq_t->kind == TK_ARRAY ? seq_t->elem : ty_error();

            uint8_t seq = fresh(G, seq_t);
            em_op(e, OP_STORE);
            em_u8(e, seq);

            uint8_t idx = fresh(G, ty_int());
            em_const_int(e, 0, G->prog);
            em_op(e, OP_STORE);
            em_u8(e, idx);

            int top = em_here(e);
            em_op(e, OP_LOAD); em_u8(e, idx);
            em_op(e, OP_LOAD); em_u8(e, seq);
            em_op(e, OP_ALEN);
            em_op(e, OP_LT_I);
            int exit = em_jump(e, OP_JUMP_IF_FALSE);

            em_op(e, OP_LOAD); em_u8(e, seq);
            em_op(e, OP_LOAD); em_u8(e, idx);
            em_op(e, OP_AGET);
            uint8_t item = declare(G, s->as.for_.var, elem_t);
            em_op(e, OP_STORE);
            em_u8(e, item);

            loop_ctx ctx;
            memset(&ctx, 0, sizeof ctx);
            vec_push(G->a, &G->loops, ctx);
            for (int i = 0; i < s->as.for_.body->stmts.len; i++)
                gen_stmt(G, s->as.for_.body->stmts.at[i], e);
            loop_ctx done = G->loops.at[--G->loops.len];

            // `continue` lands on the index bump, not on the condition, or the
            // loop would never advance.
            int bump = em_here(e);
            for (int i = 0; i < done.continues.len; i++)
                em_patch_to(e, done.continues.at[i], bump);

            em_op(e, OP_LOAD); em_u8(e, idx);
            em_const_int(e, 1, G->prog);
            em_op(e, OP_ADD_I);
            em_op(e, OP_STORE); em_u8(e, idx);

            em_jump_back(e, OP_JUMP, top);
            em_patch(e, exit);
            for (int i = 0; i < done.breaks.len; i++) em_patch(e, done.breaks.at[i]);
            end_scope(G);
            return;
        }

        case ST_SWITCH:
            gen_switch(G, s, e);
            return;

        case ST_RETURN:
            if (s->as.value) {
                gen_expr(G, s->as.value, e);
                em_op(e, OP_RET);
            } else {
                em_op(e, OP_RET_VOID);
            }
            return;

        case ST_BLOCK:
        case ST_ARENA:
            gen_block(G, s->as.blk, e);
            return;

        case ST_EXPR: {
            ty *t = gen_expr(G, s->as.value, e);
            if (t->kind != TK_VOID) em_op(e, OP_POP);
            return;
        }

        case ST_BREAK:
        case ST_CONTINUE: {
            if (G->loops.len == 0) {
                gerr(G, s->at, NULL, "`break` and `continue` only work inside a loop");
                return;
            }
            int j = em_jump(e, OP_JUMP);
            loop_ctx *ctx = &G->loops.at[G->loops.len - 1];
            if (s->kind == ST_BREAK) vec_push(G->a, &ctx->breaks, j);
            else vec_push(G->a, &ctx->continues, j);
            return;
        }
    }
}

static const char *qual_name(arena *a, const char *mod, const char *name) {
    return strchr(name, '.') ? name : arena_printf(a, "%s.%s", mod, name);
}

static void declare_fn(gen *G, const char *q, func *f, bool is_extern, fn_sig *sig) {
    if (!is_extern) {
        uint16_t id = (uint16_t)G->prog->funcs.len;
        map_put(&G->func_ids, intern_z(G->in, q), SLOT(id));
        // A method takes `this` as a hidden first argument.
        int dots = 0;
        for (const char *p = q; *p; p++) if (*p == '.') dots++;
        int extra = (!f->is_static && dots >= 2) ? 1 : 0;
        bc_func bf;
        memset(&bf, 0, sizeof bf);
        bf.name = intern_z(G->in, q);
        bf.arity = (uint8_t)(f->params.len + extra);
        vec_push(G->a, &G->prog->funcs, bf);
    }
    if (sig) map_put(&G->sigs, intern_z(G->in, q), sig);
}

static void compile_body(gen *G, const char *q, func *f) {
    void *id = map_get(&G->func_ids, q);
    if (!id) return;
    int fid = UNSLOT(id);

    G->locals.len = 0;
    G->scopes.len = 0;
    G->loops.len = 0;
    G->nslots = 0;
    G->temp = 0;

    // `this` occupies slot 0 for an instance method.
    if (G->this_ty) declare(G, "this", G->this_ty);

    fn_sig *sig = (fn_sig *)map_get(&G->sigs, q);
    for (int i = 0; i < f->params.len; i++) {
        ty *t = sig && i < sig->params.len ? sig->params.at[i].t : ty_error();
        declare(G, f->params.at[i].name, t);
    }

    emitter e;
    em_init(&e, G->a);
    if (f->body)
        for (int i = 0; i < f->body->stmts.len; i++) gen_stmt(G, f->body->stmts.at[i], &e);
    em_op(&e, OP_RET_VOID);

    G->prog->funcs.at[fid].nslots = G->nslots;
    G->prog->funcs.at[fid].code = e.code;
    G->prog->funcs.at[fid].code_len = (size_t)e.len;
}

// <start>: run the module-level initialisers, then call main. Imports first,
// so a global can read one from a module it imports.
typedef VEC(const char *) str_vec;

static void order_visit(gen *G, const char *m, str_vec *order, str_vec *seen) {
    for (int i = 0; i < seen->len; i++)
        if (strcmp(seen->at[i], m) == 0) return;
    vec_push(G->a, seen, m);

    module *md = module_find(G->l, m);
    if (md)
        for (int i = 0; i < md->imports.len; i++)
            order_visit(G, md->imports.at[i], order, seen);
    vec_push(G->a, order, m);
}

static uint16_t emit_start(gen *G, uint16_t main_id) {
    str_vec order;
    str_vec seen;
    memset(&order, 0, sizeof order);
    memset(&seen, 0, sizeof seen);
    order_visit(G, G->l->root, &order, &seen);
    // Anything loaded but not reachable from the root still gets its turn.
    for (int i = 0; i < G->l->modules.len; i++)
        order_visit(G, G->l->modules.at[i]->name, &order, &seen);

    uint16_t id = (uint16_t)G->prog->funcs.len;
    bc_func bf;
    memset(&bf, 0, sizeof bf);
    bf.name = "<start>";
    vec_push(G->a, &G->prog->funcs, bf);

    emitter e;
    em_init(&e, G->a);

    for (int oi = 0; oi < order.len; oi++) {
        module *m = module_find(G->l, order.at[oi]);
        if (!m) continue;
        set_imports(G, m);
        G->locals.len = 0;
        G->scopes.len = 0;
        G->loops.len = 0;
        G->nslots = 0;
        G->temp = 0;
        G->this_ty = NULL;

        for (int i = 0; i < m->prog->items.len; i++) {
            item *it = &m->prog->items.at[i];
            if (it->kind != IT_LET || it->as.let->kind != ST_LET) continue;
            ty *t = gen_expr(G, it->as.let->as.let_.value, &e);
            maybe_copy(G, t, &e);
            global_info *gi = (global_info *)map_get(
                &G->globals, arena_printf(G->a, "%s.%s", m->name, it->as.let->as.let_.name));
            if (gi) {
                em_op(&e, OP_STOREG);
                em_u16(&e, gi->index);
            } else {
                em_op(&e, OP_POP);
            }
        }
    }

    em_op(&e, OP_CALL);
    em_u16(&e, main_id);
    em_u8(&e, 0);
    // RET_VOID leaves nothing behind, so only discard a real result.
    fn_sig *msig = (fn_sig *)map_get(&G->sigs, arena_printf(G->a, "%s.main", G->l->root));
    if (msig && msig->ret->kind != TK_VOID) em_op(&e, OP_POP);
    em_op(&e, OP_HALT);

    G->prog->funcs.at[id].nslots = G->nslots;
    G->prog->funcs.at[id].code = e.code;
    G->prog->funcs.at[id].code_len = (size_t)e.len;
    return id;
}

static void enter(gen *G, module *m) {
    set_imports(G, m);
}

bc_program *generate(arena *a, sl_interner *in, loaded *l, decls *d, diag_vec *diags) {
    gen G;
    memset(&G, 0, sizeof G);
    G.a = a;
    G.in = in;
    G.d = d;
    G.l = l;
    G.diags = diags;
    G.current = "";
    map_init(&G.layouts, a);
    map_init(&G.elayouts, a);
    map_init(&G.func_ids, a);
    map_init(&G.globals, a);
    map_init(&G.sigs, a);

    bc_program *prog = NEW(a, bc_program);
    bc_init(prog, a);
    G.prog = prog;

    // Pass 1: layouts for every type, taken straight from the checker's
    // tables, so field indices and variant tags are known everywhere.
    for (int i = 0; i < d->structs.cap; i++) {
        const char *key = d->structs.slots[i].key;
        if (!key) continue;
        struct_info *si = (struct_info *)d->structs.slots[i].val;
        layout *lay = NEW(a, layout);
        for (int k = 0; k < si->fields.len; k++) {
            slot s;
            s.name = si->fields.at[k].name;
            s.t = si->fields.at[k].t;
            vec_push(a, &lay->fields, s);
        }
        map_put(&G.layouts, key, lay);
    }
    for (int i = 0; i < d->enums.cap; i++) {
        const char *key = d->enums.slots[i].key;
        if (!key) continue;
        enum_info *ei = (enum_info *)d->enums.slots[i].val;
        elayout *el = NEW(a, elayout);
        for (int k = 0; k < ei->variants.len; k++) {
            vlayout v;
            memset(&v, 0, sizeof v);
            v.name = ei->variants.at[k].name;
            for (int j = 0; j < ei->variants.at[k].payload.len; j++) {
                slot s;
                s.name = ei->variants.at[k].payload.at[j].name;
                s.t = ei->variants.at[k].payload.at[j].t;
                vec_push(a, &v.payload, s);
            }
            vec_push(a, &el->variants, v);
        }
        map_put(&G.elayouts, key, el);
    }

    // Pass 1b: a slot for every module-level binding. Initialisers are
    // compiled later, into a synthetic entry function.
    for (int mi = 0; mi < l->modules.len; mi++) {
        module *m = l->modules.at[mi];
        enter(&G, m);
        for (int i = 0; i < m->prog->items.len; i++) {
            item *it = &m->prog->items.at[i];
            if (it->kind != IT_LET || it->as.let->kind != ST_LET) continue;
            const char *q = intern_z(in, arena_printf(a, "%s.%s", m->name,
                                                      it->as.let->as.let_.name));
            global_info *gi = NEW(a, global_info);
            gi->index = prog->nglobals++;
            ty *t = (ty *)map_get(&d->consts, q);
            gi->t = t ? t : ty_error();
            map_put(&G.globals, q, gi);
        }
    }

    // Pass 2: reserve a slot for every function, including methods, so calls
    // can be emitted before the callee is compiled.
    for (int mi = 0; mi < l->modules.len; mi++) {
        module *m = l->modules.at[mi];
        enter(&G, m);
        for (int i = 0; i < m->prog->items.len; i++) {
            item *it = &m->prog->items.at[i];
            if (it->kind == IT_FUNC || it->kind == IT_EXTERN) {
                const char *q = arena_printf(a, "%s.%s", m->name, it->as.fn->name);
                declare_fn(&G, q, it->as.fn, it->kind == IT_EXTERN,
                           (fn_sig *)map_get(&d->funcs, q));
            } else if (it->kind == IT_STRUCT && it->as.st->generics.len == 0) {
                const char *sq = qual_name(a, m->name, it->as.st->name);
                struct_info *si = (struct_info *)map_get(&d->structs, sq);
                for (int k = 0; k < it->as.st->methods.len; k++) {
                    func *mt = it->as.st->methods.at[k];
                    const char *q = arena_printf(a, "%s.%s", sq, mt->name);
                    declare_fn(&G, q, mt, false,
                               si ? sig_lookup(&si->methods, mt->name) : NULL);
                }
            }
        }
    }

    for (int mi = 0; mi < l->modules.len; mi++) {
        module *m = l->modules.at[mi];
        enter(&G, m);
        for (int i = 0; i < m->prog->items.len; i++) {
            item *it = &m->prog->items.at[i];
            if (it->kind == IT_FUNC) {
                G.this_ty = NULL;
                compile_body(&G, arena_printf(a, "%s.%s", m->name, it->as.fn->name),
                             it->as.fn);
            } else if (it->kind == IT_STRUCT && it->as.st->generics.len == 0) {
                const char *sq = qual_name(a, m->name, it->as.st->name);
                for (int k = 0; k < it->as.st->methods.len; k++) {
                    func *mt = it->as.st->methods.at[k];
                    G.this_ty = mt->is_static ? NULL : ty_named(a, intern_z(in, sq));
                    compile_body(&G, arena_printf(a, "%s.%s", sq, mt->name), mt);
                }
            }
        }
    }

    const char *entry = arena_printf(a, "%s.main", l->root);
    void *main_id = map_get(&G.func_ids, entry);
    if (!main_id) {
        diag_add(a, diags, span_make(0, 0),
                 arena_printf(a, "no `main` function in `%s`", l->root));
        diags->at[diags->len - 1].module = l->root;
        return NULL;
    }
    prog->entry = emit_start(&G, (uint16_t)UNSLOT(main_id));

    return prog;
}
