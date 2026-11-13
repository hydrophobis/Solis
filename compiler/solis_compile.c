#include "solis_compile.h"

#include "check.h"
#include "emit.h"
#include "gen.h"
#include "module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// tmpfile() instead of open_memstream() so this builds on Windows
static char *render(const loaded *l, const diag_vec *diags, const char *path) {
    FILE *f = tmpfile();
    if (!f) return NULL;
    for (int i = 0; i < diags->len; i++) {
        const diag *d = &diags->at[i];
        const char *src = "";
        size_t len = 0;
        const char *where = path;
        if (d->module && l) {
            module *m = module_find(l, d->module);
            if (m) {
                src = m->src;
                len = m->src_len;
                where = m->path;
            }
        }
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

static int compile(const char *path, const char *prelude, const char *host, sl_compiled *out, int emit) {
    memset(out, 0, sizeof *out);

    arena a;
    arena_init(&a);
    sl_interner *in = intern_new(&a);
    diag_vec diags;
    memset(&diags, 0, sizeof diags);

    if (prelude && *prelude) {  // an explicit prelude has to exist; the automatic one doesn't
        FILE *pf = fopen(prelude, "rb");
        if (!pf) {
            const char *msg = "cannot open the prelude file";
            out->errors = (char *)malloc(strlen(msg) + 1);
            if (out->errors) strcpy(out->errors, msg);
            out->error_count = 1;
            arena_free(&a);
            return 1;
        }
        fclose(pf);
    }

    loaded *l = module_load(&a, in, path, prelude, host, &diags);
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
        free(out->image);
        out->image = NULL;
        out->image_len = 0;
    }
    arena_free(&a);
    return diags.len ? 1 : 0;
}

int sl_compile_file(const char *path, const char *prelude, sl_compiled *out) {
    return compile(path, prelude, NULL, out, 1);
}

int sl_compile_file_with_host(const char *path, const char *prelude, const char *host, sl_compiled *out) {
    return compile(path, prelude, host, out, 1);
}

int sl_check_file(const char *path, const char *prelude, sl_compiled *out) {
    return compile(path, prelude, NULL, out, 0);
}

int sl_check_file_with_host(const char *path, const char *prelude, const char *host, sl_compiled *out) {
    return compile(path, prelude, host, out, 0);
}

void sl_compiled_free(sl_compiled *c) {
    free(c->image);
    free(c->errors);
    memset(c, 0, sizeof *c);
}
