#define _POSIX_C_SOURCE 200809L // open_memstream

#include "check.h"
#include "gen.h"
#include "module.h"
#include "parse.h"
#include "solis.h"

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *g_image     = NULL;
static size_t   g_image_len = 0;
static char     g_err[4096] = {0};

static void set_err(const char *s) {
    snprintf(g_err, sizeof g_err, "%s", s ? s : "");
}

static void render_diags(const loaded *l, const diag_vec *diags, const char *path) {
    char  *buf  = NULL;
    size_t size = 0;
    FILE  *mem  = open_memstream(&buf, &size);
    if (!mem) { set_err("out of memory"); return; }

    for (int i = 0; i < diags->len; i++) {
        const diag *d   = &diags->at[i];
        const char *src = "";
        size_t      len = 0;
        const char *p   = path;
        module *m = (d->module && l) ? module_find(l, d->module) : NULL;
        if (m) { src = m->src; len = m->src_len; p = m->path; }
        fputc('\n', mem);
        diag_render(src, len, p, d, mem);
    }
    fclose(mem);
    set_err(buf ? buf : "");
    free(buf);
}

EMSCRIPTEN_KEEPALIVE
int solis_compile(const char *path, const char *prelude_path) {
    free(g_image);
    g_image = NULL;
    g_image_len = 0;
    g_err[0] = 0;

    arena a;
    arena_init(&a);
    sl_interner *in = intern_new(&a);

    diag_vec diags;
    memset(&diags, 0, sizeof diags);

    loaded *l = module_load(&a, in, path, prelude_path, &diags);

    bc_program *prog = NULL;
    if (diags.len == 0) {
        decls *d = check_modules(&a, in, l, &diags);
        if (diags.len == 0) prog = generate(&a, in, l, d, &diags);
    }

    if (diags.len || !prog) {
        render_diags(l, &diags, path);
        arena_free(&a);
        return 1;
    }

    size_t len = 0;
    uint8_t *bytes = bc_serialize(prog, &len);
    arena_free(&a);
    if (!bytes) { set_err("codegen produced no image"); return 1; }

    g_image = bytes;
    g_image_len = len;
    return 0;
}

EMSCRIPTEN_KEEPALIVE uint8_t *solis_image_ptr(void) { return g_image; }
EMSCRIPTEN_KEEPALIVE int      solis_image_len(void) { return (int)g_image_len; }
EMSCRIPTEN_KEEPALIVE const char *solis_last_error(void) { return g_err; }

// ---- vm lifecycle ---------------------------------------------------------

EMSCRIPTEN_KEEPALIVE void *solis_vm_new(void)  { return sl_new(); }
EMSCRIPTEN_KEEPALIVE void  solis_vm_free(void *vm) { sl_free((sl_vm *)vm); }
EMSCRIPTEN_KEEPALIVE void  solis_open_std(void *vm) { sl_open_std((sl_vm *)vm); }

EMSCRIPTEN_KEEPALIVE
int solis_load(void *vm, const uint8_t *bytes, int len) {
    sl_result rc = sl_load((sl_vm *)vm, bytes, (size_t)len);
    if (rc != SL_OK) set_err(sl_error((sl_vm *)vm));
    return rc == SL_OK ? 0 : 1;
}

EMSCRIPTEN_KEEPALIVE
int solis_run(void *vm) {
    sl_result rc = sl_run((sl_vm *)vm);
    if (rc != SL_OK) set_err(sl_error((sl_vm *)vm));
    return rc == SL_OK ? 0 : 1;
}

EM_JS(void, solis_js_dispatch, (int id, void *vm, int argc, void *argv), {
    Module.__solisDispatch(id, vm, argc, argv);
});

static sl_value g_pending_result;

EMSCRIPTEN_KEEPALIVE int    solis_arg_type(void *argv, int i) { return ((sl_value *)argv)[i].type; }
EMSCRIPTEN_KEEPALIVE double solis_arg_int(void *argv, int i)   { return (double)((sl_value *)argv)[i].as.i; }
EMSCRIPTEN_KEEPALIVE double solis_arg_float(void *argv, int i) { return ((sl_value *)argv)[i].as.f; }
EMSCRIPTEN_KEEPALIVE int    solis_arg_bool(void *argv, int i)  { return ((sl_value *)argv)[i].as.b; }
EMSCRIPTEN_KEEPALIVE const char *solis_arg_cstr(void *argv, int i, int *len_out) {
    size_t n = 0;
    const char *p = sl_as_str(((sl_value *)argv)[i], &n);
    *len_out = (int)n;
    return p;
}

EMSCRIPTEN_KEEPALIVE void solis_ret_void(void)         { g_pending_result = sl_void(); }
EMSCRIPTEN_KEEPALIVE void solis_ret_int(double v)      { g_pending_result = sl_int((int64_t)v); }
EMSCRIPTEN_KEEPALIVE void solis_ret_float(double v)    { g_pending_result = sl_float(v); }
EMSCRIPTEN_KEEPALIVE void solis_ret_bool(int v)        { g_pending_result = sl_bool(v); }
EMSCRIPTEN_KEEPALIVE void solis_ret_str(void *vm, const char *s, int len) {
    g_pending_result = sl_str((sl_vm *)vm, s, len < 0 ? strlen(s) : (size_t)len);
}
EMSCRIPTEN_KEEPALIVE void solis_ret_fail(void *vm, const char *msg) {
    sl_fail((sl_vm *)vm, msg);
    g_pending_result = sl_void();
}

#define TRAMPOLINE(n) \
    static sl_value trampoline_##n(sl_vm *vm, int argc, sl_value *argv) { \
        (void)argc; \
        solis_js_dispatch(n, vm, argc, argv); \
        return g_pending_result; \
    }

TRAMPOLINE(0)  TRAMPOLINE(1)  TRAMPOLINE(2)  TRAMPOLINE(3)
TRAMPOLINE(4)  TRAMPOLINE(5)  TRAMPOLINE(6)  TRAMPOLINE(7)
TRAMPOLINE(8)  TRAMPOLINE(9)  TRAMPOLINE(10) TRAMPOLINE(11)
TRAMPOLINE(12) TRAMPOLINE(13) TRAMPOLINE(14) TRAMPOLINE(15)
TRAMPOLINE(16) TRAMPOLINE(17) TRAMPOLINE(18) TRAMPOLINE(19)
TRAMPOLINE(20) TRAMPOLINE(21) TRAMPOLINE(22) TRAMPOLINE(23)
TRAMPOLINE(24) TRAMPOLINE(25) TRAMPOLINE(26) TRAMPOLINE(27)
TRAMPOLINE(28) TRAMPOLINE(29) TRAMPOLINE(30) TRAMPOLINE(31)

#define SOLIS_NATIVE_POOL 32

static const sl_native_fn trampolines[SOLIS_NATIVE_POOL] = {
    trampoline_0,  trampoline_1,  trampoline_2,  trampoline_3,
    trampoline_4,  trampoline_5,  trampoline_6,  trampoline_7,
    trampoline_8,  trampoline_9,  trampoline_10, trampoline_11,
    trampoline_12, trampoline_13, trampoline_14, trampoline_15,
    trampoline_16, trampoline_17, trampoline_18, trampoline_19,
    trampoline_20, trampoline_21, trampoline_22, trampoline_23,
    trampoline_24, trampoline_25, trampoline_26, trampoline_27,
    trampoline_28, trampoline_29, trampoline_30, trampoline_31,
};

// Binds `name` to JS dispatch slot `id` (0..SOLIS_NATIVE_POOL-1). The JS
// wrapper owns which id maps to which callback.
EMSCRIPTEN_KEEPALIVE
int solis_register(void *vm, const char *name, int id) {
    if (id < 0 || id >= SOLIS_NATIVE_POOL) return 0;
    return sl_register((sl_vm *)vm, name, trampolines[id]) ? 1 : 0;
}
