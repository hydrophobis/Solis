// sl: the project tool for Solis.
//
// This is an ordinary host. It links the compiler and the runtime and uses
// only their public headers (solis.h and solis_compile.h), the same way an
// application embedding Solis would. Anything it needs that those headers do
// not offer is a gap in the embedding API, not something to reach around.
//
//   sl new <name>             create a project
//   sl run [file] [args...]   run a file, or the project's entry point
//   sl check [file...]        type check the entry point and the tests
//   sl outline <file>         list the declarations in a file
//   sl lsp                    run as a language server on standard input and output
//   sl info <file> <l> <c>    what the name at a position is, and where it is declared
//   sl refs <file> <l> <c>    every use of that name
//   sl complete <file> <l> <c>  what can be typed there
//   sl test [filter]          run every *_test.sl under src/
//
// A project is a directory with a solis.toml at its root.

#include "solis.h"
#include "solis_analysis.h"
#include "solis_compile.h"
#include "solis_syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#define GETCWD _getcwd
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0777)
#define GETCWD getcwd
#endif

#define VERSION "0.1.0"

int lsp_main(void);

// ---- small helpers -----------------------------------------------------------

static char *dup_str(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) { fprintf(stderr, "sl: out of memory\n"); exit(1); }
    memcpy(p, s, n + 1);
    return p;
}

static char *join(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    char *p = (char *)malloc(na + nb + 2);
    if (!p) { fprintf(stderr, "sl: out of memory\n"); exit(1); }
    memcpy(p, a, na);
    if (na && a[na - 1] != '/' && a[na - 1] != '\\') p[na++] = '/';
    memcpy(p + na, b, nb + 1);
    return p;
}

static bool exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (1L << 30)) { fclose(f); return NULL; }   // a directory reports a huge size
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static bool write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

// ---- file lists --------------------------------------------------------------

typedef struct { char **at; int len, cap; } paths;

static void paths_add(paths *p, char *path) {
    if (p->len == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 16;
        p->at = (char **)realloc(p->at, (size_t)p->cap * sizeof(char *));
        if (!p->at) { fprintf(stderr, "sl: out of memory\n"); exit(1); }
    }
    p->at[p->len++] = path;
}

static int path_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// Every file under `dir` whose name ends in `suffix`, in a stable order.
static void walk(const char *dir, const char *suffix, paths *out) {
#ifdef _WIN32
    char *pattern = join(dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char *full = join(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { walk(full, suffix, out); free(full); }
        else if (ends_with(full, suffix)) paths_add(out, full);
        else free(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char *full = join(dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) { free(full); continue; }
        if (S_ISDIR(st.st_mode)) { walk(full, suffix, out); free(full); }
        else if (ends_with(full, suffix)) paths_add(out, full);
        else free(full);
    }
    closedir(d);
#endif
    if (out->len > 1) qsort(out->at, (size_t)out->len, sizeof(char *), path_cmp);
}

// ---- projects -----------------------------------------------------------------

typedef struct {
    char *root;        // directory holding solis.toml
    char *name;
    char *entry;       // relative to root
} project;

// The first key = "value" line with this key, or NULL.
static char *manifest_value(const char *text, const char *key) {
    size_t klen = strlen(key);
    for (const char *p = text; *p;) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        const char *q = p;
        while (q < p + len && (*q == ' ' || *q == '\t')) q++;
        if ((size_t)(p + len - q) > klen && strncmp(q, key, klen) == 0) {
            const char *r = q + klen;
            while (r < p + len && (*r == ' ' || *r == '\t')) r++;
            if (r < p + len && *r == '=') {
                r++;
                while (r < p + len && (*r == ' ' || *r == '\t')) r++;
                if (r < p + len && *r == '"') {
                    r++;
                    const char *end = r;
                    while (end < p + len && *end != '"') end++;
                    char *v = (char *)malloc((size_t)(end - r) + 1);
                    memcpy(v, r, (size_t)(end - r));
                    v[end - r] = 0;
                    return v;
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return NULL;
}

// Walk up from the current directory looking for solis.toml.
static bool find_project(project *pr) {
    char cwd[4096];
    if (!GETCWD(cwd, sizeof cwd)) return false;
    char *dir = dup_str(cwd);
    for (;;) {
        char *manifest = join(dir, "solis.toml");
        char *text = read_all(manifest);
        free(manifest);
        if (text) {
            pr->root = dir;
            pr->name = manifest_value(text, "name");
            pr->entry = manifest_value(text, "entry");
            free(text);
            if (!pr->name) pr->name = dup_str("project");
            if (!pr->entry) pr->entry = dup_str("src/main.sl");
            return true;
        }
        char *slash = strrchr(dir, '/');
#ifdef _WIN32
        char *back = strrchr(dir, '\\');
        if (back && (!slash || back > slash)) slash = back;
#endif
        if (!slash || slash == dir) break;
        *slash = 0;
    }
    free(dir);
    return false;
}

// ---- running scripts ------------------------------------------------------------

static bool g_capture = false;       // collect print output instead of writing it
static char *g_out = NULL;
static size_t g_out_len = 0, g_out_cap = 0;
static int g_failed_assertions = 0;  // lines the test module printed as FAIL

static void out_append(const char *p, size_t n) {
    if (!g_capture) { fwrite(p, 1, n, stdout); return; }
    if (g_out_len + n + 1 > g_out_cap) {
        g_out_cap = (g_out_len + n + 1) * 2;
        g_out = (char *)realloc(g_out, g_out_cap);
        if (!g_out) { fprintf(stderr, "sl: out of memory\n"); exit(1); }
    }
    memcpy(g_out + g_out_len, p, n);
    g_out_len += n;
    g_out[g_out_len] = 0;
}

// Same output as the interpreter's print; also counts the lines std/test.sl
// writes when an assertion fails, which is how `sl test` finds failures.
static sl_value host_print(sl_vm *vm, int argc, sl_value *argv) {
    for (int i = 0; i < argc; i++) {
        if (i) out_append(" ", 1);
        sl_value s = sl_to_string(vm, argv[i]);
        size_t n;
        const char *p = sl_as_str(s, &n);
        if (i == 0 && n >= 5 && strncmp(p, "FAIL:", 5) == 0) g_failed_assertions++;
        out_append(p, n);
        sl_release(vm, s);
    }
    out_append("\n", 1);
    return sl_void();
}

// Compile and run one file. Returns 0 on success, 1 if it did not compile or
// faulted. `argv` are the script's own arguments.
static int run_file(const char *path, int argc, char **argv) {
    sl_compiled c;
    if (sl_compile_file(path, NULL, &c) != 0) {
        if (g_capture) out_append(c.errors ? c.errors : "compile error\n", c.errors ? strlen(c.errors) : 14);
        else fputs(c.errors ? c.errors : "compile error\n", stderr);
        sl_compiled_free(&c);
        return 1;
    }
    sl_vm *vm = sl_new();
    if (!vm) { sl_compiled_free(&c); return 1; }
    sl_open_std(vm);
    sl_open_os(vm, argc, argv);
    sl_register(vm, "print", host_print);
    int rc = 0;
    if (sl_load(vm, c.image, c.image_len) != SL_OK) {
        fprintf(stderr, "load error: %s\n", sl_error(vm));
        rc = 1;
    } else if (sl_run(vm) != SL_OK) {
        fflush(stdout);
        char msg[1200];
        snprintf(msg, sizeof msg, "runtime error: %s\n", sl_error(vm));
        if (g_capture) out_append(msg, strlen(msg)); else fputs(msg, stderr);
        rc = 1;
    }
    sl_free(vm);
    sl_compiled_free(&c);
    return rc;
}

// ---- commands --------------------------------------------------------------------

static int cmd_new(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: sl new <name>\n"); return 2; }
    const char *name = argv[0];
    if (exists(name)) { fprintf(stderr, "sl: `%s` already exists\n", name); return 1; }
    char *src = join(name, "src");
    if (MKDIR(name) != 0 || MKDIR(src) != 0) {
        fprintf(stderr, "sl: cannot create `%s`\n", name);
        free(src);
        return 1;
    }
    char manifest[512], main_sl[512];
    snprintf(manifest, sizeof manifest, "name = \"%s\"\nversion = \"0.1.0\"\nentry = \"src/main.sl\"\n", name);
    snprintf(main_sl, sizeof main_sl,
             "func greet(who: str): str {\n    return \"hello, ${who}\";\n}\n\n"
             "func main() {\n    print(greet(\"%s\"));\n}\n", name);
    const char *test_sl =
        "import test;\n\n"
        "func main() {\n"
        "    test.assertEqual(1 + 1, 2, \"addition\");\n"
        "}\n";
    char *p1 = join(name, "solis.toml"), *p2 = join(src, "main.sl"), *p3 = join(src, "main_test.sl");
    bool ok = write_file(p1, manifest) && write_file(p2, main_sl) && write_file(p3, test_sl);
    free(p1); free(p2); free(p3); free(src);
    if (!ok) { fprintf(stderr, "sl: cannot write the project files\n"); return 1; }
    printf("created %s\n  cd %s && sl run\n", name, name);
    return 0;
}

static int cmd_run(int argc, char **argv) {
    const char *file = NULL;
    char *owned = NULL;
    project pr;
    memset(&pr, 0, sizeof pr);
    if (argc >= 1 && ends_with(argv[0], ".sl")) {
        file = argv[0];
        argc--; argv++;
    } else {
        if (!find_project(&pr)) {
            fprintf(stderr, "sl: no solis.toml here or in any parent directory (pass a .sl file to run it directly)\n");
            return 2;
        }
        owned = join(pr.root, pr.entry);
        file = owned;
    }
    int rc = run_file(file, argc, argv);
    free(owned);
    return rc;
}

static void json_string(const char *s) {
    putchar('"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static void print_json(const sl_compiled *c, bool *first) {
    for (int i = 0; i < c->diagnostic_count; i++) {
        const sl_diagnostic *d = &c->diagnostics[i];
        printf(*first ? "\n" : ",\n");
        *first = false;
        printf("  {\"file\": "); json_string(d->file);
        printf(", \"line\": %u, \"col\": %u, \"endLine\": %u, \"endCol\": %u, \"message\": ",
               d->line, d->col, d->end_line, d->end_col);
        json_string(d->message);
        if (d->help) { printf(", \"help\": "); json_string(d->help); }
        printf("}");
    }
}

// Reads standard input to the end.
static char *read_stdin(void) {
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    size_t got;
    while ((got = fread(buf + len, 1, cap - len - 1, stdin)) > 0) {
        len += got;
        if (len + 1 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); if (!buf) return NULL; }
    }
    buf[len] = 0;
    return buf;
}

// sl check [--json] [--stdin <path>] [file...]
//   --json            print the errors as JSON, for editors and other tools
//   --stdin <path>    check the source on standard input as if it were <path>
static int cmd_check(int argc, char **argv) {
    bool json = false;
    const char *stdin_path = NULL;
    paths files;
    memset(&files, 0, sizeof files);
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = true;
        else if (strcmp(argv[i], "--stdin") == 0 && i + 1 < argc) stdin_path = argv[++i];
        else paths_add(&files, dup_str(argv[i]));
    }
    char *buffer = NULL;
    if (stdin_path) {
        buffer = read_stdin();
        if (!buffer) { fprintf(stderr, "sl: cannot read standard input\n"); return 2; }
        paths_add(&files, dup_str(stdin_path));
    } else if (files.len == 0) {
        project pr;
        if (!find_project(&pr)) {
            fprintf(stderr, "sl: no solis.toml here or in any parent directory\n");
            return 2;
        }
        // The entry point and the tests pull in every module the project uses.
        paths_add(&files, join(pr.root, pr.entry));
        char *src = join(pr.root, "src");
        walk(src, "_test.sl", &files);
        free(src);
    }
    int bad = 0;
    bool first = true;
    if (json) printf("[");
    for (int i = 0; i < files.len; i++) {
        sl_compile_options o;
        memset(&o, 0, sizeof o);
        o.check_only = 1;
        o.entry_source = stdin_path ? buffer : NULL;
        sl_compiled c;
        if (sl_compile_with(files.at[i], &o, &c) != 0) {
            if (json) print_json(&c, &first);
            else fputs(c.errors ? c.errors : "error\n", stderr);
            bad++;
        }
        sl_compiled_free(&c);
    }
    if (json) printf(first ? "]\n" : "\n]\n");
    else printf("%d file%s checked, %d with errors\n", files.len, files.len == 1 ? "" : "s", bad);
    free(buffer);
    return bad ? 1 : 0;
}

// sl outline <file>: the declarations in a file, from the syntax alone
static int cmd_outline(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: sl outline <file>\n"); return 2; }
    char *text = read_all(argv[0]);
    if (!text) { fprintf(stderr, "sl: cannot read %s\n", argv[0]); return 1; }
    size_t len = strlen(text);
    sl_syntax *syn = sl_syntax_parse(text, len);
    if (!syn) { fprintf(stderr, "sl: out of memory\n"); free(text); return 1; }
    static const char *kinds[] = { "func", "struct", "enum", "interface", "extern", "let", "import", "field", "method", "variant" };
    int n;
    const sl_symbol *sy = sl_syntax_symbols(syn, &n);
    for (int i = 0; i < n; i++) {
        uint32_t line, col;
        sl_offset_to_line_col(text, len, sy[i].name_start, &line, &col);
        printf("%s%-9s %s  (%u:%u)\n", sy[i].parent >= 0 ? "    " : "", kinds[sy[i].kind], sy[i].name, line, col);
    }
    int nd;
    const sl_diagnostic *d = sl_syntax_diagnostics(syn, &nd);
    for (int i = 0; i < nd; i++)
        fprintf(stderr, "%s:%u:%u: %s\n", argv[0], d[i].line, d[i].col, d[i].message);
    sl_syntax_free(syn);
    free(text);
    return nd ? 1 : 0;
}

// ---- questions about a position ----------------------------------------------------

static const char *entity_name(sl_entity_kind k) {
    static const char *names[] = { "local", "parameter", "function", "constant", "type", "field",
                                   "method", "variant", "module", "builtin", "keyword" };
    return names[k];
}

// Parses `<file> <line> <col>` and reads the file. Returns the byte offset.
static bool read_position(int argc, char **argv, char **text, size_t *len, uint32_t *offset) {
    if (argc < 3) return false;
    *text = read_all(argv[0]);
    if (!*text) { fprintf(stderr, "sl: cannot read %s\n", argv[0]); return false; }
    *len = strlen(*text);
    *offset = sl_line_col_to_offset(*text, *len, (uint32_t)atoi(argv[1]), (uint32_t)atoi(argv[2]));
    return true;
}

static sl_analysis *analyze_file(const char *file, const char *source) {
    sl_compile_options o;
    memset(&o, 0, sizeof o);
    o.entry_source = source;
    return sl_analyze(file, &o);
}

static int cmd_info(int argc, char **argv) {
    char *text; size_t len; uint32_t off;
    if (!read_position(argc, argv, &text, &len, &off)) { fprintf(stderr, "usage: sl info <file> <line> <col>\n"); return 2; }
    sl_analysis *a = analyze_file(argv[0], text);
    int rc = 1;
    sl_entity e;
    if (!a) { fprintf(stderr, "sl: out of memory\n"); }
    else if (!sl_analysis_checked(a)) {
        int n;
        const sl_diagnostic *d = sl_analysis_diagnostics(a, &n);
        fprintf(stderr, "sl: the file does not parse, so nothing can be resolved\n");
        for (int i = 0; i < n; i++) fprintf(stderr, "  %s:%u:%u: %s\n", d[i].file, d[i].line, d[i].col, d[i].message);
    } else if (!sl_analysis_entity_at(a, argv[0], off, &e)) {
        fprintf(stderr, "sl: no name at %s:%s:%s\n", argv[0], argv[1], argv[2]);
    } else {
        printf("%s %s", entity_name(e.kind), e.name);
        if (*e.type) printf(" : %s", e.type);
        printf("\n");
        if (*e.detail) printf("  %s\n", e.detail);
        if (e.has_definition) printf("  declared at %s:%u:%u\n", e.definition.file, e.definition.line, e.definition.col);
        else printf("  no declaration in source\n");
        rc = 0;
    }
    sl_analysis_free(a);
    free(text);
    return rc;
}

static int cmd_refs(int argc, char **argv) {
    char *text; size_t len; uint32_t off;
    if (!read_position(argc, argv, &text, &len, &off)) { fprintf(stderr, "usage: sl refs <file> <line> <col>\n"); return 2; }
    sl_analysis *a = analyze_file(argv[0], text);
    int rc = 1;
    sl_entity e;
    if (a && sl_analysis_checked(a) && sl_analysis_entity_at(a, argv[0], off, &e)) {
        sl_location locs[256];
        int n = sl_analysis_references(a, &e, locs, 256);
        for (int i = 0; i < n && i < 256; i++) printf("%s:%u:%u\n", locs[i].file, locs[i].line, locs[i].col);
        rc = 0;
    } else {
        fprintf(stderr, "sl: no name at %s:%s:%s\n", argv[0], argv[1], argv[2]);
    }
    sl_analysis_free(a);
    free(text);
    return rc;
}

static int cmd_complete(int argc, char **argv) {
    char *text; size_t len; uint32_t off;
    if (!read_position(argc, argv, &text, &len, &off)) { fprintf(stderr, "usage: sl complete <file> <line> <col>\n"); return 2; }
    // Code being typed rarely parses; patch the dot so it does.
    size_t plen = 0;
    char *patched = sl_completion_source(text, len, off, &plen);
    sl_analysis *a = analyze_file(argv[0], patched ? patched : text);
    int rc = 1;
    if (a && sl_analysis_checked(a)) {
        int n;
        const sl_completion *c = sl_analysis_complete(a, argv[0], off, &n);
        for (int i = 0; i < n; i++) printf("%-24s %-9s %s\n", c[i].label, entity_name(c[i].kind), c[i].detail);
        rc = 0;
    } else {
        fprintf(stderr, "sl: the file does not parse, even with the cursor patched\n");
    }
    sl_analysis_free(a);
    free(patched);
    free(text);
    return rc;
}

static int cmd_test(int argc, char **argv) {
    const char *filter = argc > 0 ? argv[0] : NULL;
    project pr;
    if (!find_project(&pr)) {
        fprintf(stderr, "sl: no solis.toml here or in any parent directory\n");
        return 2;
    }
    paths files;
    memset(&files, 0, sizeof files);
    char *src = join(pr.root, "src");
    walk(src, "_test.sl", &files);
    free(src);

    int ran = 0, failed = 0;
    for (int i = 0; i < files.len; i++) {
        if (filter && !strstr(files.at[i], filter)) continue;
        ran++;
        g_capture = true;
        g_out_len = 0;
        if (g_out) g_out[0] = 0;
        g_failed_assertions = 0;
        int rc = run_file(files.at[i], 0, NULL);
        g_capture = false;
        bool ok = rc == 0 && g_failed_assertions == 0;
        const char *shown = files.at[i];
        size_t root_len = strlen(pr.root);
        if (strncmp(shown, pr.root, root_len) == 0 && shown[root_len] == '/') shown += root_len + 1;
        if (ok) {
            printf("ok    %s\n", shown);
        } else {
            failed++;
            printf("FAIL  %s\n", shown);
            if (g_out && g_out_len) {
                for (const char *line = g_out; *line;) {
                    const char *eol = strchr(line, '\n');
                    size_t n = eol ? (size_t)(eol - line) : strlen(line);
                    printf("      %.*s\n", (int)n, line);
                    if (!eol) break;
                    line = eol + 1;
                }
            }
        }
    }
    if (ran == 0) printf("no tests found (name test files *_test.sl and put them under src/)\n");
    else printf("\n%d file%s, %d failed\n", ran, ran == 1 ? "" : "s", failed);
    return failed ? 1 : 0;
}

static void usage(void) {
    fprintf(stderr,
            "sl " VERSION " - the Solis project tool\n"
            "\n"
            "usage: sl <command> [arguments]\n"
            "\n"
            "  new <name>             create a project\n"
            "  run [file] [args...]   run a file, or the project's entry point\n"
            "  check [file...]        type check the entry point and the tests\n"
            "                           --json          print errors as JSON\n"
            "                           --stdin <path>  check source from standard input\n"
            "  outline <file>         list the declarations in a file\n"
            "  lsp                    run as a language server on standard input and output\n"
            "  info <file> <l> <c>    what the name at a position is, and where it is declared\n"
            "  refs <file> <l> <c>    every use of that name\n"
            "  complete <file> <l> <c>  what can be typed there\n"
            "  test [filter]          run every *_test.sl under src/\n"
            "  version                print the version\n"
            "\n"
            "A project is a directory with a solis.toml at its root. A test is a file\n"
            "named *_test.sl that calls the test module; any failed assertion fails the file.\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];
    if (strcmp(cmd, "new") == 0) return cmd_new(argc - 2, argv + 2);
    if (strcmp(cmd, "run") == 0) return cmd_run(argc - 2, argv + 2);
    if (strcmp(cmd, "check") == 0) return cmd_check(argc - 2, argv + 2);
    if (strcmp(cmd, "test") == 0) return cmd_test(argc - 2, argv + 2);
    if (strcmp(cmd, "outline") == 0) return cmd_outline(argc - 2, argv + 2);
    if (strcmp(cmd, "lsp") == 0) return lsp_main();
    if (strcmp(cmd, "info") == 0) return cmd_info(argc - 2, argv + 2);
    if (strcmp(cmd, "refs") == 0) return cmd_refs(argc - 2, argv + 2);
    if (strcmp(cmd, "complete") == 0) return cmd_complete(argc - 2, argv + 2);
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) { printf("sl " VERSION "\n"); return 0; }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0) { usage(); return 0; }
    fprintf(stderr, "sl: unknown command `%s`\n\n", cmd);
    usage();
    return 2;
}
