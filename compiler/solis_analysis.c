#include "solis_analysis.h"

#include "check.h"
#include "compile_internal.h"
#include "module.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct sl_analysis {
    arena         arena;
    sl_interner  *in;
    diag_vec      diags;
    loaded       *l;
    decls        *d;
    check_record  rec;
    int           checked;
    const char   *path;
    sl_diagnostic *diagnostics;
    int            ndiagnostics;
    sl_completion *comp;
    int            ncomp, compcap;
};

static char *dup_str(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

sl_analysis *sl_analyze(const char *path, const sl_compile_options *options) {
    sl_analysis *an = (sl_analysis *)calloc(1, sizeof *an);
    if (!an) return NULL;
    arena_init(&an->arena);
    an->in = intern_new(&an->arena);
    an->path = arena_strdup(&an->arena, path);

    sl_compile_options none;
    memset(&none, 0, sizeof none);
    const sl_compile_options *opt = options ? options : &none;

    int prelude_missing;
    an->l = sl_load_modules(&an->arena, an->in, path, opt, &an->diags, &prelude_missing);
    if (prelude_missing) {
        an->diagnostics = (sl_diagnostic *)calloc(1, sizeof(sl_diagnostic));
        if (an->diagnostics) {
            an->ndiagnostics = 1;
            an->diagnostics[0].file = dup_str(opt->prelude);
            an->diagnostics[0].line = an->diagnostics[0].col = 1;
            an->diagnostics[0].end_line = an->diagnostics[0].end_col = 1;
            an->diagnostics[0].message = dup_str("cannot open the prelude file");
        }
        return an;
    }
    if (an->diags.len == 0) {
        an->d = check_modules_recorded(&an->arena, an->in, an->l, &an->diags, &an->rec);
        an->checked = 1;
    }
    sl_fill_diagnostics(an->l, &an->diags, path, &an->diagnostics, &an->ndiagnostics);
    return an;
}

void sl_analysis_free(sl_analysis *an) {
    if (!an) return;
    for (int i = 0; i < an->ndiagnostics; i++) {
        free(an->diagnostics[i].file);
        free(an->diagnostics[i].message);
        free(an->diagnostics[i].help);
    }
    free(an->diagnostics);
    free(an->comp);
    arena_free(&an->arena);
    free(an);
}

const sl_diagnostic *sl_analysis_diagnostics(const sl_analysis *an, int *count) {
    *count = an->ndiagnostics;
    return an->diagnostics;
}

int sl_analysis_checked(const sl_analysis *an) { return an->checked; }

// ---- locations ----------------------------------------------------------------

static module *module_for_file(const sl_analysis *an, const char *file) {
    if (!an->l) return NULL;
    for (int i = 0; i < an->l->modules.len; i++)
        if (strcmp(an->l->modules.at[i]->path, file) == 0) return an->l->modules.at[i];
    return NULL;
}

static void make_location(const sl_analysis *an, const char *module_name, span sp, sl_location *out) {
    memset(out, 0, sizeof *out);
    module *m = module_name && an->l ? module_find(an->l, module_name) : NULL;
    out->file = m ? m->path : "<host>";
    out->start = sp.start;
    out->end = sp.end;
    if (m) {
        line_col a = line_col_of(m->src, m->src_len, sp.start);
        line_col b = line_col_of(m->src, m->src_len, sp.end);
        out->line = a.line; out->col = a.col;
        out->end_line = b.line; out->end_col = b.col;
    } else {
        out->line = out->col = out->end_line = out->end_col = 1;
    }
}

static sl_entity_kind entity_kind(ref_kind k) {
    switch (k) {
        case REF_LOCAL: return SL_ENT_LOCAL;
        case REF_PARAM: return SL_ENT_PARAM;
        case REF_FUNC: return SL_ENT_FUNC;
        case REF_CONST: return SL_ENT_CONST;
        case REF_TYPE: return SL_ENT_TYPE;
        case REF_FIELD: return SL_ENT_FIELD;
        case REF_METHOD: return SL_ENT_METHOD;
        case REF_VARIANT: return SL_ENT_VARIANT;
        case REF_MODULE: return SL_ENT_MODULE;
        case REF_BUILTIN: return SL_ENT_BUILTIN;
    }
    return SL_ENT_BUILTIN;
}

static const char *type_text(sl_analysis *an, ty *t) {
    if (!t || t->kind == TK_ERROR) return "";
    return ty_show(&an->arena, t);
}

static void fill_entity(sl_analysis *an, const ref_info *r, sl_entity *out) {
    memset(out, 0, sizeof *out);
    out->kind = entity_kind(r->kind);
    out->name = r->name ? r->name : "";
    out->type = type_text(an, r->t);
    out->detail = r->detail ? r->detail : "";
    out->has_definition = r->has_def;
    make_location(an, r->module, r->use, &out->use);
    if (r->has_def) make_location(an, r->def_module, r->def, &out->definition);
}

int sl_analysis_entity_at(const sl_analysis *an, const char *file, uint32_t offset, sl_entity *out) {
    module *m = module_for_file(an, file);
    if (!m || !an->checked) return 0;
    const ref_info *best = NULL;
    for (int i = 0; i < an->rec.refs.len; i++) {
        const ref_info *r = &an->rec.refs.at[i];
        if (strcmp(r->module, m->name) != 0) continue;
        if (offset < r->use.start || offset > r->use.end) continue;
        if (!best || r->use.end - r->use.start < best->use.end - best->use.start) best = r;
    }
    if (!best) return 0;
    fill_entity((sl_analysis *)an, best, out);
    return 1;
}

typedef struct { const char *file; uint32_t start, end; sl_location loc; } found_loc;

static int loc_cmp(const void *x, const void *y) {
    const found_loc *a = (const found_loc *)x, *b = (const found_loc *)y;
    int c = strcmp(a->file, b->file);
    if (c) return c;
    if (a->start != b->start) return a->start < b->start ? -1 : 1;
    return 0;
}

int sl_analysis_references(const sl_analysis *an, const sl_entity *e, sl_location *out, int max) {
    if (!an->checked) return 0;
    int cap = 64, n = 0;
    found_loc *all = (found_loc *)malloc((size_t)cap * sizeof *all);
    if (!all) return 0;
    for (int i = 0; i < an->rec.refs.len; i++) {
        const ref_info *r = &an->rec.refs.at[i];
        bool same;
        if (e->has_definition) {
            if (!r->has_def) continue;
            sl_location d;
            make_location(an, r->def_module, r->def, &d);
            same = strcmp(d.file, e->definition.file) == 0 && d.start == e->definition.start && d.end == e->definition.end;
        } else {
            same = !r->has_def && entity_kind(r->kind) == e->kind && strcmp(r->name ? r->name : "", e->name) == 0;
        }
        if (!same) continue;
        found_loc f;
        make_location(an, r->module, r->use, &f.loc);
        f.file = f.loc.file; f.start = f.loc.start; f.end = f.loc.end;
        // the checker can visit one source position more than once (generic bodies)
        bool dup = false;
        for (int k = 0; k < n; k++)
            if (all[k].start == f.start && all[k].end == f.end && strcmp(all[k].file, f.file) == 0) { dup = true; break; }
        if (dup) continue;
        if (n == cap) {
            cap *= 2;
            all = (found_loc *)realloc(all, (size_t)cap * sizeof *all);
            if (!all) return 0;
        }
        all[n++] = f;
    }
    qsort(all, (size_t)n, sizeof *all, loc_cmp);
    for (int i = 0; i < n && i < max; i++) out[i] = all[i].loc;
    free(all);
    return n;
}

// ---- completion ------------------------------------------------------------------

static void add_completion(sl_analysis *an, const char *prefix, size_t plen, const char *label,
                           sl_entity_kind kind, const char *detail) {
    if (strlen(label) < plen || strncmp(label, prefix, plen) != 0) return;
    for (int i = 0; i < an->ncomp; i++)           // inner names shadow outer ones
        if (strcmp(an->comp[i].label, label) == 0) {
            an->comp[i].kind = kind;
            an->comp[i].detail = detail;
            return;
        }
    if (an->ncomp == an->compcap) {
        an->compcap = an->compcap ? an->compcap * 2 : 64;
        an->comp = (sl_completion *)realloc(an->comp, (size_t)an->compcap * sizeof(sl_completion));
        if (!an->comp) { an->ncomp = an->compcap = 0; return; }
    }
    an->comp[an->ncomp].label = label;
    an->comp[an->ncomp].kind = kind;
    an->comp[an->ncomp].detail = detail;
    an->ncomp++;
}

static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static const char *func_detail(sl_analysis *an, const char *name, const fn_sig *sig) {
    return sig_describe(&an->arena, name, sig);
}

// Members of a value of type `t`.
static void complete_members(sl_analysis *an, ty *t, const char *prefix, size_t plen) {
    if (!t) return;
    if (t->kind == TK_NAMED) {
        struct_info *si = (struct_info *)map_get(&an->d->structs, t->name);
        if (!si) return;
        for (int i = 0; i < si->fields.len; i++)
            add_completion(an, prefix, plen, si->fields.at[i].name, SL_ENT_FIELD,
                           arena_printf(&an->arena, "%s: %s", si->fields.at[i].name, ty_show(&an->arena, si->fields.at[i].t)));
        for (int i = 0; i < si->methods.len; i++)
            if (!si->methods.at[i]->is_static)
                add_completion(an, prefix, plen, si->methods.at[i]->name, SL_ENT_METHOD,
                               func_detail(an, si->methods.at[i]->name, si->methods.at[i]));
    } else if (t->kind == TK_ARRAY || t->kind == TK_MAP || t->kind == TK_STR) {
        add_completion(an, prefix, plen, "len", SL_ENT_BUILTIN, "builtin len(): int");
    }
}

// Items declared in module `mname`, listed by their qualified-name prefix.
static void complete_module_items(sl_analysis *an, const char *mname, const char *prefix, size_t plen, bool from_outside) {
    size_t mn = strlen(mname);
    #define EACH(MAP, BODY) \
        for (int i = 0; i < (MAP).cap; i++) { \
            const char *key = (MAP).slots[i].key; \
            if (!key || strncmp(key, mname, mn) != 0 || key[mn] != '.') continue; \
            const char *nm = key + mn + 1; \
            if (strchr(nm, '.') || strchr(nm, '[')) continue; \
            void *val = (MAP).slots[i].val; (void)val; BODY \
        }
    EACH(an->d->funcs, { add_completion(an, prefix, plen, nm, SL_ENT_FUNC, func_detail(an, nm, (fn_sig *)val)); })
    EACH(an->d->consts, { add_completion(an, prefix, plen, nm, SL_ENT_CONST, arena_printf(&an->arena, "%s: %s", nm, ty_show(&an->arena, (ty *)val))); })
    EACH(an->d->structs, { add_completion(an, prefix, plen, nm, SL_ENT_TYPE, arena_printf(&an->arena, "struct %s", nm)); })
    EACH(an->d->enums, { add_completion(an, prefix, plen, nm, SL_ENT_TYPE, arena_printf(&an->arena, "enum %s", nm)); })
    #undef EACH
    (void)from_outside;
}

static const char *const KEYWORDS[] = {
    "func", "let", "var", "if", "else", "while", "for", "in", "switch", "case", "default", "return",
    "break", "continue", "struct", "enum", "interface", "static", "extern", "import", "arena", "weak",
    "mut", "this", "true", "false", "null", NULL
};
static const char *const BUILTINS[] = {
    "print", "len", "push", "pop", "sort", "reverse", "indexOf", "contains", "removeAt", "insertAt",
    "removeKey", "keys", "values", "sqrt", "abs", NULL
};

// The type of the name `n` as seen at `offset` in module `m`, or NULL. *module_name is
// set when the name is a module, *type_q when it is a struct or enum.
static ty *type_of_name(sl_analysis *an, module *m, const char *n, uint32_t offset,
                        const char **module_name, const char **type_q) {
    *module_name = NULL;
    *type_q = NULL;
    for (int i = an->rec.scopes.len - 1; i >= 0; i--) {
        const scope_info *s = &an->rec.scopes.at[i];
        if (strcmp(s->module, m->name) == 0 && strcmp(s->name, n) == 0 && s->from <= offset && offset < s->to)
            return s->t;
    }
    const char *q = arena_printf(&an->arena, "%s.%s", m->name, n);
    ty *c = (ty *)map_get(&an->d->consts, q);
    if (c) return c;
    if (map_has(&an->d->structs, q) || map_has(&an->d->enums, q)) { *type_q = q; return NULL; }
    for (int i = 0; i < m->imports.len; i++)
        if (strcmp(m->imports.at[i], n) == 0) { *module_name = m->imports.at[i]; return NULL; }
    return NULL;
}

const sl_completion *sl_analysis_complete(sl_analysis *an, const char *file, uint32_t offset, int *count) {
    an->ncomp = 0;
    *count = 0;
    module *m = module_for_file(an, file);
    if (!m || !an->checked) return NULL;
    const char *src = m->src;
    size_t len = m->src_len;
    if (offset > len) offset = (uint32_t)len;

    uint32_t ps = offset;
    while (ps > 0 && ident_char(src[ps - 1])) ps--;
    const char *prefix = src + ps;
    size_t plen = offset - ps;

    // Member access: a '.' just before the word being typed.
    uint32_t dot = ps;
    while (dot > 0 && (src[dot - 1] == ' ' || src[dot - 1] == '\t')) dot--;
    if (dot > 0 && src[dot - 1] == '.') {
        dot--;
        // the dotted chain of names before it: a.b.c
        uint32_t end = dot, start = end;
        for (;;) {
            while (start > 0 && ident_char(src[start - 1])) start--;
            if (start > 0 && src[start - 1] == '.') { start--; continue; }
            break;
        }
        if (start == end) goto done;
        char chain[256];
        size_t cn = end - start;
        if (cn >= sizeof chain) goto done;
        memcpy(chain, src + start, cn);
        chain[cn] = 0;

        ty *t = NULL;
        const char *mod_name = NULL, *type_q = NULL;
        bool first = true;
        for (char *seg = chain, *next; seg && *seg; seg = next) {
            next = strchr(seg, '.');
            if (next) *next++ = 0;
            if (first) {
                t = type_of_name(an, m, seg, offset, &mod_name, &type_q);
                first = false;
            } else if (mod_name) {
                // module.Type or module.value
                const char *q = arena_printf(&an->arena, "%s.%s", mod_name, seg);
                ty *c = (ty *)map_get(&an->d->consts, q);
                mod_name = NULL;
                t = c;
                if (!c && (map_has(&an->d->structs, q) || map_has(&an->d->enums, q))) type_q = q;
            } else if (t && t->kind == TK_NAMED) {
                struct_info *si = (struct_info *)map_get(&an->d->structs, t->name);
                ty *next = NULL;
                for (int i = 0; si && i < si->fields.len; i++)
                    if (strcmp(si->fields.at[i].name, seg) == 0) next = si->fields.at[i].t;
                t = next;
            } else {
                t = NULL;
            }
        }
        if (mod_name) {
            complete_module_items(an, mod_name, prefix, plen, true);
        } else if (type_q) {
            enum_info *ei = (enum_info *)map_get(&an->d->enums, type_q);
            if (ei)
                for (int i = 0; i < ei->variants.len; i++)
                    add_completion(an, prefix, plen, ei->variants.at[i].name, SL_ENT_VARIANT,
                                   arena_printf(&an->arena, "%s.%s", strchr(type_q, '.') + 1, ei->variants.at[i].name));
            struct_info *si = (struct_info *)map_get(&an->d->structs, type_q);
            if (si)
                for (int i = 0; i < si->methods.len; i++)
                    if (si->methods.at[i]->is_static)
                        add_completion(an, prefix, plen, si->methods.at[i]->name, SL_ENT_METHOD,
                                       func_detail(an, si->methods.at[i]->name, si->methods.at[i]));
        } else {
            complete_members(an, t, prefix, plen);
        }
        goto done;
    }

    // Names in scope: locals first (innermost wins), then the module, the prelude,
    // imported modules, built-ins and keywords.
    for (int i = 0; i < an->rec.scopes.len; i++) {
        const scope_info *s = &an->rec.scopes.at[i];
        if (strcmp(s->module, m->name) != 0 || offset < s->from || offset >= s->to) continue;
        add_completion(an, prefix, plen, s->name, s->kind == REF_PARAM ? SL_ENT_PARAM : SL_ENT_LOCAL,
                       arena_printf(&an->arena, "%s: %s", s->name, ty_show(&an->arena, s->t)));
    }
    complete_module_items(an, m->name, prefix, plen, false);
    if (an->l->prelude && strcmp(an->l->prelude, m->name) != 0)
        complete_module_items(an, an->l->prelude, prefix, plen, false);
    for (int i = 0; i < m->imports.len; i++)
        add_completion(an, prefix, plen, m->imports.at[i], SL_ENT_MODULE,
                       arena_printf(&an->arena, "module %s", m->imports.at[i]));
    for (int i = 0; BUILTINS[i]; i++) add_completion(an, prefix, plen, BUILTINS[i], SL_ENT_BUILTIN, "builtin");
    for (int i = 0; KEYWORDS[i]; i++) add_completion(an, prefix, plen, KEYWORDS[i], SL_ENT_KEYWORD, "keyword");

done:
    *count = an->ncomp;
    return an->comp;
}

// ---- helpers for clients -----------------------------------------------------------

char *sl_completion_source(const char *source, size_t len, uint32_t offset, size_t *out_len) {
    if (offset > len) offset = (uint32_t)len;
    uint32_t ps = offset;
    while (ps > 0 && ident_char(source[ps - 1])) ps--;
    uint32_t dot = ps;
    while (dot > 0 && (source[dot - 1] == ' ' || source[dot - 1] == '\t')) dot--;
    if (dot == 0 || source[dot - 1] != '.') return NULL;      // not after a dot: nothing to patch

    // What follows on the line decides whether a statement needs closing.
    size_t eol = offset;
    while (eol < len && source[eol] != '\n') eol++;
    bool blank_rest = true;
    for (size_t i = offset; i < eol; i++)
        if (source[i] != ' ' && source[i] != '\t' && source[i] != '\r') { blank_rest = false; break; }
    bool need_name = offset == ps;     // nothing typed after the dot
    const char *name = "__";
    const char *tail = blank_rest ? ";" : "";
    size_t add = (need_name ? strlen(name) : 0) + strlen(tail);
    if (add == 0) return NULL;

    char *out = (char *)malloc(len + add + 1);
    if (!out) return NULL;
    memcpy(out, source, offset);
    size_t w = offset;
    if (need_name) { memcpy(out + w, name, strlen(name)); w += strlen(name); }
    memcpy(out + w, tail, strlen(tail)); w += strlen(tail);
    memcpy(out + w, source + offset, len - offset);
    w += len - offset;
    out[w] = 0;
    if (out_len) *out_len = w;
    return out;
}
