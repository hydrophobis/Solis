// The analysis API as a language server would use it: definition, type, references
// and completion, over a small two-module project served from memory.

#include "solis_analysis.h"
#include "solis_compile.h"
#include "solis_syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *MAIN_SL =
    "import geom;\n"                                              // 1
    "\n"                                                          // 2
    "struct Counter {\n"                                          // 3
    "    count: int;\n"                                           // 4
    "    func bump(by: int): int {\n"                             // 5
    "        return this.count + by;\n"                           // 6
    "    }\n"                                                     // 7
    "}\n"                                                         // 8
    "\n"                                                          // 9
    "enum Mode { Fast, Slow(delay: int) }\n"                      // 10
    "\n"                                                          // 11
    "func scale(factor: float): float {\n"                        // 12
    "    let doubled = factor * 2.0;\n"                           // 13
    "    return doubled;\n"                                       // 14
    "}\n"                                                         // 15
    "\n"                                                          // 16
    "func main() {\n"                                             // 17
    "    let c = Counter { count: 1 };\n"                         // 18
    "    let n = c.bump(2);\n"                                    // 19
    "    var total = n + c.count;\n"                              // 20
    "    let shape = geom.Point.origin();\n"                      // 21
    "    let m = Mode.Fast;\n"                                    // 22
    "    print(scale(1.5), total, shape.x);\n"                    // 23
    "}\n";

static const char *GEOM_SL =
    "struct Point {\n"
    "    x: float;\n"
    "    y: float;\n"
    "    static func origin(): Point { return Point { x: 0.0, y: 0.0 }; }\n"
    "}\n";

typedef struct { const char *path, *text; } vfile;
static const vfile FILES[] = { { "app/main.sl", NULL }, { "app/geom.sl", NULL } };

static char *vread(void *user, const char *path, size_t *len) {
    (void)user;
    const char *t = strcmp(path, "app/geom.sl") == 0 ? GEOM_SL : strcmp(path, "app/main.sl") == 0 ? MAIN_SL : NULL;
    if (!t) return NULL;
    *len = strlen(t);
    char *c = (char *)malloc(*len + 1);
    memcpy(c, t, *len + 1);
    return c;
}
static int vexists(void *user, const char *path) { (void)user; return strcmp(path, "app/geom.sl") == 0 || strcmp(path, "app/main.sl") == 0; }

// Byte offset of the nth (0-based) occurrence of `needle`, plus `delta`.
static uint32_t at(const char *text, const char *needle, int nth, int delta) {
    const char *p = text;
    for (int i = 0; i <= nth; i++) {
        p = strstr(p, needle);
        if (!p) { printf("test bug: no %d-th \"%s\"\n", nth, needle); exit(2); }
        if (i < nth) p++;
    }
    return (uint32_t)(p - text) + (uint32_t)delta;
}

static sl_analysis *analyze_main(const char *text) {
    sl_compile_options o;
    memset(&o, 0, sizeof o);
    o.entry_source = text;
    o.read = vread;
    o.exists = vexists;
    return sl_analyze("app/main.sl", &o);
}

static int entity(sl_analysis *a, const char *file, const char *text, const char *needle, int nth, int delta, sl_entity *e) {
    return sl_analysis_entity_at(a, file, at(text, needle, nth, delta), e);
}

static int has_label(const sl_completion *c, int n, const char *label) {
    for (int i = 0; i < n; i++) if (strcmp(c[i].label, label) == 0) return 1;
    return 0;
}

int main(void) {
    (void)FILES;
    sl_analysis *a = analyze_main(MAIN_SL);
    CHECK(a != NULL, "analysis failed");
    int nd;
    sl_analysis_diagnostics(a, &nd);
    CHECK(nd == 0 && sl_analysis_checked(a), "the project should be clean (%d diagnostics)", nd);
    sl_entity e;

    // A function: its type, signature and declaration.
    CHECK(entity(a, "app/main.sl", MAIN_SL, "scale(1.5)", 0, 1, &e), "no entity at scale");
    CHECK(e.kind == SL_ENT_FUNC && strcmp(e.name, "scale") == 0, "scale was kind %d '%s'", e.kind, e.name);
    CHECK(strcmp(e.detail, "func scale(factor: float): float") == 0, "detail was '%s'", e.detail);
    CHECK(e.has_definition && e.definition.line == 12 && e.definition.col == 6, "scale defined at %u:%u", e.definition.line, e.definition.col);
    CHECK(strcmp(e.definition.file, "app/main.sl") == 0, "definition file %s", e.definition.file);

    // A local, and where it was declared.
    CHECK(entity(a, "app/main.sl", MAIN_SL, "return doubled", 0, 8, &e), "no entity at doubled");
    CHECK(e.kind == SL_ENT_LOCAL && strcmp(e.type, "float") == 0, "doubled: kind %d type '%s'", e.kind, e.type);
    CHECK(e.has_definition && e.definition.line == 13 && e.definition.col == 9, "doubled declared at %u:%u", e.definition.line, e.definition.col);

    // A parameter.
    CHECK(entity(a, "app/main.sl", MAIN_SL, "factor * 2.0", 0, 2, &e), "no entity at factor");
    CHECK(e.kind == SL_ENT_PARAM && e.definition.line == 12, "factor: kind %d defined on %u", e.kind, e.definition.line);

    // The declaration itself answers the same way as a use.
    CHECK(entity(a, "app/main.sl", MAIN_SL, "factor: float", 0, 1, &e) && e.kind == SL_ENT_PARAM, "declaration of factor");

    // Methods and fields through a variable.
    CHECK(entity(a, "app/main.sl", MAIN_SL, "c.bump", 0, 3, &e), "no entity at bump");
    CHECK(e.kind == SL_ENT_METHOD && e.has_definition && e.definition.line == 5, "bump: kind %d line %u", e.kind, e.definition.line);
    CHECK(strstr(e.detail, "func bump(by: int): int") != NULL, "bump detail '%s'", e.detail);
    CHECK(entity(a, "app/main.sl", MAIN_SL, "c.count", 0, 3, &e), "no entity at count");
    CHECK(e.kind == SL_ENT_FIELD && strcmp(e.type, "int") == 0 && e.definition.line == 4, "count: kind %d type '%s' line %u", e.kind, e.type, e.definition.line);

    // Struct literals name a type and its fields.
    CHECK(entity(a, "app/main.sl", MAIN_SL, "Counter {", 0, 2, &e) && e.kind == SL_ENT_TYPE && e.definition.line == 3, "Counter in the literal");
    CHECK(entity(a, "app/main.sl", MAIN_SL, "count: 1", 0, 2, &e) && e.kind == SL_ENT_FIELD && e.definition.line == 4, "count: in the literal");

    // Another module: the definition is in the other file.
    CHECK(entity(a, "app/main.sl", MAIN_SL, "Point.origin", 0, 1, &e), "no entity at Point");
    CHECK(e.kind == SL_ENT_TYPE && e.has_definition && strcmp(e.definition.file, "app/geom.sl") == 0 && e.definition.line == 1,
          "Point: kind %d file %s line %u", e.kind, e.definition.file, e.definition.line);
    CHECK(entity(a, "app/main.sl", MAIN_SL, "origin()", 0, 2, &e), "no entity at origin");
    CHECK(e.kind == SL_ENT_METHOD && strcmp(e.definition.file, "app/geom.sl") == 0 && e.definition.line == 4,
          "origin: kind %d file %s line %u", e.kind, e.definition.file, e.definition.line);

    // Enum variants.
    CHECK(entity(a, "app/main.sl", MAIN_SL, "Mode.Fast", 0, 6, &e), "no entity at Fast");
    CHECK(e.kind == SL_ENT_VARIANT && e.definition.line == 10, "Fast: kind %d line %u", e.kind, e.definition.line);

    // A built-in has a type and no declaration to jump to.
    CHECK(entity(a, "app/main.sl", MAIN_SL, "print(", 0, 1, &e) && e.kind == SL_ENT_BUILTIN && !e.has_definition, "print");

    // Nothing at whitespace.
    CHECK(!sl_analysis_entity_at(a, "app/main.sl", at(MAIN_SL, "\n\nstruct", 0, 0), &e), "an entity where there is only a blank line");
    CHECK(!sl_analysis_entity_at(a, "no/such/file.sl", 5, &e), "an entity in an unknown file");

    // References: the field `count` is declared once, and used three times.
    CHECK(entity(a, "app/main.sl", MAIN_SL, "c.count", 0, 3, &e), "no entity for count");
    sl_location refs[16];
    int nr = sl_analysis_references(a, &e, refs, 16);
    CHECK(nr == 4, "expected 4 references to count, got %d", nr);
    if (nr == 4) {
        CHECK(refs[0].line == 4 && refs[1].line == 6 && refs[2].line == 18 && refs[3].line == 20,
              "reference lines were %u %u %u %u", refs[0].line, refs[1].line, refs[2].line, refs[3].line);
    }
    // A type used in another module is found from the other module too.
    CHECK(entity(a, "app/main.sl", MAIN_SL, "Point.origin", 0, 1, &e), "no Point");
    nr = sl_analysis_references(a, &e, refs, 16);
    CHECK(nr >= 3, "Point should have at least its declaration and two uses, got %d", nr);

    // Completion: names in scope at the end of main's body.
    {
        uint32_t off = at(MAIN_SL, "    print(scale", 0, 0);
        int n;
        const sl_completion *c = sl_analysis_complete(a, "app/main.sl", off, &n);
        CHECK(c && has_label(c, n, "c") && has_label(c, n, "total") && has_label(c, n, "shape"), "locals missing from completion");
        CHECK(has_label(c, n, "scale") && has_label(c, n, "Counter") && has_label(c, n, "Mode"), "module-level names missing");
        CHECK(has_label(c, n, "geom") && has_label(c, n, "print") && has_label(c, n, "while"), "module, builtin or keyword missing");
        CHECK(!has_label(c, n, "doubled") && !has_label(c, n, "factor"), "another function's locals should not be offered");
        c = sl_analysis_complete(a, "app/main.sl", off + 4 + 2, &n);
        (void)c;
    }
    // A prefix narrows the list.
    {
        uint32_t off = at(MAIN_SL, "print(scale", 0, 3);      // after "pri"
        int n;
        const sl_completion *c = sl_analysis_complete(a, "app/main.sl", off, &n);
        CHECK(n >= 1 && has_label(c, n, "print") && !has_label(c, n, "scale"), "prefix 'pri' gave %d entries", n);
    }
    // Members after a dot.
    {
        const char *src =
            "struct Counter { count: int; func bump(by: int): int { return by; } }\n"
            "enum Mode { Fast, Slow }\n"
            "func main() {\n"
            "    let c = Counter { count: 1 };\n"
            "    let n = c.\n"
            "    print(n);\n"
            "}\n";
        // The statement `let n = c.` does not parse; patch it the way a client would.
        size_t plen;
        uint32_t off = at(src, "c.\n", 0, 2);
        char *patched = sl_completion_source(src, strlen(src), off, &plen);
        CHECK(patched != NULL, "the source should need a placeholder");
        if (patched) {
            sl_compile_options o;
            memset(&o, 0, sizeof o);
            o.entry_source = patched;
            sl_analysis *b = sl_analyze("members.sl", &o);
            int n;
            const sl_completion *c = sl_analysis_complete(b, "members.sl", off, &n);
            CHECK(sl_analysis_checked(b), "the patched source should parse");
            CHECK(has_label(c, n, "count") && has_label(c, n, "bump") && n == 2, "members of Counter: %d entries", n);
            sl_analysis_free(b);
            free(patched);
        }
        // Static members after a type name.
        const char *src2 =
            "enum Mode { Fast, Slow }\n"
            "struct Maker { static func make(): int { return 1; } func inst(): int { return 2; } }\n"
            "func main() {\n"
            "    let a = Mode.Fast;\n"
            "    let b = Maker.make();\n"
            "}\n";
        sl_compile_options o;
        memset(&o, 0, sizeof o);
        o.entry_source = src2;
        sl_analysis *b = sl_analyze("types.sl", &o);
        int n;
        const sl_completion *c = sl_analysis_complete(b, "types.sl", at(src2, "Mode.Fast", 0, 5), &n);
        CHECK(has_label(c, n, "Fast") && has_label(c, n, "Slow") && n == 2, "variants of Mode: %d entries", n);
        c = sl_analysis_complete(b, "types.sl", at(src2, "Maker.make", 0, 6), &n);
        CHECK(has_label(c, n, "make") && !has_label(c, n, "inst"), "static members of Maker");
        sl_analysis_free(b);
    }
    sl_analysis_free(a);

    // Type errors do not stop the checker: names elsewhere still resolve, and the error is reported.
    {
        const char *src =
            "func ok(a: int): int { return a; }\n"
            "func bad(): int { return \"no\"; }\n"
            "func main() { print(ok(1)); }\n";
        sl_compile_options o;
        memset(&o, 0, sizeof o);
        o.entry_source = src;
        sl_analysis *b = sl_analyze("t.sl", &o);
        int n;
        const sl_diagnostic *d = sl_analysis_diagnostics(b, &n);
        CHECK(n == 1 && d[0].line == 2, "expected one error on line 2, got %d", n);
        CHECK(sl_analysis_checked(b), "the checker should still have run");
        CHECK(sl_analysis_entity_at(b, "t.sl", at(src, "ok(1)", 0, 1), &e) && e.kind == SL_ENT_FUNC, "ok should resolve despite the error");
        sl_analysis_free(b);
    }
    // A syntax error stops before the checker: diagnostics, but no entities.
    {
        const char *src = "func main() {\n    let a = 1\n    print(a);\n}\n";
        sl_compile_options o;
        memset(&o, 0, sizeof o);
        o.entry_source = src;
        sl_analysis *b = sl_analyze("t.sl", &o);
        int n;
        sl_analysis_diagnostics(b, &n);
        CHECK(n >= 1 && !sl_analysis_checked(b), "a syntax error should be reported and stop the checker");
        CHECK(!sl_analysis_entity_at(b, "t.sl", at(src, "print", 0, 1), &e), "no entities without the checker");
        sl_analysis_free(b);
    }
    // Names inside a string interpolation have real positions in the file.
    {
        const char *src =
            "func f(x: int): int { return x; }\n"
            "func main() { let a = 1; print(\"v=${f(a)} and ${a + \"s\"}\"); }\n";
        sl_compile_options o;
        memset(&o, 0, sizeof o);
        o.entry_source = src;
        sl_analysis *b = sl_analyze("t.sl", &o);
        CHECK(entity(b, "t.sl", src, "f(a)", 0, 0, &e) && e.kind == SL_ENT_FUNC && e.definition.line == 1,
              "f inside an interpolation");
        CHECK(entity(b, "t.sl", src, "f(a)", 0, 2, &e) && e.kind == SL_ENT_LOCAL && e.definition.line == 2 && e.definition.col == 19,
              "a inside an interpolation: kind %d declared at %u:%u", e.kind, e.definition.line, e.definition.col);
        int n;
        const sl_diagnostic *d = sl_analysis_diagnostics(b, &n);
        CHECK(n >= 1 && d[0].line == 2 && d[0].col == 53, "error inside an interpolation was at %u:%u", n ? d[0].line : 0, n ? d[0].col : 0);
        sl_analysis_free(b);
    }
    // Line and column round-trip through the helpers.
    {
        const char *src = "ab\ncdé fg\n";
        uint32_t off = sl_line_col_to_offset(src, strlen(src), 2, 5);       // the 'f'
        uint32_t line, col;
        sl_offset_to_line_col(src, strlen(src), off, &line, &col);
        CHECK(src[off] == 'f' && line == 2 && col == 5, "round trip gave '%c' %u:%u", src[off], line, col);
    }

    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("analysis: all checks passed\n");
    return 0;
}
