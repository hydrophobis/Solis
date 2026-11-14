#include "solis_syntax.h"

#include "lex.h"
#include "parse.h"

#include <stdlib.h>
#include <string.h>

struct sl_syntax {
    arena          arena;
    char          *source;
    size_t         len;
    sl_token      *tokens;
    int            ntokens;
    sl_comment    *comments;
    int            ncomments;
    sl_symbol     *symbols;
    int            nsymbols;
    sl_diagnostic *diagnostics;
    int            ndiagnostics;
};

// The enum in the header is a copy of the compiler's; keep them in step.
_Static_assert((int)SL_T_EOF == (int)T_EOF, "sl_token_kind is out of step with token_kind");
_Static_assert((int)SL_T_OR_OR == (int)T_OR_OR, "sl_token_kind is out of step with token_kind");
_Static_assert((int)SL_T_IMPORT == (int)T_IMPORT, "sl_token_kind is out of step with token_kind");
_Static_assert((int)SL_T_STR == (int)T_STR, "sl_token_kind is out of step with token_kind");

static char *copy_str(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

typedef struct {
    sl_symbol *at;
    int len, cap;
    arena *a;
    const sl_token *toks;
    int ntoks;
    const char *src;
} symtab;

// The span of the first identifier called `name` at or after `from`, within `to`.
// Declarations record where they start, not where their name is.
static void name_span(const symtab *t, uint32_t from, uint32_t to, const char *name,
                      uint32_t *start, uint32_t *end) {
    size_t n = strlen(name);
    for (int i = 0; i < t->ntoks; i++) {
        const sl_token *k = &t->toks[i];
        if (k->start < from) continue;
        if (k->start >= to) break;
        if (k->kind == SL_T_IDENT && k->end - k->start == n && memcmp(t->src + k->start, name, n) == 0) {
            *start = k->start;
            *end = k->end;
            return;
        }
    }
    *start = from;
    *end = from;
}

static int add_symbol(symtab *t, sl_symbol_kind kind, const char *name, int parent,
                      span whole, bool has_name_span, span name_at) {
    if (t->len == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 32;
        t->at = (sl_symbol *)realloc(t->at, (size_t)t->cap * sizeof(sl_symbol));
        if (!t->at) return -1;
    }
    sl_symbol *s = &t->at[t->len];
    s->kind = kind;
    s->name = arena_strdup(t->a, name ? name : "");
    s->parent = parent;
    s->start = whole.start;
    s->end = whole.end;
    if (has_name_span) {
        s->name_start = name_at.start;
        s->name_end = name_at.end;
    } else {
        name_span(t, whole.start, whole.end, s->name, &s->name_start, &s->name_end);
    }
    return t->len++;
}

static void collect_func(symtab *t, const func *f, sl_symbol_kind kind, int parent) {
    add_symbol(t, kind, f->name, parent, f->at, true, f->name_at);
}

static void collect(symtab *t, const program *p) {
    span none = span_make(0, 0);
    for (int i = 0; i < p->items.len; i++) {
        const item *it = &p->items.at[i];
        switch (it->kind) {
            case IT_FUNC: collect_func(t, it->as.fn, SL_SYM_FUNC, -1); break;
            case IT_EXTERN: collect_func(t, it->as.fn, SL_SYM_EXTERN, -1); break;
            case IT_IMPORT:
                add_symbol(t, SL_SYM_IMPORT, it->as.import.name, -1, it->as.import.at, true, it->as.import.at);
                break;
            case IT_LET:
                add_symbol(t, SL_SYM_LET, it->as.let->as.let_.name, -1, it->as.let->at, false, none);
                break;
            case IT_STRUCT: {
                const struct_decl *s = it->as.st;
                int me = add_symbol(t, SL_SYM_STRUCT, s->name, -1, s->at, false, none);
                for (int k = 0; me >= 0 && k < s->fields.len; k++)
                    add_symbol(t, SL_SYM_FIELD, s->fields.at[k].name, me, s->fields.at[k].at, false, none);
                for (int k = 0; me >= 0 && k < s->methods.len; k++)
                    collect_func(t, s->methods.at[k], SL_SYM_METHOD, me);
                break;
            }
            case IT_ENUM: {
                const enum_decl *e = it->as.en;
                int me = add_symbol(t, SL_SYM_ENUM, e->name, -1, e->at, false, none);
                for (int k = 0; me >= 0 && k < e->variants.len; k++)
                    add_symbol(t, SL_SYM_VARIANT, e->variants.at[k].name, me, e->variants.at[k].at, false, none);
                break;
            }
            case IT_INTERFACE: {
                const interface_decl *d = it->as.iface;
                int me = add_symbol(t, SL_SYM_INTERFACE, d->name, -1, d->at, false, none);
                for (int k = 0; me >= 0 && k < d->methods.len; k++)
                    collect_func(t, d->methods.at[k], SL_SYM_METHOD, me);
                break;
            }
        }
    }
}

sl_syntax *sl_syntax_parse(const char *source, size_t len) {
    sl_syntax *s = (sl_syntax *)calloc(1, sizeof *s);
    if (!s) return NULL;
    arena_init(&s->arena);
    s->source = (char *)malloc(len + 1);
    if (!s->source) { arena_free(&s->arena); free(s); return NULL; }
    memcpy(s->source, source, len);
    s->source[len] = 0;
    s->len = len;

    sl_interner *in = intern_new(&s->arena);

    // Tokens and comments from one scan; the diagnostics from it are dropped,
    // because the parse below scans again and reports the same ones.
    diag_vec scratch;
    memset(&scratch, 0, sizeof scratch);
    span_vec comments;
    memset(&comments, 0, sizeof comments);
    token_vec toks = lex_with_comments(&s->arena, in, s->source, len, &scratch, &comments);

    s->tokens = (sl_token *)malloc((size_t)(toks.len ? toks.len : 1) * sizeof(sl_token));
    s->comments = (sl_comment *)malloc((size_t)(comments.len ? comments.len : 1) * sizeof(sl_comment));
    if (!s->tokens || !s->comments) { sl_syntax_free(s); return NULL; }
    for (int i = 0; i < toks.len; i++) {
        s->tokens[i].kind = (sl_token_kind)toks.at[i].kind;
        s->tokens[i].start = toks.at[i].at.start;
        s->tokens[i].end = toks.at[i].at.end;
    }
    s->ntokens = toks.len;
    for (int i = 0; i < comments.len; i++) {
        s->comments[i].start = comments.at[i].start;
        s->comments[i].end = comments.at[i].end;
    }
    s->ncomments = comments.len;

    diag_vec diags;
    memset(&diags, 0, sizeof diags);
    program *prog = parse(&s->arena, in, s->source, len, &diags);

    symtab t;
    memset(&t, 0, sizeof t);
    t.a = &s->arena;
    t.toks = s->tokens;
    t.ntoks = s->ntokens;
    t.src = s->source;
    if (prog) collect(&t, prog);
    s->symbols = t.at;
    s->nsymbols = t.len;

    if (diags.len) {
        s->diagnostics = (sl_diagnostic *)calloc((size_t)diags.len, sizeof(sl_diagnostic));
        if (s->diagnostics) {
            s->ndiagnostics = diags.len;
            for (int i = 0; i < diags.len; i++) {
                const diag *d = &diags.at[i];
                line_col a = line_col_of(s->source, len, d->at.start);
                line_col b = line_col_of(s->source, len, d->at.end);
                sl_diagnostic *o = &s->diagnostics[i];
                o->file = copy_str("<source>");
                o->line = a.line; o->col = a.col;
                o->end_line = b.line; o->end_col = b.col;
                o->message = copy_str(d->message);
                o->help = d->help ? copy_str(d->help) : NULL;
            }
        }
    }
    return s;
}

void sl_syntax_free(sl_syntax *s) {
    if (!s) return;
    for (int i = 0; i < s->ndiagnostics; i++) {
        free(s->diagnostics[i].file);
        free(s->diagnostics[i].message);
        free(s->diagnostics[i].help);
    }
    free(s->diagnostics);
    free(s->symbols);
    free(s->comments);
    free(s->tokens);
    free(s->source);
    arena_free(&s->arena);
    free(s);
}

const char *sl_syntax_source(const sl_syntax *s, size_t *len) {
    if (len) *len = s->len;
    return s->source;
}
const sl_token *sl_syntax_tokens(const sl_syntax *s, int *count) { *count = s->ntokens; return s->tokens; }
const sl_comment *sl_syntax_comments(const sl_syntax *s, int *count) { *count = s->ncomments; return s->comments; }
const sl_symbol *sl_syntax_symbols(const sl_syntax *s, int *count) { *count = s->nsymbols; return s->symbols; }
const sl_diagnostic *sl_syntax_diagnostics(const sl_syntax *s, int *count) { *count = s->ndiagnostics; return s->diagnostics; }

const char *sl_token_kind_name(sl_token_kind kind) {
    switch (kind) {
        case SL_T_INT: return "int";
        case SL_T_FLOAT: return "float";
        case SL_T_STR: return "string";
        case SL_T_IDENT: return "ident";
        case SL_T_EOF: return "eof";
        default: break;
    }
    const char *lx = token_lexeme((token_kind)kind);
    return lx && *lx ? lx : "?";
}

void sl_offset_to_line_col(const char *source, size_t len, uint32_t offset, uint32_t *line, uint32_t *col) {
    line_col lc = line_col_of(source, len, offset);
    if (line) *line = lc.line;
    if (col) *col = lc.col;
}

uint32_t sl_line_col_to_offset(const char *source, size_t len, uint32_t line, uint32_t col) {
    size_t i = 0;
    for (uint32_t l = 1; l < line && i < len; ) {
        if (source[i++] == '\n') l++;
    }
    for (uint32_t c = 1; c < col && i < len && source[i] != '\n'; c++) {
        unsigned char b = (unsigned char)source[i];
        i += b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    }
    return (uint32_t)(i < len ? i : len);
}
