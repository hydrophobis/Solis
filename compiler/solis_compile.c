#include "solis_compile.h"

#include "check.h"
#include "compile_internal.h"
#include "emit.h"
#include "gen.h"
#include "module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Where a diagnostic's module lives: its source text and the path to show.
// A diagnostic with no module (single-file use) belongs to the entry.
static void diag_origin(const loaded *l, const diag *d, const char *path,
                        const char **src, size_t *len, const char **where) {
    *src = "";
    *len = 0;
    *where = path;
    if (!l) return;
    module *m = module_find(l, d->module ? d->module : l->root);
    if (m) {
        *src = m->src;
        *len = m->src_len;
        *where = m->path;
    }
}

// tmpfile() instead of open_memstream() so this builds on Windows
static char *render(const loaded *l, const diag_vec *diags, const char *path) {
    FILE *f = tmpfile();
    if (!f) return NULL;
    for (int i = 0; i < diags->len; i++) {
        const diag *d = &diags->at[i];
        const char *src, *where;
        size_t len;
        diag_origin(l, d, path, &src, &len, &where);
        fputc('\n', f);
        diag_render(src, len, where, d, f);
    }
    long n = ftell(f);
    char *text = n >= 0 ? (char *)malloc((size_t)n + 1) : NULL;
    if (text) {
        rewind(f);
        size_t got = fread(text, 1, (size_t)n, f);
        text[got] = 0;
    }
    fclose(f);
    return text;
}

static char *copy_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

void sl_fill_diagnostics(const loaded *l, const diag_vec *diags, const char *path,
                         sl_diagnostic **out, int *count) {
    *out = NULL;
    *count = 0;
    if (diags->len == 0) return;
    *out = (sl_diagnostic *)calloc((size_t)diags->len, sizeof(sl_diagnostic));
    if (!*out) return;
    *count = diags->len;
    for (int i = 0; i < diags->len; i++) {
        const diag *d = &diags->at[i];
        const char *src, *where;
        size_t len;
        diag_origin(l, d, path, &src, &len, &where);
        line_col a = line_col_of(src, len, d->at.start);
        line_col b = line_col_of(src, len, d->at.end);
        sl_diagnostic *o = &(*out)[i];
        o->file = copy_str(where);
        o->line = a.line; o->col = a.col;
        o->end_line = b.line; o->end_col = b.col;
        o->message = copy_str(d->message);
        o->help = copy_str(d->help);
    }
}

// The caller's hooks, adapted to what the module loader expects. Whatever a
// hook returns is copied into the arena and the original freed.
typedef struct {
    const sl_compile_options *opt;
} hook_ctx;

static const char *hook_read(void *user, arena *a, const char *path, size_t *len) {
    const sl_compile_options *o = ((hook_ctx *)user)->opt;
    size_t n = 0;
    char *text = o->read(o->user, path, &n);
    if (!text) return NULL;
    char *copy = (char *)arena_alloc(a, n + 1);
    memcpy(copy, text, n);
    copy[n] = 0;
    free(text);
    *len = n;
    return copy;
}

static bool hook_exists(void *user, const char *path) {
    const sl_compile_options *o = ((hook_ctx *)user)->opt;
    return o->exists(o->user, path) != 0;
}

static const char *hook_resolve(void *user, arena *a, const char *from, const char *name) {
    const sl_compile_options *o = ((hook_ctx *)user)->opt;
    char *path = o->resolve(o->user, from, name);
    if (!path) return NULL;
    char *copy = arena_strdup(a, path);
    free(path);
    return copy;
}

loaded *sl_load_modules(arena *a, sl_interner *in, const char *path, const sl_compile_options *opt,
                        diag_vec *diags, int *prelude_missing) {
    *prelude_missing = 0;
    const char *prelude = opt->prelude;
    if (prelude && *prelude) {  // an explicit prelude has to exist; the automatic one doesn't
        bool found;
        if (opt->exists) {
            found = opt->exists(opt->user, prelude) != 0;
        } else if (opt->read) {
            size_t n;
            char *t = opt->read(opt->user, prelude, &n);
            found = t != NULL;
            free(t);
        } else {
            FILE *pf = fopen(prelude, "rb");
            found = pf != NULL;
            if (pf) fclose(pf);
        }
        if (!found) {
            *prelude_missing = 1;
            return NULL;
        }
    }

    hook_ctx ctx = { opt };
    module_io io;
    memset(&io, 0, sizeof io);
    io.user = &ctx;
    if (opt->read) io.read = hook_read;
    if (opt->exists) io.exists = hook_exists;
    if (opt->resolve) io.resolve = hook_resolve;
    bool hooked = opt->read || opt->exists || opt->resolve;

    return module_load_io(a, in, path, opt->entry_source, prelude, opt->host, hooked ? &io : NULL, diags);
}

static int compile(const char *path, const sl_compile_options *opt, sl_compiled *out, int emit) {
    memset(out, 0, sizeof *out);

    arena a;
    arena_init(&a);
    sl_interner *in = intern_new(&a);
    diag_vec diags;
    memset(&diags, 0, sizeof diags);

    int prelude_missing;
    loaded *l = sl_load_modules(&a, in, path, opt, &diags, &prelude_missing);
    if (prelude_missing) {
        const char *msg = "cannot open the prelude file";
        out->errors = copy_str(msg);
        out->error_count = 1;
        out->diagnostics = (sl_diagnostic *)calloc(1, sizeof(sl_diagnostic));
        if (out->diagnostics) {
            out->diagnostic_count = 1;
            out->diagnostics[0].file = copy_str(opt->prelude);
            out->diagnostics[0].line = out->diagnostics[0].col = 1;
            out->diagnostics[0].end_line = out->diagnostics[0].end_col = 1;
            out->diagnostics[0].message = copy_str(msg);
        }
        arena_free(&a);
        return 1;
    }

    if (diags.len == 0) {
        decls *d = check_modules(&a, in, l, &diags);
        if (diags.len == 0 && emit) {
            bc_program *prog = generate(&a, in, l, d, &diags);
            if (prog && diags.len == 0) out->image = bc_serialize(prog, &out->image_len);
        }
    }

    if (diags.len) {
        out->error_count = diags.len;
        out->errors = render(l, &diags, path);
        sl_fill_diagnostics(l, &diags, path, &out->diagnostics, &out->diagnostic_count);
        free(out->image);
        out->image = NULL;
        out->image_len = 0;
    }
    arena_free(&a);
    return diags.len ? 1 : 0;
}

int sl_compile_with(const char *path, const sl_compile_options *options, sl_compiled *out) {
    sl_compile_options none;
    memset(&none, 0, sizeof none);
    return compile(path, options ? options : &none, out, options ? !options->check_only : 1);
}

static int simple(const char *path, const char *prelude, const char *host, sl_compiled *out, int emit) {
    sl_compile_options o;
    memset(&o, 0, sizeof o);
    o.prelude = prelude;
    o.host = host;
    return compile(path, &o, out, emit);
}

int sl_compile_file(const char *path, const char *prelude, sl_compiled *out) {
    return simple(path, prelude, NULL, out, 1);
}

int sl_compile_file_with_host(const char *path, const char *prelude, const char *host, sl_compiled *out) {
    return simple(path, prelude, host, out, 1);
}

int sl_check_file(const char *path, const char *prelude, sl_compiled *out) {
    return simple(path, prelude, NULL, out, 0);
}

int sl_check_file_with_host(const char *path, const char *prelude, const char *host, sl_compiled *out) {
    return simple(path, prelude, host, out, 0);
}

void sl_compiled_free(sl_compiled *c) {
    for (int i = 0; i < c->diagnostic_count; i++) {
        free(c->diagnostics[i].file);
        free(c->diagnostics[i].message);
        free(c->diagnostics[i].help);
    }
    free(c->diagnostics);
    free(c->image);
    free(c->errors);
    memset(c, 0, sizeof *c);
}
