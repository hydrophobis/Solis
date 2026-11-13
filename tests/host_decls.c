#include "solis.h"
#include "solis_compile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sl_value nop(sl_vm *vm, int argc, sl_value *argv) {
    (void)vm; (void)argc; (void)argv;
    return sl_void();
}

static int failures;

static void expect_line(const char *decls, const char *line) {
    if (!strstr(decls, line)) {
        fprintf(stderr, "host_decls: missing line: %s\n", line);
        failures++;
    }
}

static void expect_absent(const char *decls, const char *text) {
    if (strstr(decls, text)) {
        fprintf(stderr, "host_decls: should not contain: %s\n", text);
        failures++;
    }
}

int main(void) {
    sl_vm *vm = sl_new();
    sl_register_sig(vm, "none", "()->()", nop);
    sl_register_sig(vm, "scalars", "(int,float,bool,str)->str", nop);
    sl_register_sig(vm, "compound", "([int],{str: int})->[str]", nop);
    sl_register_sig(vm, "nested", "([[int]])->{str:{str: int}}", nop);
    sl_register_sig(vm, "spaced", "(  int ,  str )->  bool  ", nop);
    sl_register_sig(vm, "usertype", "(tour.Vec2,int)->()", nop);
    sl_register_sig(vm, "garbled", "nonsense", nop);
    sl_register_sig(vm, "twice", "()->int", nop);
    sl_register_sig(vm, "twice", "()->str", nop);        // the later registration wins
    sl_register(vm, "unsigned", nop);
    sl_register_sig(vm, "math.dotted", "()->int", nop);  // not a name a script can write after `host.`

    char *decls = sl_declarations(vm);
    if (!decls) { fprintf(stderr, "host_decls: sl_declarations returned NULL\n"); return 1; }

    expect_line(decls, "extern func none();\n");
    expect_line(decls, "extern func scalars(p0: int, p1: float, p2: bool, p3: str): str;\n");
    expect_line(decls, "extern func compound(p0: [int], p1: {str: int}): [str];\n");
    expect_line(decls, "extern func nested(p0: [[int]]): {str:{str: int}};\n");
    expect_line(decls, "extern func spaced(p0: int, p1: str): bool;\n");
    expect_line(decls, "// `usertype` is not declared here");
    expect_line(decls, "// `garbled` is not declared here");
    expect_line(decls, "extern func twice(): str;\n");
    expect_absent(decls, "extern func twice(): int;");
    expect_absent(decls, "unsigned");
    expect_absent(decls, "dotted");

    FILE *f = fopen("host_decls_probe.sl", "w");
    if (!f) { fprintf(stderr, "host_decls: cannot write the probe script\n"); return 1; }
    fputs("import host;\nfunc main() {\n    host.none();\n}\n", f);
    fclose(f);
    sl_compiled c;
    if (sl_check_file_with_host("host_decls_probe.sl", "", decls, &c) != 0) {
        fprintf(stderr, "host_decls: the generated module does not compile:\n%s\n", c.errors ? c.errors : "?");
        failures++;
    }
    sl_compiled_free(&c);
    remove("host_decls_probe.sl");

    free(decls);
    sl_free(vm);
    if (failures) return 1;
    printf("host_decls: ok\n");
    return 0;
}
