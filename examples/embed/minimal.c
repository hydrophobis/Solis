#include "solis.h"
#include "solis_compile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sl_value native_print(sl_vm *vm, int argc, sl_value *argv) {
    for (int i = 0; i < argc; i++) {
        sl_value s = sl_to_string(vm, argv[i]);  // owned by the caller
        size_t n;
        const char *p = sl_as_str(s, &n);
        if (i) putchar(' ');
        fwrite(p, 1, n, stdout);
        sl_release(vm, s);
    }
    putchar('\n');
    return sl_void();
}

static sl_value native_now_ms(sl_vm *vm, int argc, sl_value *argv) {
    (void)vm; (void)argc; (void)argv;
    return sl_int(0);
}

static sl_value native_host_name(sl_vm *vm, int argc, sl_value *argv) {
    (void)argc; (void)argv;
    return sl_cstr(vm, "minimal.c");
}

static sl_value native_repeat(sl_vm *vm, int argc, sl_value *argv) {
    size_t n = 0;
    const char *s = argc >= 2 && argv[1].type == SL_INT ? sl_as_str(argv[0], &n) : NULL;
    if (!s) {
        sl_fail(vm, "repeat(str, int) expects a string and an int");
        return sl_void();
    }
    size_t times = argv[1].as.i > 0 ? (size_t)argv[1].as.i : 0;
    char *buf = (char *)malloc(n * times + 1);
    if (!buf) {
        sl_fail(vm, "out of memory");
        return sl_void();
    }
    for (size_t i = 0; i < times; i++) memcpy(buf + i * n, s, n);
    sl_value out = sl_str(vm, buf, n * times);  // copies buf
    free(buf);
    return out;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: minimal <script.sl>\n");
        return 2;
    }

    sl_vm *vm = sl_new();
    sl_open_std(vm);
    sl_register(vm, "print", native_print);
    sl_register_sig(vm, "nowMs", "()->int", native_now_ms);
    sl_register_sig(vm, "hostName", "()->str", native_host_name);
    sl_register_sig(vm, "repeat", "(str,int)->str", native_repeat);

    char *host = sl_declarations(vm);
    sl_compiled c;
    int bad = sl_compile_file_with_host(argv[1], NULL, host, &c);
    free(host);
    if (bad != 0) {
        fputs(c.errors ? c.errors : "compile failed\n", stderr);
        sl_compiled_free(&c);
        sl_free(vm);
        return 1;
    }

    int status = 0;
    if (sl_load(vm, c.image, c.image_len) != SL_OK) {
        fprintf(stderr, "load error: %s\n", sl_error(vm));
        status = 1;
    } else if (sl_run(vm) != SL_OK) {
        fflush(stdout);
        fprintf(stderr, "runtime error: %s\n", sl_error(vm));
        status = 1;
    }

    sl_compiled_free(&c);  // sl_load copies the image
    sl_free(vm);
    return status;
}
