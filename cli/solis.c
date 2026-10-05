#include "check.h"
#include "gen.h"
#include "module.h"
#include "parse.h"
#include "solis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VERSION "0.1.0"

static void usage(void) {
    fprintf(stderr,
            "solis " VERSION " - the Solis interpreter\n"
            "\n"
            "usage: solis [options] <file.sl|file.slb> [script args...]\n"
            "\n"
            "  -o <file.slb>     compile to an image instead of running\n"
            "  --check           type check only\n"
            "  --prelude <file>  host declarations every module can see\n"
            "  --no-prelude      ignore a prelude.sl sitting beside the script\n"
            "  --check-leaks     report live objects after the run\n"
            "  --tokens          token and diagnostic counts only\n"
            "  --stats           report what the image contains\n"
            "  --version         print the version\n"
            "\n"
            "Arguments after the script belong to the script: os.args().\n");
}

static sl_value native_print(sl_vm *vm, int argc, sl_value *argv) {
    for (int i = 0; i < argc; i++) {
        if (i) printf(" ");
        sl_value s = sl_to_string(vm, argv[i]);
        size_t n;
        const char *p = sl_as_str(s, &n);
        fwrite(p, 1, n, stdout);
        sl_release(vm, s);
    }
    printf("\n");
    return sl_void();
}

// What examples/host.sl expects a plain host to register.
static sl_value native_now_ms(sl_vm *vm, int argc, sl_value *argv) {
    (void)vm; (void)argc; (void)argv;
    return sl_int((int64_t)(clock() * 1000.0 / CLOCKS_PER_SEC));
}

static sl_value native_host_name(sl_vm *vm, int argc, sl_value *argv) {
    (void)argc; (void)argv;
    return sl_cstr(vm, "solis-c");
}

static sl_value native_repeat(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 2 || argv[1].type != SL_INT) {
        sl_fail(vm, "repeat(str, int) expects a string and an int");
        return sl_void();
    }
    size_t n;
    const char *p = sl_as_str(argv[0], &n);
    if (!p) { sl_fail(vm, "repeat(str, int) expects a string and an int"); return sl_void(); }
    int64_t times = argv[1].as.i < 0 ? 0 : argv[1].as.i;
    size_t total = n * (size_t)times;
    char *buf = (char *)malloc(total ? total : 1);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }
    for (int64_t i = 0; i < times; i++) memcpy(buf + (size_t)i * n, p, n);
    sl_value out = sl_str(vm, buf, total);
    free(buf);
    return out;
}

static void report(const loaded *l, const diag_vec *diags, const char *fallback_src,
                   size_t fallback_len, const char *fallback_path) {
    for (int i = 0; i < diags->len; i++) {
        const diag *d = &diags->at[i];
        const char *src = fallback_src;
        size_t len = fallback_len;
        const char *path = fallback_path;

        if (d->module && l) {
            module *m = module_find(l, d->module);
            if (m) {
                src = m->src;
                len = m->src_len;
                path = m->path;
            }
        }
        fputc('\n', stderr);
        diag_render(src, len, path, d, stderr);
    }
}

static uint8_t *read_whole(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    *len_out = got;
    return buf;
}

static int run_image(const uint8_t *bytes, size_t len, bool check_leaks,
                     int sargc, char **sargv) {
    sl_vm *vm = sl_new();
    if (!vm) return 1;

    sl_open_std(vm);
    sl_open_os(vm, sargc, sargv);

    sl_register(vm, "print", native_print);
    sl_register(vm, "nowMs", native_now_ms);
    sl_register(vm, "hostName", native_host_name);
    sl_register(vm, "repeat", native_repeat);

    sl_result rc = sl_load(vm, bytes, len);
    if (rc != SL_OK) {
        fprintf(stderr, "load error: %s\n", sl_error(vm));
        sl_free(vm);
        return 1;
    }

    rc = sl_run(vm);
    if (rc != SL_OK) {
        fflush(stdout);
        fprintf(stderr, "runtime error: %s\n", sl_error(vm));
        sl_free(vm);
        return 1;
    }

    int status = 0;
    if (check_leaks) {
        int64_t live = sl_live_objects(vm);
        fflush(stdout);
        fprintf(stderr, "live objects after run: %lld\n", (long long)live);
        status = live == 0 ? 0 : 3;
    }
    sl_free(vm);
    return status;
}

int main(int argc, char **argv) {
    const char *in_path = NULL;
    const char *out_path = NULL;
    const char *prelude = NULL;
    bool no_prelude = false;
    bool want_tokens = false;
    bool want_stats = false;
    bool want_check = false;
    bool check_leaks = false;
    int  script_argc = 0;
    char **script_argv = argv;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (strcmp(argv[i], "--prelude") == 0 && i + 1 < argc) prelude = argv[++i];
        else if (strcmp(argv[i], "--no-prelude") == 0) no_prelude = true;
        else if (strcmp(argv[i], "--check") == 0) want_check = true;
        else if (strcmp(argv[i], "--check-leaks") == 0) check_leaks = true;
        else if (strcmp(argv[i], "--tokens") == 0) want_tokens = true;
        else if (strcmp(argv[i], "--stats") == 0) want_stats = true;
        else if (strcmp(argv[i], "--version") == 0) { printf("solis " VERSION "\n"); return 0; }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        }
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "solis: unknown option `%s`\n", argv[i]);
            return 2;
        }
        else {
            in_path = argv[i];
            script_argv = argv + i;
            script_argc = argc - i;
            break;
        }
    }

    if (!in_path) {
        usage();
        return 2;
    }

    size_t raw_len = 0;
    uint8_t *raw = read_whole(in_path, &raw_len);
    if (!raw) {
        fprintf(stderr, "solis: cannot open %s\n", in_path);
        return 2;
    }

    if (raw_len >= 4 && memcmp(raw, "SLBC", 4) == 0) {
        if (out_path || want_check || want_tokens || want_stats) {
            fprintf(stderr, "solis: %s is already compiled\n", in_path);
            free(raw);
            return 2;
        }
        int rc = run_image(raw, raw_len, check_leaks, script_argc, script_argv);
        free(raw);
        return rc;
    }

    arena a;
    arena_init(&a);
    sl_interner *in = intern_new(&a);

    diag_vec diags;
    memset(&diags, 0, sizeof diags);

    if (want_tokens) {
        token_vec toks = lex(&a, in, (char *)raw, raw_len, &diags);
        printf("%d tokens, %d diagnostics\n", toks.len, diags.len);
        report(NULL, &diags, (char *)raw, raw_len, in_path);
        int rc = diags.len ? 1 : 0;
        arena_free(&a);
        free(raw);
        return rc;
    }
    free(raw);

    // An explicit --prelude has to exist; the automatic one doesn't.
    if (prelude && !no_prelude) {
        FILE *pf = fopen(prelude, "rb");
        if (!pf) {
            fprintf(stderr, "solis: cannot open prelude %s\n", prelude);
            arena_free(&a);
            return 2;
        }
        fclose(pf);
    }

    loaded *l = module_load(&a, in, in_path, no_prelude ? "" : prelude, &diags);

    uint8_t *bytes = NULL;
    size_t len = 0;

    if (diags.len == 0) {
        decls *d = check_modules(&a, in, l, &diags);

        if (diags.len == 0 && !want_check) {
            bc_program *prog = generate(&a, in, l, d, &diags);
            if (prog && diags.len == 0) {
                bytes = bc_serialize(prog, &len);

                if (out_path) {
                    FILE *f = fopen(out_path, "wb");
                    if (!f) {
                        fprintf(stderr, "solis: cannot write %s\n", out_path);
                        free(bytes);
                        arena_free(&a);
                        return 2;
                    }
                    fwrite(bytes, 1, len, f);
                    fclose(f);
                    printf("wrote %s: %zu bytes, %d function(s), %d constant(s), "
                           "%d native(s)\n",
                           out_path, len, prog->funcs.len, prog->consts.len,
                           prog->natives.len);
                }
                if (want_stats) {
                    printf("modules   %d\n", l->modules.len);
                    printf("functions %d\n", prog->funcs.len);
                    printf("constants %d\n", prog->consts.len);
                    printf("natives   %d\n", prog->natives.len);
                    printf("globals   %u\n", (unsigned)prog->nglobals);
                    printf("image     %zu bytes\n", len);
                    printf("arena     %zu bytes\n", a.used);
                }
            }
        }
    }

    if (diags.len) {
        report(l, &diags, "", 0, in_path);
        fprintf(stderr, "\n%d error(s)%s\n", diags.len,
                out_path ? "; nothing emitted" : "");
        free(bytes);
        arena_free(&a);
        return 1;
    }

    if (want_check) {
        printf("%s: no errors\n", in_path);
        arena_free(&a);
        return 0;
    }

    arena_free(&a);

    if (out_path || want_stats) {
        free(bytes);
        return 0;
    }

    int rc = run_image(bytes, len, check_leaks, script_argc, script_argv);
    free(bytes);
    return rc;
}
