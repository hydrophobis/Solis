// Exercises the compiler's public API the way an outside host would: structured
// diagnostics, compiling from a string, the file and import hooks, and the
// syntax API. Uses only the public headers.

#include "solis.h"
#include "solis_compile.h"
#include "solis_syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char captured[256];
static sl_value capture_print(sl_vm *vm, int argc, sl_value *argv) {
    captured[0] = 0;
    for (int i = 0; i < argc; i++) {
        sl_value s = sl_to_string(vm, argv[i]);
        size_t n;
        const char *p = sl_as_str(s, &n);
        if (strlen(captured) + n < sizeof captured - 1) strncat(captured, p, n);
        sl_release(vm, s);
    }
    return sl_void();
}

static int run_image(const sl_compiled *c) {
    sl_vm *vm = sl_new();
    sl_open_std(vm);
    sl_register(vm, "print", capture_print);
    int ok = sl_load(vm, c->image, c->image_len) == SL_OK && sl_run(vm) == SL_OK;
    sl_free(vm);
    return ok;
}

// ---- a tiny virtual file system for the hooks ---------------------------------

typedef struct { const char *path, *text; } vfile;
typedef struct { const vfile *files; int n; int reads; } vfs;

static const vfile *find_file(const vfs *v, const char *path) {
    for (int i = 0; i < v->n; i++) if (strcmp(v->files[i].path, path) == 0) return &v->files[i];
    return NULL;
}
static char *vfs_read(void *user, const char *path, size_t *len) {
    vfs *v = (vfs *)user;
    const vfile *f = find_file(v, path);
    if (!f) return NULL;
    v->reads++;
    *len = strlen(f->text);
    char *copy = (char *)malloc(*len + 1);
    memcpy(copy, f->text, *len + 1);
    return copy;
}
static int vfs_exists(void *user, const char *path) { return find_file((vfs *)user, path) != NULL; }
// `import name;` anywhere means modules/name.mod in the virtual tree
static char *vfs_resolve(void *user, const char *from, const char *name) {
    (void)user; (void)from;
    char *p = (char *)malloc(strlen(name) + 16);
    sprintf(p, "modules/%s.mod", name);
    return p;
}

// A native that tries to call back into the VM that is running it.
static sl_vm *reenter_vm;
static sl_value reenter_native(sl_vm *vm, int argc, sl_value *argv) {
    (void)argc; (void)argv;
    sl_value r;
    return sl_int(sl_call(reenter_vm ? reenter_vm : vm, "game.noop", 0, NULL, &r) == SL_ERR_CALL ? 1 : 0);
}

int main(void) {
    // 1. A type error comes back with a position, a message and the file.
    {
        sl_compile_options o;
        memset(&o, 0, sizeof o);
        o.entry_source = "func main() {\n    let x: int = \"text\";\n}\n";
        sl_compiled c;
        int rc = sl_compile_with("buffer.sl", &o, &c);
        CHECK(rc != 0, "a type error should fail");
        CHECK(c.diagnostic_count == 1, "expected one diagnostic, got %d", c.diagnostic_count);
        if (c.diagnostic_count == 1) {
            const sl_diagnostic *d = &c.diagnostics[0];
            CHECK(strcmp(d->file, "buffer.sl") == 0, "file was %s", d->file);
            CHECK(d->line == 2 && d->col == 18, "position was %u:%u", d->line, d->col);
            CHECK(d->end_line == 2 && d->end_col == 24, "end was %u:%u", d->end_line, d->end_col);
            CHECK(strstr(d->message, "expected `int`") != NULL, "message was %s", d->message);
        }
        CHECK(c.errors && strstr(c.errors, "buffer.sl:2:18"), "rendered text should still be there");
        sl_compiled_free(&c);
    }

    // 2. Compiling from a string needs no file on disk, and the result runs.
    {
        sl_compile_options o;
        memset(&o, 0, sizeof o);
        o.entry_source = "func main() { print(\"sum=${1 + 2}\"); }";
        sl_compiled c;
        int rc = sl_compile_with("does/not/exist.sl", &o, &c);
        CHECK(rc == 0, "compiling a string failed: %s", c.errors ? c.errors : "");
        if (rc == 0) {
            CHECK(run_image(&c), "the image should run");
            CHECK(strcmp(captured, "sum=3") == 0, "output was '%s'", captured);
        }
        sl_compiled_free(&c);
    }

    // 3. Check-only produces no image but still reports errors.
    {
        sl_compile_options o;
        memset(&o, 0, sizeof o);
        o.check_only = 1;
        o.entry_source = "func main() { print(1); }";
        sl_compiled c;
        CHECK(sl_compile_with("a.sl", &o, &c) == 0, "check should pass");
        CHECK(c.image == NULL, "check-only should not emit an image");
        sl_compiled_free(&c);
    }

    // 4. Hooks: imports come from a virtual tree, in a layout the default would never find.
    {
        static const vfile files[] = {
            { "project/main.sl", "import util;\nfunc main() { print(util.twice(21)); }\n" },
            { "modules/util.mod", "func twice(n: int): int { return n * 2; }\n" },
        };
        vfs v = { files, 2, 0 };
        sl_compile_options o;
        memset(&o, 0, sizeof o);
        o.read = vfs_read;
        o.exists = vfs_exists;
        o.resolve = vfs_resolve;
        o.user = &v;
        sl_compiled c;
        int rc = sl_compile_with("project/main.sl", &o, &c);
        CHECK(rc == 0, "virtual project failed: %s", c.errors ? c.errors : "");
        CHECK(v.reads >= 2, "both files should come through the read hook (%d)", v.reads);
        if (rc == 0) {
            CHECK(run_image(&c), "virtual project should run");
            CHECK(strcmp(captured, "42") == 0, "output was '%s'", captured);
        }
        sl_compiled_free(&c);
    }

    // 5. A module the hooks cannot supply is reported against the importer.
    {
        static const vfile files[] = { { "p/main.sl", "import gone;\nfunc main() { }\n" } };
        vfs v = { files, 1, 0 };
        sl_compile_options o;
        memset(&o, 0, sizeof o);
        o.read = vfs_read;
        o.exists = vfs_exists;
        o.user = &v;
        sl_compiled c;
        CHECK(sl_compile_with("p/main.sl", &o, &c) != 0, "a missing module should fail");
        CHECK(c.diagnostic_count >= 1 && strstr(c.diagnostics[0].message, "cannot read module `gone`"),
              "message was %s", c.diagnostic_count ? c.diagnostics[0].message : "(none)");
        sl_compiled_free(&c);
    }

    // 6. Errors in an imported module name that module's file.
    {
        static const vfile files[] = {
            { "p/main.sl", "import bad;\nfunc main() { }\n" },
            { "p/bad.sl", "func broken(): int {\n    return \"no\";\n}\n" },
        };
        vfs v = { files, 2, 0 };
        sl_compile_options o;
        memset(&o, 0, sizeof o);
        o.read = vfs_read;
        o.exists = vfs_exists;
        o.user = &v;
        sl_compiled c;
        CHECK(sl_compile_with("p/main.sl", &o, &c) != 0, "should fail");
        CHECK(c.diagnostic_count >= 1 && strcmp(c.diagnostics[0].file, "p/bad.sl") == 0 && c.diagnostics[0].line == 2,
              "diagnostic was %s:%u", c.diagnostic_count ? c.diagnostics[0].file : "(none)",
              c.diagnostic_count ? c.diagnostics[0].line : 0);
        sl_compiled_free(&c);
    }

    // 7. The syntax API: tokens, comments and an outline, plus errors with positions.
    {
        const char *src =
            "import math;\n"
            "// the origin\n"
            "struct Point {\n"
            "    x: int;\n"
            "    y: int;\n"
            "    func sum(): int { return this.x + this.y; }\n"
            "}\n"
            "/* a block\n   comment */\n"
            "enum Shape { Dot, Line(a: int) }\n"
            "let limit = 10;\n"
            "func main() { print(limit); }\n";
        sl_syntax *s = sl_syntax_parse(src, strlen(src));
        CHECK(s != NULL, "parse returned NULL");
        int nt, nc, ns, nd;
        const sl_token *toks = sl_syntax_tokens(s, &nt);
        const sl_comment *cm = sl_syntax_comments(s, &nc);
        const sl_symbol *sy = sl_syntax_symbols(s, &ns);
        sl_syntax_diagnostics(s, &nd);
        CHECK(nd == 0, "no syntax errors expected, got %d", nd);
        CHECK(nt > 20 && toks[nt - 1].kind == SL_T_EOF, "tokens should end with EOF (%d)", nt);
        CHECK(toks[0].kind == SL_T_IMPORT, "first token should be import");
        CHECK(nc == 2, "expected 2 comments, got %d", nc);
        if (nc == 2) {
            CHECK(strncmp(src + cm[0].start, "// the origin", cm[0].end - cm[0].start) == 0, "first comment text");
            CHECK(src[cm[1].start] == '/' && src[cm[1].start + 1] == '*', "second comment is a block comment");
        }
        // import, Point, x, y, sum, Shape, Dot, Line, limit, main
        CHECK(ns == 10, "expected 10 symbols, got %d", ns);
        if (ns == 10) {
            CHECK(sy[0].kind == SL_SYM_IMPORT && strcmp(sy[0].name, "math") == 0, "symbol 0");
            CHECK(sy[1].kind == SL_SYM_STRUCT && strcmp(sy[1].name, "Point") == 0 && sy[1].parent == -1, "symbol 1");
            CHECK(sy[2].kind == SL_SYM_FIELD && sy[2].parent == 1, "field belongs to Point");
            CHECK(sy[4].kind == SL_SYM_METHOD && strcmp(sy[4].name, "sum") == 0 && sy[4].parent == 1, "method belongs to Point");
            CHECK(sy[6].kind == SL_SYM_VARIANT && sy[6].parent == 5, "variant belongs to Shape");
            CHECK(strncmp(src + sy[1].name_start, "Point", sy[1].name_end - sy[1].name_start) == 0 &&
                  sy[1].name_end - sy[1].name_start == 5, "name span of Point");
            CHECK(sy[8].kind == SL_SYM_LET && strcmp(sy[8].name, "limit") == 0, "module-level let");
            CHECK(sy[9].kind == SL_SYM_FUNC && strcmp(sy[9].name, "main") == 0, "main");
        }
        sl_syntax_free(s);
    }
    {
        const char *src = "func main() {\n    let a = 1\n    let b = 2;\n}\n";
        sl_syntax *s = sl_syntax_parse(src, strlen(src));
        int nd, ns;
        const sl_diagnostic *d = sl_syntax_diagnostics(s, &nd);
        CHECK(nd >= 1, "a missing semicolon should be reported");
        if (nd >= 1) CHECK(d[0].line == 2, "error should be on line 2, was %u", d[0].line);
        const sl_symbol *sy = sl_syntax_symbols(s, &ns);
        CHECK(ns >= 1 && sy[0].kind == SL_SYM_FUNC, "the outline should survive a syntax error");
        sl_syntax_free(s);
    }

    // Calling a script's functions by name, after main has run.
    {
        sl_compile_options o;
        memset(&o, 0, sizeof o);
        o.entry_source =
            "import strings;\n"
            "extern func reenter(): int;\n"
            "var counter = 10;\n"
            "func main() { counter = counter + 1; }\n"
            "func bump(by: int): int { counter = counter + by; return counter; }\n"
            "func greet(name: str): str { return \"hi ${name}\"; }\n"
            "func noop() { }\n"
            "func boom() { let a: [int] = [1]; print(a[5]); }\n"
            "func isEmpty(s: str): bool { return true; }\n"
            "func try_reenter(): int { return reenter(); }\n";
        sl_compiled c;
        int rc = sl_compile_with("game.sl", &o, &c);
        CHECK(rc == 0, "the call-by-name script failed to compile: %s", c.errors ? c.errors : "");
        if (rc == 0) {
            sl_vm *vm = sl_new();
            sl_open_std(vm);
            sl_register(vm, "print", capture_print);
            sl_register(vm, "reenter", reenter_native);
            CHECK(sl_load(vm, c.image, c.image_len) == SL_OK, "load: %s", sl_error(vm));

            // enumeration and lookup
            int n = sl_function_count(vm);
            CHECK(n >= 7, "expected the script's functions in the image, got %d", n);
            int bump_i = sl_find_function(vm, "game.bump");
            CHECK(bump_i >= 0 && sl_function_arity(vm, bump_i) == 1 && strcmp(sl_function_name(vm, bump_i), "game.bump") == 0, "game.bump lookup");
            CHECK(sl_find_function(vm, "bump") == bump_i, "a bare name should find game.bump");
            CHECK(sl_find_function(vm, "nope") == -1, "unknown name");
            CHECK(sl_find_function(vm, "isEmpty") == -2, "isEmpty exists in game and strings, so a bare name is ambiguous");
            CHECK(sl_find_function(vm, "game.isEmpty") >= 0, "the qualified name is not ambiguous");
            CHECK(sl_function_name(vm, -1) == NULL && sl_function_name(vm, n) == NULL && sl_function_arity(vm, n) == -1, "out of range");

            // not before sl_run: the globals are not initialised yet
            sl_value arg = sl_int(5), res = sl_int(99);
            CHECK(sl_call(vm, "game.bump", 1, &arg, &res) == SL_ERR_CALL, "calling before sl_run should be refused");
            CHECK(res.type == SL_VOID, "result should be reset to void on failure");
            CHECK(strstr(sl_error(vm), "sl_run") != NULL, "message was '%s'", sl_error(vm));

            CHECK(sl_run(vm) == SL_OK, "run: %s", sl_error(vm));

            // state persists between calls: main made it 11, then +5, +5
            CHECK(sl_call(vm, "game.bump", 1, &arg, &res) == SL_OK && res.type == SL_INT && res.as.i == 16, "first bump");
            CHECK(sl_call(vm, "bump", 1, &arg, &res) == SL_OK && res.as.i == 21, "second bump through the bare name");
            CHECK(sl_call_index(vm, bump_i, 1, &arg, &res) == SL_OK && res.as.i == 26, "bump by index");

            // strings in and out; the result is the caller's to release
            sl_value name = sl_cstr(vm, "Ada");
            CHECK(sl_call(vm, "game.greet", 1, &name, &res) == SL_OK, "greet: %s", sl_error(vm));
            size_t len = 0;
            const char *text = res.type == SL_OBJ ? sl_as_str(res, &len) : "";
            CHECK(len == 6 && memcmp(text, "hi Ada", 6) == 0, "greet returned '%.*s'", (int)len, text);
            sl_release(vm, res);
            sl_release(vm, name);

            // no result, and no result wanted
            CHECK(sl_call(vm, "noop", 0, NULL, &res) == SL_OK && res.type == SL_VOID, "a function with no result gives void");
            CHECK(sl_call(vm, "noop", 0, NULL, NULL) == SL_OK, "the result is optional");

            // refusals
            CHECK(sl_call(vm, "game.bump", 0, NULL, &res) == SL_ERR_CALL && strstr(sl_error(vm), "takes 1") != NULL, "wrong argument count: '%s'", sl_error(vm));
            CHECK(sl_call(vm, "game.nothing", 0, NULL, &res) == SL_ERR_CALL && strstr(sl_error(vm), "no function") != NULL, "unknown function: '%s'", sl_error(vm));
            CHECK(sl_call(vm, "isEmpty", 0, NULL, &res) == SL_ERR_CALL && strstr(sl_error(vm), "ambiguous") != NULL, "ambiguous: '%s'", sl_error(vm));
            CHECK(sl_call(vm, "<start>", 0, NULL, &res) == SL_ERR_CALL, "the synthetic entry function is not callable");
            CHECK(sl_call_index(vm, 100000, 0, NULL, &res) == SL_ERR_CALL, "bad index");

            // a fault inside the function is reported, and the VM stays usable
            CHECK(sl_call(vm, "boom", 0, NULL, &res) == SL_ERR_RUNTIME && strstr(sl_error(vm), "out of bounds") != NULL, "boom: '%s'", sl_error(vm));
            CHECK(sl_call(vm, "game.bump", 1, &arg, &res) == SL_OK && res.as.i == 31, "the VM should still work after a fault");

            // calling from inside a native is refused, and does not break the run in progress
            reenter_vm = vm;
            CHECK(sl_call(vm, "try_reenter", 0, NULL, &res) == SL_OK && res.type == SL_INT && res.as.i == 1, "a nested call should be refused (SL_ERR_CALL seen by the native) without failing the outer one");

            CHECK(sl_live_objects(vm) == 0, "leaked %lld objects", (long long)sl_live_objects(vm));
            sl_free(vm);
        }
        sl_compiled_free(&c);
    }

    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("api: all checks passed\n");
    return 0;
}
