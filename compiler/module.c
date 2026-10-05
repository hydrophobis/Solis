#include "module.h"

#include "parse.h"

#include <stdio.h>

module *module_find(const loaded *l, const char *name) {
    for (int i = 0; i < l->modules.len; i++)
        if (strcmp(l->modules.at[i]->name, name) == 0) return l->modules.at[i];
    return NULL;
}

// Keep sorted by name: function numbering follows module order, and must
// not depend on the order things happened to load.
static void insert_sorted(arena *a, module_vec *v, module *m) {
    vec_push(a, v, m);
    int i = v->len - 1;
    while (i > 0 && strcmp(v->at[i - 1]->name, m->name) > 0) {
        v->at[i] = v->at[i - 1];
        i--;
    }
    v->at[i] = m;
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static char *read_file(arena *a, const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)arena_alloc(a, (size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    *len_out = got;
    return buf;
}

typedef struct {
    arena       *a;
    sl_interner *in;
    loaded      *out;
    diag_vec    *diags;
    const char  *dir;
    // The import stack, so a cycle can be named as a chain.
    VEC(const char *) stack;
} loader;

static void tagged(loader *L, const char *module_name, span at,
                   const char *help, const char *msg) {
    diag_add(L->a, L->diags, at, msg);
    L->diags->at[L->diags->len - 1].help = help;
    L->diags->at[L->diags->len - 1].module = module_name;
}

static void load_one(loader *L, const char *name, const char *path);

// `dir/name.sl`
static const char *child_path(loader *L, const char *name) {
    if (!L->dir || !*L->dir) return arena_printf(L->a, "%s.sl", name);
    return arena_printf(L->a, "%s/%s.sl", L->dir, name);
}

static void load_one(loader *L, const char *name, const char *path) {
    if (module_find(L->out, name)) return;      // already loaded

    // Built-ins are compiled in, but a file of the same name beside the
    // source still wins, so the stdlib stays overridable.
    const char *src = NULL;
    size_t src_len = 0;
    const char *embedded = stdlib_source(name);
    if (embedded && !file_exists(path)) {
        src = embedded;
        src_len = strlen(embedded);
    } else {
        char *buf = read_file(L->a, path, &src_len);
        if (!buf) {
            // Blamed on the importer, which is where the reader can act on it.
            const char *blame = L->stack.len ? L->stack.at[L->stack.len - 1] : name;
            tagged(L, blame, span_make(0, 0), "the file could not be opened",
                   arena_printf(L->a, "cannot read module `%s` (%s)", name, path));
            return;
        }
        src = buf;
    }

    diag_vec parse_diags;
    memset(&parse_diags, 0, sizeof parse_diags);
    program *prog = parse(L->a, L->in, src, src_len, &parse_diags);
    for (int i = 0; i < parse_diags.len; i++) {
        diag d = parse_diags.at[i];
        d.module = name;
        vec_push(L->a, L->diags, d);
    }

    module *m = NEW(L->a, module);
    m->name = name;
    m->path = path;
    m->src = src;
    m->src_len = src_len;
    m->prog = prog;

    // Collect imports before recursing, and reject duplicates, self-imports
    // and cycles.
    for (int i = 0; i < prog->items.len; i++) {
        if (prog->items.at[i].kind != IT_IMPORT) continue;
        const char *imp = prog->items.at[i].as.import.name;
        span at = prog->items.at[i].as.import.at;

        if (strcmp(imp, name) == 0) {
            tagged(L, name, at, NULL,
                   arena_printf(L->a, "module `%s` imports itself", name));
            continue;
        }

        bool dup = false;
        for (int k = 0; k < m->imports.len; k++)
            if (strcmp(m->imports.at[k], imp) == 0) { dup = true; break; }
        if (dup) {
            tagged(L, name, at, NULL,
                   arena_printf(L->a, "`%s` is imported more than once", imp));
            continue;
        }

        int pos = -1;
        for (int k = 0; k < L->stack.len; k++)
            if (strcmp(L->stack.at[k], imp) == 0) { pos = k; break; }
        if (pos >= 0) {
            // Report the cycle as a chain, not just "cycle detected".
            const char *chain = L->stack.at[pos];
            for (int k = pos + 1; k < L->stack.len; k++)
                chain = arena_printf(L->a, "%s -> %s", chain, L->stack.at[k]);
            chain = arena_printf(L->a, "%s -> %s -> %s", chain, name, imp);
            tagged(L, name, at, "modules cannot import each other, even indirectly",
                   arena_printf(L->a, "import cycle: %s", chain));
            continue;
        }

        vec_push(L->a, &m->imports, imp);
    }

    insert_sorted(L->a, &L->out->modules, m);

    vec_push(L->a, &L->stack, name);
    for (int i = 0; i < m->imports.len; i++) {
        const char *imp = m->imports.at[i];
        load_one(L, imp, child_path(L, imp));
    }
    L->stack.len--;
}

// The module name is the file stem; the search directory is its parent.
static void split_path(arena *a, const char *path, const char **dir, const char **stem) {
    size_t n = strlen(path);
    size_t cut = (size_t)-1;
    for (size_t i = 0; i < n; i++)
        if (path[i] == '/' || path[i] == '\\') cut = i;

    if (cut == (size_t)-1) {
        *dir = "";
        *stem = path;
    } else {
        *dir = arena_strndup(a, path, cut);
        *stem = path + cut + 1;
    }

    // Drop a trailing extension.
    const char *s = *stem;
    size_t sn = strlen(s);
    for (size_t i = sn; i > 0; i--) {
        if (s[i - 1] == '.') {
            *stem = arena_strndup(a, s, i - 1);
            break;
        }
    }
    if (!**stem) *stem = "main";
}

loaded *module_load(arena *a, sl_interner *in, const char *entry_path,
                    const char *prelude_path, diag_vec *diags) {
    loaded *out = NEW(a, loaded);

    const char *dir;
    const char *stem;
    split_path(a, entry_path, &dir, &stem);
    out->root = intern_z(in, stem);

    loader L;
    memset(&L, 0, sizeof L);
    L.a = a;
    L.in = in;
    L.out = out;
    L.diags = diags;
    L.dir = dir;

    load_one(&L, out->root, entry_path);

    // The prelude, if there is one. Unasked-for, a file named prelude.sl
    // beside the entry is picked up, which is how a host drops one into a
    // plugin directory and every script there inherits its declarations.
    // "" disables that; anything else names a file explicitly.
    const char *path = prelude_path;
    if (!path) {
        path = child_path(&L, "prelude");
        if (!file_exists(path)) path = "";
    }
    if (*path) {
        const char *pdir;
        const char *pstem;
        split_path(a, path, &pdir, &pstem);
        pstem = intern_z(in, pstem);
        if (strcmp(pstem, out->root) != 0) {
            // Loaded from its own directory, so a prelude may import modules
            // sitting next to itself rather than next to the script.
            const char *saved = L.dir;
            L.dir = pdir;
            load_one(&L, pstem, path);
            L.dir = saved;
            out->prelude = pstem;

            // Every module imports it, whether it said so or not. That is
            // what makes it ambient, and it also puts it first in the
            // initialisation order, so a module-level `let` elsewhere can
            // call into the prelude and see its globals already set.
            for (int i = 0; i < out->modules.len; i++) {
                module *m = out->modules.at[i];
                if (strcmp(m->name, pstem) == 0) continue;
                bool already = false;
                for (int k = 0; k < m->imports.len; k++)
                    if (strcmp(m->imports.at[k], pstem) == 0) already = true;
                if (!already) vec_push(a, &m->imports, pstem);
            }
        }
    }
    return out;
}
