#include "parse.h"

#include <stdio.h>

const char *path_text(arena *a, const path *p) {
    if (p->n == 1) return p->seg[0];
    size_t n = 0;
    for (int i = 0; i < p->n; i++) n += strlen(p->seg[i]) + 1;
    char *out = (char *)arena_alloc(a, n + 1);
    size_t at = 0;
    for (int i = 0; i < p->n; i++) {
        if (i) out[at++] = '.';
        size_t k = strlen(p->seg[i]);
        memcpy(out + at, p->seg[i], k);
        at += k;
    }
    out[at] = 0;
    return out;
}

const char *bin_op_symbol(bin_op op) {
    switch (op) {
        case OP_OR: return "||";
        case OP_AND: return "&&";
        case OP_EQ: return "==";
        case OP_NE: return "!=";
        case OP_LT: return "<";
        case OP_LE: return "<=";
        case OP_GT: return ">";
        case OP_GE: return ">=";
        case OP_ADD: return "+";
        case OP_SUB: return "-";
        case OP_MUL: return "*";
        case OP_DIV: return "/";
        case OP_REM: return "%";
    }
    return "?";
}

typedef struct {
    token_vec    toks;
    int          pos;
    arena       *a;
    sl_interner *in;
    diag_vec    *diags;
    // Stops a recovery loop spinning, and collapses repeated errors at one
    // position, usually one mistake seen from several depths.
    int          last_error_pos;
} parser;

// Widen a node's span across its children.
static span span_to(span x, span y) {
    span s;
    s.start = x.start < y.start ? x.start : y.start;
    s.end = x.end > y.end ? x.end : y.end;
    return s;
}

static const token *tok_at(parser *P, int i) {
    if (i < 0) i = 0;
    if (i >= P->toks.len) i = P->toks.len - 1;
    return &P->toks.at[i];
}

static token_kind peek(parser *P) { return tok_at(P, P->pos)->kind; }
static token_kind peek_at(parser *P, int n) { return tok_at(P, P->pos + n)->kind; }
static span cur_span(parser *P) { return tok_at(P, P->pos)->at; }
static span prev_span(parser *P) { return tok_at(P, P->pos - 1)->at; }
static bool at_end(parser *P) { return peek(P) == T_EOF; }

static const token *advance(parser *P) {
    const token *t = tok_at(P, P->pos);
    if (!at_end(P)) P->pos++;
    return t;
}

static bool check(parser *P, token_kind k) { return peek(P) == k; }

static bool eat(parser *P, token_kind k) {
    if (check(P, k)) { advance(P); return true; }
    return false;
}

static void perror_at(parser *P, span at, const char *msg) {
    if (P->last_error_pos == P->pos) return;
    P->last_error_pos = P->pos;
    diag_add(P->a, P->diags, at, msg);
}

static void perror_help(parser *P, span at, const char *msg, const char *help) {
    if (P->last_error_pos == P->pos) return;
    P->last_error_pos = P->pos;
    diag_add(P->a, P->diags, at, msg);
    P->diags->at[P->diags->len - 1].help = help;
}

// `k` was expected. Returns false and records a diagnostic if absent.
static bool expect(parser *P, token_kind k) {
    if (check(P, k)) { advance(P); return true; }
    const char *found = token_describe(P->a, tok_at(P, P->pos));
    perror_at(P, cur_span(P),
              arena_printf(P->a, "expected `%s`, found %s", token_lexeme(k), found));
    return false;
}

// A semicolon, with a message aimed at the most common cause.
static bool expect_semi(parser *P) {
    if (eat(P, T_SEMI)) return true;
    const char *found = token_describe(P->a, tok_at(P, P->pos));
    perror_help(P, prev_span(P),
                arena_printf(P->a, "expected `;`, found %s", found),
                "Solis requires a semicolon after a statement");
    return false;
}

static bool expect_ident(parser *P, const char **out, span *out_at) {
    span at = cur_span(P);
    if (peek(P) == T_IDENT) {
        const token *t = advance(P);
        if (out) *out = t->as.ident;
        if (out_at) *out_at = at;
        return true;
    }
    const char *found = token_describe(P->a, tok_at(P, P->pos));
    perror_at(P, at, arena_printf(P->a, "expected an identifier, found %s", found));
    return false;
}

// Skip ahead to somewhere a new statement or item plausibly starts.
static void synchronize(parser *P) {
    // Always consume at least one token so recovery cannot spin.
    if (!at_end(P)) advance(P);
    while (!at_end(P)) {
        token_kind prev = tok_at(P, P->pos - 1)->kind;
        if (prev == T_SEMI || prev == T_RBRACE) return;
        switch (peek(P)) {
            case T_FUNC: case T_IMPORT: case T_STRUCT: case T_ENUM:
            case T_INTERFACE: case T_EXTERN: case T_LET: case T_VAR:
            case T_IF: case T_WHILE: case T_FOR: case T_SWITCH:
            case T_RETURN: case T_ARENA: case T_STATIC: case T_RBRACE:
                return;
            default:
                advance(P);
        }
    }
}

static expr  *parse_expr(parser *P);
static type  *parse_type(parser *P);
static block *parse_block(parser *P);
static stmt  *parse_stmt(parser *P);
static func  *parse_func(parser *P);
static bool   parse_path(parser *P, path *out);

static bool parse_path(parser *P, path *out) {
    span start = cur_span(P);
    const char *first;
    if (!expect_ident(P, &first, NULL)) return false;

    VEC(const char *) segs;
    memset(&segs, 0, sizeof segs);
    vec_push(P->a, &segs, first);

    while (check(P, T_DOT)) {
        // Only a path separator when an identifier follows.
        if (peek_at(P, 1) != T_IDENT) break;
        advance(P);
        const char *seg;
        if (!expect_ident(P, &seg, NULL)) return false;
        vec_push(P->a, &segs, seg);
    }
    out->seg = segs.at;
    out->n = segs.len;
    out->at = span_to(start, prev_span(P));
    return true;
}

// `[T, U: Bound + Other]`, or empty.
static bool parse_generics(parser *P, generic_vec *out) {
    memset(out, 0, sizeof *out);
    if (!check(P, T_LBRACKET)) return true;
    advance(P);
    if (check(P, T_RBRACKET)) {
        perror_at(P, cur_span(P), "empty generic parameter list");
        advance(P);
        return true;
    }
    for (;;) {
        span start = cur_span(P);
        generic_param g;
        memset(&g, 0, sizeof g);
        if (!expect_ident(P, &g.name, NULL)) return false;
        if (eat(P, T_COLON)) {
            for (;;) {
                path b;
                if (!parse_path(P, &b)) return false;
                vec_push(P->a, &g.bounds, b);
                if (!eat(P, T_PLUS)) break;
            }
        }
        g.at = span_to(start, prev_span(P));
        vec_push(P->a, out, g);
        if (!eat(P, T_COMMA)) break;
    }
    return expect(P, T_RBRACKET);
}

static type *parse_type(parser *P) {
    span start = cur_span(P);
    type *t = NEW(P->a, type);
    t->is_weak = eat(P, T_WEAK);

    switch (peek(P)) {
        case T_LBRACKET: {           // [T]
            advance(P);
            t->kind = TY_ARRAY;
            if (!(t->as.array = parse_type(P))) return NULL;
            if (!expect(P, T_RBRACKET)) return NULL;
            break;
        }
        case T_LBRACE: {             // {K: V}
            advance(P);
            t->kind = TY_MAP;
            if (!(t->as.map.k = parse_type(P))) return NULL;
            if (!expect(P, T_COLON)) return NULL;
            if (!(t->as.map.v = parse_type(P))) return NULL;
            if (!expect(P, T_RBRACE)) return NULL;
            break;
        }
        case T_LPAREN: {             // () or (A, B)
            advance(P);
            t->kind = TY_TUPLE;
            if (!check(P, T_RPAREN)) {
                for (;;) {
                    type *e = parse_type(P);
                    if (!e) return NULL;
                    vec_push(P->a, &t->as.tuple, e);
                    if (!eat(P, T_COMMA)) break;
                }
            }
            if (!expect(P, T_RPAREN)) return NULL;
            break;
        }
        case T_FUNC: {               // func(A): B
            advance(P);
            t->kind = TY_FN;
            if (!expect(P, T_LPAREN)) return NULL;
            if (!check(P, T_RPAREN)) {
                for (;;) {
                    type *e = parse_type(P);
                    if (!e) return NULL;
                    vec_push(P->a, &t->as.fn.params, e);
                    if (!eat(P, T_COMMA)) break;
                }
            }
            if (!expect(P, T_RPAREN)) return NULL;
            if (eat(P, T_COLON)) {
                if (!(t->as.fn.ret = parse_type(P))) return NULL;
            }
            break;
        }
        case T_IDENT: {              // Name or Name[A, B]
            t->kind = TY_NAMED;
            if (!parse_path(P, &t->as.named.p)) return NULL;
            if (check(P, T_LBRACKET)) {
                advance(P);
                for (;;) {
                    type *e = parse_type(P);
                    if (!e) return NULL;
                    vec_push(P->a, &t->as.named.args, e);
                    if (!eat(P, T_COMMA)) break;
                }
                if (!expect(P, T_RBRACKET)) return NULL;
            }
            break;
        }
        default: {
            const char *found = token_describe(P->a, tok_at(P, P->pos));
            perror_at(P, cur_span(P),
                      arena_printf(P->a, "expected a type, found %s", found));
            return NULL;
        }
    }
    t->at = span_to(start, prev_span(P));
    return t;
}

// Signature only; leaves the body to the caller.
static func *parse_func_sig(parser *P, span start) {
    func *f = NEW(P->a, func);
    for (;;) {
        if (peek(P) == T_STATIC) { advance(P); f->is_static = true; }
        else if (peek(P) == T_MUT) { advance(P); f->is_mut = true; }
        else break;
    }
    if (!expect(P, T_FUNC)) return NULL;
    if (!expect_ident(P, &f->name, &f->name_at)) return NULL;
    if (!parse_generics(P, &f->generics)) return NULL;

    if (!expect(P, T_LPAREN)) return NULL;
    if (!check(P, T_RPAREN)) {
        for (;;) {
            span pstart = cur_span(P);
            param pm;
            memset(&pm, 0, sizeof pm);
            if (!expect_ident(P, &pm.name, NULL)) return NULL;
            if (!expect(P, T_COLON)) return NULL;
            if (!(pm.ty = parse_type(P))) return NULL;
            pm.at = span_to(pstart, prev_span(P));
            vec_push(P->a, &f->params, pm);
            if (!eat(P, T_COMMA)) break;
            if (check(P, T_RPAREN)) break;      // trailing comma
        }
    }
    if (!expect(P, T_RPAREN)) return NULL;

    if (eat(P, T_COLON)) {
        if (!(f->ret = parse_type(P))) return NULL;
    }
    f->at = span_to(start, prev_span(P));
    return f;
}

static func *parse_func(parser *P) {
    span start = cur_span(P);
    func *f = parse_func_sig(P, start);
    if (!f) return NULL;
    if (!(f->body = parse_block(P))) return NULL;
    f->at = span_to(start, prev_span(P));
    return f;
}

static bool parse_field(parser *P, field *out) {
    span start = cur_span(P);
    memset(out, 0, sizeof *out);
    out->is_weak = eat(P, T_WEAK);
    if (!expect_ident(P, &out->name, NULL)) return false;
    if (!expect(P, T_COLON)) return false;
    if (!(out->ty = parse_type(P))) return false;
    if (!expect_semi(P)) return false;
    out->at = span_to(start, prev_span(P));
    return true;
}

static struct_decl *parse_struct(parser *P) {
    span start = cur_span(P);
    struct_decl *s = NEW(P->a, struct_decl);
    if (!expect(P, T_STRUCT)) return NULL;
    if (!expect_ident(P, &s->name, NULL)) return NULL;
    if (!parse_generics(P, &s->generics)) return NULL;

    if (eat(P, T_COLON)) {
        for (;;) {
            iface_ref ref;
            memset(&ref, 0, sizeof ref);
            span rstart = cur_span(P);
            if (!parse_path(P, &ref.p)) return NULL;
            if (check(P, T_LBRACKET)) {
                advance(P);
                for (;;) {
                    type *ta = parse_type(P);
                    if (!ta) return NULL;
                    vec_push(P->a, &ref.args, ta);
                    if (!eat(P, T_COMMA)) break;
                }
                if (!expect(P, T_RBRACKET)) return NULL;
            }
            ref.at = span_to(rstart, prev_span(P));
            vec_push(P->a, &s->implements, ref);
            if (!eat(P, T_COMMA)) break;
        }
    }

    if (!expect(P, T_LBRACE)) return NULL;
    while (!check(P, T_RBRACE) && !at_end(P)) {
        token_kind k = peek(P);
        if (k == T_FUNC || k == T_STATIC || k == T_MUT) {
            func *m = parse_func(P);
            if (m) vec_push(P->a, &s->methods, m);
            else synchronize(P);
        } else {
            field fl;
            if (parse_field(P, &fl)) vec_push(P->a, &s->fields, fl);
            else synchronize(P);
        }
    }
    if (!expect(P, T_RBRACE)) return NULL;
    s->at = span_to(start, prev_span(P));
    return s;
}

static enum_decl *parse_enum(parser *P) {
    span start = cur_span(P);
    enum_decl *e = NEW(P->a, enum_decl);
    if (!expect(P, T_ENUM)) return NULL;
    if (!expect_ident(P, &e->name, NULL)) return NULL;
    if (!parse_generics(P, &e->generics)) return NULL;
    if (!expect(P, T_LBRACE)) return NULL;

    while (!check(P, T_RBRACE) && !at_end(P)) {
        span vstart = cur_span(P);
        variant v;
        memset(&v, 0, sizeof v);
        if (!expect_ident(P, &v.name, NULL)) {
            synchronize(P);
            continue;
        }
        if (eat(P, T_LPAREN)) {
            if (!check(P, T_RPAREN)) {
                for (;;) {
                    span pstart = cur_span(P);
                    param pm;
                    memset(&pm, 0, sizeof pm);
                    if (!expect_ident(P, &pm.name, NULL)) return NULL;
                    if (!expect(P, T_COLON)) return NULL;
                    if (!(pm.ty = parse_type(P))) return NULL;
                    pm.at = span_to(pstart, prev_span(P));
                    vec_push(P->a, &v.payload, pm);
                    if (!eat(P, T_COMMA)) break;
                }
            }
            if (!expect(P, T_RPAREN)) return NULL;
        }
        v.at = span_to(vstart, prev_span(P));
        vec_push(P->a, &e->variants, v);
        if (!eat(P, T_COMMA)) break;
    }
    if (!expect(P, T_RBRACE)) return NULL;
    e->at = span_to(start, prev_span(P));
    return e;
}

static interface_decl *parse_interface(parser *P) {
    span start = cur_span(P);
    interface_decl *d = NEW(P->a, interface_decl);
    if (!expect(P, T_INTERFACE)) return NULL;
    if (!expect_ident(P, &d->name, NULL)) return NULL;
    if (!parse_generics(P, &d->generics)) return NULL;
    if (!expect(P, T_LBRACE)) return NULL;

    while (!check(P, T_RBRACE) && !at_end(P)) {
        span mstart = cur_span(P);
        func *f = parse_func_sig(P, mstart);
        if (!f) { synchronize(P); continue; }

        // A body makes it a default implementation.
        if (check(P, T_LBRACE)) {
            if (!(f->body = parse_block(P))) { synchronize(P); continue; }
        } else if (!expect_semi(P)) {
            synchronize(P);
            continue;
        }
        f->at = span_to(mstart, prev_span(P));
        vec_push(P->a, &d->methods, f);
    }
    if (!expect(P, T_RBRACE)) return NULL;
    d->at = span_to(start, prev_span(P));
    return d;
}

static block *parse_block(parser *P) {
    span start = cur_span(P);
    if (!expect(P, T_LBRACE)) return NULL;
    block *b = NEW(P->a, block);
    while (!check(P, T_RBRACE) && !at_end(P)) {
        stmt *s = parse_stmt(P);
        if (s) vec_push(P->a, &b->stmts, s);
        else synchronize(P);
    }
    if (!expect(P, T_RBRACE)) return NULL;
    b->at = span_to(start, prev_span(P));
    return b;
}

static stmt *parse_let_stmt(parser *P) {
    span start = cur_span(P);
    stmt *s = NEW(P->a, stmt);
    s->kind = ST_LET;
    s->as.let_.is_var = peek(P) == T_VAR;
    advance(P);
    if (!expect_ident(P, &s->as.let_.name, NULL)) return NULL;
    if (eat(P, T_COLON)) {
        if (!(s->as.let_.ty = parse_type(P))) return NULL;
    }
    if (!expect(P, T_EQ)) return NULL;
    if (!(s->as.let_.value = parse_expr(P))) return NULL;
    if (!expect_semi(P)) return NULL;
    s->at = span_to(start, prev_span(P));
    return s;
}

static stmt *parse_if(parser *P) {
    span start = cur_span(P);
    if (!expect(P, T_IF)) return NULL;
    stmt *s = NEW(P->a, stmt);
    s->kind = ST_IF;
    if (!(s->as.if_.cond = parse_expr(P))) return NULL;
    if (!(s->as.if_.then = parse_block(P))) return NULL;
    if (eat(P, T_ELSE)) {
        if (check(P, T_IF)) {
            if (!(s->as.if_.els = parse_if(P))) return NULL;
        } else {
            block *b = parse_block(P);
            if (!b) return NULL;
            stmt *wrap = NEW(P->a, stmt);
            wrap->kind = ST_BLOCK;
            wrap->as.blk = b;
            wrap->at = b->at;
            s->as.if_.els = wrap;
        }
    }
    s->at = span_to(start, prev_span(P));
    return s;
}

// A case body runs until the next `case`, a `default:`, or the closing brace.
static bool is_case_boundary(parser *P) {
    if (at_end(P) || check(P, T_CASE) || check(P, T_RBRACE)) return true;
    if (peek(P) == T_IDENT && strcmp(tok_at(P, P->pos)->as.ident, "default") == 0 &&
        peek_at(P, 1) == T_COLON) {
        return true;
    }
    return false;
}

static bool parse_pattern(parser *P, pattern *out) {
    span start = cur_span(P);
    memset(out, 0, sizeof *out);

    switch (peek(P)) {
        case T_UNDERSCORE: advance(P); out->kind = PAT_WILDCARD; break;
        case T_INT: out->kind = PAT_INT; out->as.i = advance(P)->as.i; break;
        case T_TRUE: advance(P); out->kind = PAT_BOOL; out->as.b = true; break;
        case T_FALSE: advance(P); out->kind = PAT_BOOL; out->as.b = false; break;
        case T_NULL: advance(P); out->kind = PAT_NULL; break;
        case T_STR: {
            const token *t = advance(P);
            out->kind = PAT_STR;
            // A pattern is a constant, so interpolation cannot appear.
            size_t n = 0;
            for (int i = 0; i < t->as.parts.len; i++) {
                if (t->as.parts.at[i].is_expr) {
                    perror_at(P, start, "a pattern cannot contain string interpolation");
                } else {
                    n += strlen(t->as.parts.at[i].text);
                }
            }
            char *buf = (char *)arena_alloc(P->a, n + 1);
            size_t k = 0;
            for (int i = 0; i < t->as.parts.len; i++) {
                if (t->as.parts.at[i].is_expr) continue;
                size_t m = strlen(t->as.parts.at[i].text);
                memcpy(buf + k, t->as.parts.at[i].text, m);
                k += m;
            }
            buf[k] = 0;
            out->as.s = buf;
            break;
        }
        case T_IDENT: {
            out->kind = PAT_VARIANT;
            if (!parse_path(P, &out->as.variant.p)) return false;
            if (eat(P, T_LPAREN)) {
                if (!check(P, T_RPAREN)) {
                    for (;;) {
                        binding b;
                        if (!expect_ident(P, &b.name, &b.at)) return false;
                        vec_push(P->a, &out->as.variant.bindings, b);
                        if (!eat(P, T_COMMA)) break;
                    }
                }
                if (!expect(P, T_RPAREN)) return false;
            }
            break;
        }
        default: {
            const char *found = token_describe(P->a, tok_at(P, P->pos));
            perror_at(P, cur_span(P),
                      arena_printf(P->a, "expected a pattern, found %s", found));
            return false;
        }
    }
    out->at = span_to(start, prev_span(P));
    return true;
}

static stmt *parse_switch(parser *P) {
    span start = cur_span(P);
    if (!expect(P, T_SWITCH)) return NULL;
    stmt *s = NEW(P->a, stmt);
    s->kind = ST_SWITCH;
    if (!(s->as.switch_.scrutinee = parse_expr(P))) return NULL;
    if (!expect(P, T_LBRACE)) return NULL;

    while (!check(P, T_RBRACE) && !at_end(P)) {
        span cstart = cur_span(P);

        // `default:` is spelled as a plain identifier so it need not be a
        // reserved word.
        if (peek(P) == T_IDENT && strcmp(tok_at(P, P->pos)->as.ident, "default") == 0 &&
            peek_at(P, 1) == T_COLON) {
            advance(P);
            advance(P);
            block *d = NEW(P->a, block);
            while (!is_case_boundary(P)) {
                stmt *st = parse_stmt(P);
                if (st) vec_push(P->a, &d->stmts, st);
                else synchronize(P);
            }
            d->at = span_to(cstart, prev_span(P));
            s->as.switch_.dflt = d;
            continue;
        }

        if (!expect(P, T_CASE)) { synchronize(P); continue; }

        switch_case c;
        memset(&c, 0, sizeof c);
        for (;;) {
            pattern pat;
            if (!parse_pattern(P, &pat)) { synchronize(P); break; }
            vec_push(P->a, &c.patterns, pat);
            if (!eat(P, T_COMMA)) break;
        }
        if (!expect(P, T_COLON)) { synchronize(P); continue; }

        while (!is_case_boundary(P)) {
            stmt *st = parse_stmt(P);
            if (st) vec_push(P->a, &c.body, st);
            else synchronize(P);
        }
        c.at = span_to(cstart, prev_span(P));
        vec_push(P->a, &s->as.switch_.cases, c);
    }

    if (!expect(P, T_RBRACE)) return NULL;
    s->at = span_to(start, prev_span(P));
    return s;
}

// Can this expression appear on the left of an assignment?
static bool is_place(const expr *e) {
    return e->kind == EX_PATH || e->kind == EX_THIS ||
           e->kind == EX_FIELD || e->kind == EX_INDEX;
}

static bool assign_op_of(token_kind k, assign_op *out) {
    switch (k) {
        case T_EQ: *out = ASSIGN_SET; return true;
        case T_PLUS_EQ: *out = ASSIGN_ADD; return true;
        case T_MINUS_EQ: *out = ASSIGN_SUB; return true;
        case T_STAR_EQ: *out = ASSIGN_MUL; return true;
        case T_SLASH_EQ: *out = ASSIGN_DIV; return true;
        case T_PERCENT_EQ: *out = ASSIGN_REM; return true;
        default: return false;
    }
}

static stmt *parse_stmt(parser *P) {
    span start = cur_span(P);

    switch (peek(P)) {
        case T_LET:
        case T_VAR:
            return parse_let_stmt(P);

        case T_IF:
            return parse_if(P);

        case T_WHILE: {
            advance(P);
            stmt *s = NEW(P->a, stmt);
            s->kind = ST_WHILE;
            if (!(s->as.while_.cond = parse_expr(P))) return NULL;
            if (!(s->as.while_.body = parse_block(P))) return NULL;
            s->at = span_to(start, prev_span(P));
            return s;
        }

        case T_FOR: {
            advance(P);
            stmt *s = NEW(P->a, stmt);
            s->kind = ST_FOR;
            if (!expect_ident(P, &s->as.for_.var, NULL)) return NULL;
            if (!expect(P, T_IN)) return NULL;
            if (!(s->as.for_.iter = parse_expr(P))) return NULL;
            if (!(s->as.for_.body = parse_block(P))) return NULL;
            s->at = span_to(start, prev_span(P));
            return s;
        }

        case T_SWITCH:
            return parse_switch(P);

        case T_ARENA: {
            advance(P);
            block *b = parse_block(P);
            if (!b) return NULL;
            stmt *s = NEW(P->a, stmt);
            s->kind = ST_ARENA;
            s->as.blk = b;
            s->at = span_to(start, prev_span(P));
            return s;
        }

        case T_RETURN: {
            advance(P);
            stmt *s = NEW(P->a, stmt);
            s->kind = ST_RETURN;
            if (!check(P, T_SEMI)) {
                if (!(s->as.value = parse_expr(P))) return NULL;
            }
            if (!expect_semi(P)) return NULL;
            s->at = span_to(start, prev_span(P));
            return s;
        }

        case T_BREAK:
        case T_CONTINUE: {
            bool is_break = peek(P) == T_BREAK;
            advance(P);
            if (!expect_semi(P)) return NULL;
            stmt *s = NEW(P->a, stmt);
            s->kind = is_break ? ST_BREAK : ST_CONTINUE;
            s->at = span_to(start, prev_span(P));
            return s;
        }

        case T_LBRACE: {
            block *b = parse_block(P);
            if (!b) return NULL;
            stmt *s = NEW(P->a, stmt);
            s->kind = ST_BLOCK;
            s->as.blk = b;
            s->at = span_to(start, prev_span(P));
            return s;
        }

        default: {
            // Either an assignment or a bare expression statement.
            expr *e = parse_expr(P);
            if (!e) return NULL;

            assign_op op;
            if (assign_op_of(peek(P), &op)) {
                if (!is_place(e)) {
                    perror_help(P, e->at,
                                "left side of an assignment must be a variable, field or index",
                                "you cannot assign to a call or a literal");
                }
                advance(P);
                expr *v = parse_expr(P);
                if (!v) return NULL;
                if (!expect_semi(P)) return NULL;
                stmt *s = NEW(P->a, stmt);
                s->kind = ST_ASSIGN;
                s->as.assign.place = e;
                s->as.assign.op = op;
                s->as.assign.value = v;
                s->at = span_to(start, prev_span(P));
                return s;
            }
            if (!expect_semi(P)) return NULL;
            stmt *s = NEW(P->a, stmt);
            s->kind = ST_EXPR;
            s->as.value = e;
            s->at = span_to(start, prev_span(P));
            return s;
        }
    }
}

static expr *mk_bin(parser *P, bin_op op, expr *lhs, expr *rhs) {
    expr *e = NEW(P->a, expr);
    e->kind = EX_BINARY;
    e->as.binary.op = op;
    e->as.binary.lhs = lhs;
    e->as.binary.rhs = rhs;
    e->at = span_to(lhs->at, rhs->at);
    return e;
}

// Re-parse each `${...}` fragment the lexer captured as raw source.
static str_seg_vec lower_string(parser *P, const str_part_vec *parts, span at) {
    str_seg_vec out;
    memset(&out, 0, sizeof out);

    for (int i = 0; i < parts->len; i++) {
        str_seg seg;
        memset(&seg, 0, sizeof seg);
        if (!parts->at[i].is_expr) {
            seg.is_interp = false;
            seg.text = parts->at[i].text;
            vec_push(P->a, &out, seg);
            continue;
        }

        const char *src = parts->at[i].text;
        diag_vec sub_diags;
        memset(&sub_diags, 0, sizeof sub_diags);
        token_vec toks = lex(P->a, P->in, src, strlen(src), &sub_diags);
        if (sub_diags.len) {
            diag_addf(P->a, P->diags, at,
                      "invalid expression in interpolation: `%s`", src);
            continue;
        }

        parser sub;
        sub.toks = toks;
        sub.pos = 0;
        sub.a = P->a;
        sub.in = P->in;
        sub.diags = &sub_diags;
        sub.last_error_pos = -1;

        expr *e = parse_expr(&sub);
        if (e && at_end(&sub) && sub_diags.len == 0) {
            seg.is_interp = true;
            seg.value = e;
            vec_push(P->a, &out, seg);
        } else {
            diag_help(P->a, P->diags, at,
                      "`${...}` must contain a single expression",
                      "invalid expression in interpolation: `%s`", src);
        }
    }
    return out;
}

// Distinguishes `Vec2 { x: 1 }` from `if cond { ... }`.
static bool looks_like_struct_lit(parser *P) {
    if (peek_at(P, 1) == T_RBRACE) return true;
    if (peek_at(P, 1) != T_IDENT) return false;
    token_kind k = peek_at(P, 2);
    return k == T_COLON || k == T_COMMA || k == T_RBRACE;
}

static expr *parse_primary(parser *P) {
    span start = cur_span(P);
    expr *e = NEW(P->a, expr);

    switch (peek(P)) {
        case T_INT: e->kind = EX_INT; e->as.i = advance(P)->as.i; break;
        case T_FLOAT: e->kind = EX_FLOAT; e->as.f = advance(P)->as.f; break;
        case T_TRUE: advance(P); e->kind = EX_BOOL; e->as.b = true; break;
        case T_FALSE: advance(P); e->kind = EX_BOOL; e->as.b = false; break;
        case T_NULL: advance(P); e->kind = EX_NULL; break;
        case T_THIS: advance(P); e->kind = EX_THIS; break;

        case T_STR: {
            const token *t = advance(P);
            e->kind = EX_STR;
            e->as.str = lower_string(P, &t->as.parts, start);
            break;
        }

        case T_LPAREN: {
            advance(P);
            expr *inner = parse_expr(P);
            if (!inner) return NULL;
            if (!expect(P, T_RPAREN)) return NULL;
            return inner;        // parentheses leave no node behind
        }

        case T_LBRACKET: {
            advance(P);
            e->kind = EX_ARRAY;
            if (!check(P, T_RBRACKET)) {
                for (;;) {
                    expr *it = parse_expr(P);
                    if (!it) return NULL;
                    vec_push(P->a, &e->as.array, it);
                    if (!eat(P, T_COMMA)) break;
                    if (check(P, T_RBRACKET)) break;
                }
            }
            if (!expect(P, T_RBRACKET)) return NULL;
            break;
        }

        case T_LBRACE: {
            advance(P);
            e->kind = EX_MAP;
            if (!check(P, T_RBRACE)) {
                for (;;) {
                    map_entry me;
                    if (!(me.k = parse_expr(P))) return NULL;
                    if (!expect(P, T_COLON)) return NULL;
                    if (!(me.v = parse_expr(P))) return NULL;
                    vec_push(P->a, &e->as.map, me);
                    if (!eat(P, T_COMMA)) break;
                    if (check(P, T_RBRACE)) break;
                }
            }
            if (!expect(P, T_RBRACE)) return NULL;
            break;
        }

        case T_IDENT: {
            // One identifier only. `.` belongs to parse_postfix, so `a.b`
            // is always a Field node. Field access, Enum.Variant and
            // Type.staticMethod then share a spelling and the checker sorts
            // out which is which.
            const char *name;
            span nspan;
            if (!expect_ident(P, &name, &nspan)) return NULL;

            path p;
            p.seg = (const char **)arena_alloc(P->a, sizeof(char *));
            p.seg[0] = name;
            p.n = 1;
            p.at = nspan;

            // A `{` here is a struct literal, but only where a literal is
            // actually possible. `if x { ... }` must not be read as one.
            if (check(P, T_LBRACE) && looks_like_struct_lit(P)) {
                advance(P);
                e->kind = EX_STRUCT_LIT;
                e->as.struct_lit.p = p;
                if (!check(P, T_RBRACE)) {
                    for (;;) {
                        field_init fi;
                        span fspan;
                        if (!expect_ident(P, &fi.name, &fspan)) return NULL;
                        if (eat(P, T_COLON)) {
                            if (!(fi.value = parse_expr(P))) return NULL;
                        } else {
                            // shorthand punning: `Vec2 { x, y }`
                            expr *v = NEW(P->a, expr);
                            v->kind = EX_PATH;
                            v->as.p.seg = (const char **)arena_alloc(P->a, sizeof(char *));
                            v->as.p.seg[0] = fi.name;
                            v->as.p.n = 1;
                            v->as.p.at = fspan;
                            v->at = fspan;
                            fi.value = v;
                        }
                        vec_push(P->a, &e->as.struct_lit.fields, fi);
                        if (!eat(P, T_COMMA)) break;
                        if (check(P, T_RBRACE)) break;
                    }
                }
                if (!expect(P, T_RBRACE)) return NULL;
            } else {
                e->kind = EX_PATH;
                e->as.p = p;
            }
            break;
        }

        default: {
            const char *found = token_describe(P->a, tok_at(P, P->pos));
            perror_at(P, cur_span(P),
                      arena_printf(P->a, "expected an expression, found %s", found));
            return NULL;
        }
    }
    e->at = span_to(start, prev_span(P));
    return e;
}

static expr *parse_postfix(parser *P) {
    expr *e = parse_primary(P);
    if (!e) return NULL;

    for (;;) {
        switch (peek(P)) {
            case T_DOT: {
                advance(P);
                const char *name;
                if (!expect_ident(P, &name, NULL)) return NULL;
                expr *f = NEW(P->a, expr);
                f->kind = EX_FIELD;
                f->as.field.base = e;
                f->as.field.name = name;
                f->as.field.optional = false;
                f->at = span_to(e->at, prev_span(P));
                e = f;
                break;
            }
            case T_QUESTION: {
                // `?.`: null-short-circuiting field access
                if (peek_at(P, 1) != T_DOT) {
                    perror_help(P, cur_span(P), "stray `?`",
                                "`?` is only valid as part of `?.` for null-safe field access");
                    return NULL;
                }
                advance(P);
                advance(P);
                const char *name;
                if (!expect_ident(P, &name, NULL)) return NULL;
                expr *f = NEW(P->a, expr);
                f->kind = EX_FIELD;
                f->as.field.base = e;
                f->as.field.name = name;
                f->as.field.optional = true;
                f->at = span_to(e->at, prev_span(P));
                e = f;
                break;
            }
            case T_LPAREN: {
                advance(P);
                expr *c = NEW(P->a, expr);
                c->kind = EX_CALL;
                c->as.call.callee = e;
                if (!check(P, T_RPAREN)) {
                    for (;;) {
                        expr *arg = parse_expr(P);
                        if (!arg) return NULL;
                        vec_push(P->a, &c->as.call.args, arg);
                        if (!eat(P, T_COMMA)) break;
                        if (check(P, T_RPAREN)) break;
                    }
                }
                if (!expect(P, T_RPAREN)) return NULL;
                c->at = span_to(e->at, prev_span(P));
                e = c;
                break;
            }
            case T_LBRACKET: {
                advance(P);
                expr *lo = NULL, *hi = NULL;
                bool is_slice = false;
                if (check(P, T_COLON)) {
                    is_slice = true;
                    advance(P);
                } else {
                    if (!(lo = parse_expr(P))) return NULL;
                    if (check(P, T_COLON)) {
                        is_slice = true;
                        advance(P);
                    }
                }
                if (is_slice) {
                    if (!check(P, T_RBRACKET) && !(hi = parse_expr(P))) return NULL;
                    if (!expect(P, T_RBRACKET)) return NULL;
                    expr *sl = NEW(P->a, expr);
                    sl->kind = EX_SLICE;
                    sl->as.slice.base = e;
                    sl->as.slice.lo = lo;
                    sl->as.slice.hi = hi;
                    sl->at = span_to(e->at, prev_span(P));
                    e = sl;
                    break;
                }
                if (!expect(P, T_RBRACKET)) return NULL;
                expr *ix = NEW(P->a, expr);
                ix->kind = EX_INDEX;
                ix->as.index.base = e;
                ix->as.index.index = lo;
                ix->at = span_to(e->at, prev_span(P));
                e = ix;
                break;
            }
            default:
                return e;
        }
    }
}

static expr *parse_unary(parser *P) {
    span start = cur_span(P);
    un_op op;
    if (peek(P) == T_BANG) op = UN_NOT;
    else if (peek(P) == T_MINUS) op = UN_NEG;
    else return parse_postfix(P);

    advance(P);
    expr *rhs = parse_unary(P);
    if (!rhs) return NULL;
    expr *e = NEW(P->a, expr);
    e->kind = EX_UNARY;
    e->as.unary.op = op;
    e->as.unary.rhs = rhs;
    e->at = span_to(start, rhs->at);
    return e;
}

// The precedence ladder, loosest first. Each level loops so operators are
// left-associative.

static expr *parse_mul(parser *P) {
    expr *lhs = parse_unary(P);
    if (!lhs) return NULL;
    for (;;) {
        bin_op op;
        if (peek(P) == T_STAR) op = OP_MUL;
        else if (peek(P) == T_SLASH) op = OP_DIV;
        else if (peek(P) == T_PERCENT) op = OP_REM;
        else return lhs;
        advance(P);
        expr *rhs = parse_unary(P);
        if (!rhs) return NULL;
        lhs = mk_bin(P, op, lhs, rhs);
    }
}

static expr *parse_add(parser *P) {
    expr *lhs = parse_mul(P);
    if (!lhs) return NULL;
    for (;;) {
        bin_op op;
        if (peek(P) == T_PLUS) op = OP_ADD;
        else if (peek(P) == T_MINUS) op = OP_SUB;
        else return lhs;
        advance(P);
        expr *rhs = parse_mul(P);
        if (!rhs) return NULL;
        lhs = mk_bin(P, op, lhs, rhs);
    }
}

static expr *parse_compare(parser *P) {
    expr *lhs = parse_add(P);
    if (!lhs) return NULL;
    for (;;) {
        bin_op op;
        if (peek(P) == T_LT) op = OP_LT;
        else if (peek(P) == T_LT_EQ) op = OP_LE;
        else if (peek(P) == T_GT) op = OP_GT;
        else if (peek(P) == T_GT_EQ) op = OP_GE;
        else return lhs;
        advance(P);
        expr *rhs = parse_add(P);
        if (!rhs) return NULL;
        lhs = mk_bin(P, op, lhs, rhs);
    }
}

static expr *parse_equality(parser *P) {
    expr *lhs = parse_compare(P);
    if (!lhs) return NULL;
    for (;;) {
        bin_op op;
        if (peek(P) == T_EQ_EQ) op = OP_EQ;
        else if (peek(P) == T_BANG_EQ) op = OP_NE;
        else return lhs;
        advance(P);
        expr *rhs = parse_compare(P);
        if (!rhs) return NULL;
        lhs = mk_bin(P, op, lhs, rhs);
    }
}

static expr *parse_and(parser *P) {
    expr *lhs = parse_equality(P);
    if (!lhs) return NULL;
    while (check(P, T_AND_AND)) {
        advance(P);
        expr *rhs = parse_equality(P);
        if (!rhs) return NULL;
        lhs = mk_bin(P, OP_AND, lhs, rhs);
    }
    return lhs;
}

static expr *parse_expr(parser *P) {
    expr *lhs = parse_and(P);
    if (!lhs) return NULL;
    while (check(P, T_OR_OR)) {
        advance(P);
        expr *rhs = parse_and(P);
        if (!rhs) return NULL;
        lhs = mk_bin(P, OP_OR, lhs, rhs);
    }
    return lhs;
}

static bool parse_item(parser *P, item *out) {
    memset(out, 0, sizeof *out);

    switch (peek(P)) {
        case T_STRUCT:
            out->kind = IT_STRUCT;
            return (out->as.st = parse_struct(P)) != NULL;

        case T_ENUM:
            out->kind = IT_ENUM;
            return (out->as.en = parse_enum(P)) != NULL;

        case T_INTERFACE:
            out->kind = IT_INTERFACE;
            return (out->as.iface = parse_interface(P)) != NULL;

        case T_EXTERN: {
            span start = cur_span(P);
            advance(P);
            func *f = parse_func_sig(P, start);
            if (!f) return false;
            if (!expect_semi(P)) return false;
            f->at = span_to(start, prev_span(P));
            out->kind = IT_EXTERN;
            out->as.fn = f;
            return true;
        }

        case T_IMPORT: {
            span start = cur_span(P);
            advance(P);
            const char *name;
            if (!expect_ident(P, &name, NULL)) return false;
            if (!expect_semi(P)) return false;
            out->kind = IT_IMPORT;
            out->as.import.name = name;
            out->as.import.at = span_to(start, prev_span(P));
            return true;
        }

        case T_FUNC:
        case T_STATIC:
        case T_MUT:
            out->kind = IT_FUNC;
            return (out->as.fn = parse_func(P)) != NULL;

        case T_LET:
        case T_VAR:
            out->kind = IT_LET;
            return (out->as.let = parse_let_stmt(P)) != NULL;

        default: {
            const char *found = token_describe(P->a, tok_at(P, P->pos));
            perror_help(P, cur_span(P),
                        arena_printf(P->a, "expected a declaration, found %s", found),
                        "items start with `import`, `func`, `struct`, `enum`, "
                        "`interface`, `extern`, `let` or `var`");
            return false;
        }
    }
}

program *parse(arena *a, sl_interner *in, const char *src, size_t len, diag_vec *diags) {
    parser P;
    P.a = a;
    P.in = in;
    P.diags = diags;
    P.pos = 0;
    P.last_error_pos = -1;
    P.toks = lex(a, in, src, len, diags);

    program *prog = NEW(a, program);
    while (!at_end(&P)) {
        item it;
        if (parse_item(&P, &it)) vec_push(a, &prog->items, it);
        else synchronize(&P);
    }
    return prog;
}
