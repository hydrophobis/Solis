// Bytecode loader and interpreter. Opcode numbers and file layout mirror
// compiler/emit.h.

#include "solis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#define SL_STACK_MAX   4096
#define SL_FRAMES_MAX  256
#define SL_NATIVES_MAX 128

enum {
    OP_CONST_I8 = 0, OP_CONST = 1, OP_TRUE = 2, OP_FALSE = 3, OP_POP = 4,
    OP_LOAD = 5, OP_STORE = 6,
    OP_DUP = 7, OP_CONST_STR = 8, OP_CONCAT = 9,

    OP_ADD_I = 10, OP_SUB_I, OP_MUL_I, OP_DIV_I, OP_REM_I, OP_NEG_I,
    OP_STR_I = 16, OP_STR_F = 17, OP_STR_B = 18, OP_STR_OBJ = 19,

    OP_ADD_F = 20, OP_SUB_F, OP_MUL_F, OP_DIV_F, OP_REM_F, OP_NEG_F,

    OP_EQ_I = 30, OP_NE_I, OP_LT_I, OP_LE_I, OP_GT_I, OP_GE_I,
    OP_EQ_F = 40, OP_NE_F, OP_LT_F, OP_LE_F, OP_GT_F, OP_GE_F,
    OP_EQ_S = 46, OP_NE_S = 47,

    OP_EQ_B = 50, OP_NE_B, OP_NOT,

    OP_JUMP = 60, OP_JUMP_IF_FALSE,

    OP_CALL = 70, OP_NATIVE, OP_RET, OP_RET_VOID,

    OP_HALT = 90,

    OP_NEW_ARRAY = 100, OP_AGET, OP_ASET, OP_ALEN, OP_APUSH,
    OP_NEW_STRUCT = 110, OP_FGET, OP_FSET,
    OP_NEW_VARIANT = 120, OP_VTAG, OP_VGET,

    OP_LOADG = 130, OP_STOREG,

    OP_DUP2 = 140, OP_COPY,

    OP_NULL = 142,

    OP_INCR_I = 150, OP_MOVE = 151, OP_ADD_RR_I = 152
};

typedef struct {
    sl_obj   head;
    size_t   len;       // bytes, not characters
    char     bytes[1];  // NUL-terminated, over-allocated
} sl_string;

typedef struct {
    sl_obj    head;
    int64_t   len;
    int64_t   cap;
    sl_value *items;
} sl_arr;

typedef struct {
    sl_obj   head;
    uint16_t nfields;
    sl_value fields[1]; // over-allocated
} sl_struct;

typedef struct {
    sl_obj   head;
    uint16_t tag;       // which variant
    uint16_t n;
    sl_value payload[1];
} sl_variant;

typedef enum { C_INT, C_FLOAT, C_STR } const_type;

typedef struct {
    const_type type;
    union { int64_t i; double f; struct { char *p; size_t n; } s; } as;
} sl_const;

typedef struct {
    char    *name;
    uint8_t  arity;
    uint16_t nslots;
    uint8_t *code;
    uint32_t code_len;
} sl_func;

typedef struct {
    sl_func  *fn;
    uint8_t  *ip;
    sl_value *slots;
} sl_frame;

struct sl_vm {
    sl_const *consts;       uint16_t nconsts;
    sl_func  *funcs;        uint16_t nfuncs;
    char    **native_names; uint16_t nnative_names;
    sl_value *globals;      uint16_t nglobals;

    struct { char *name; sl_native_fn fn; } registry[SL_NATIVES_MAX];
    int nregistered;
    int *native_bind;

    uint16_t entry;

    sl_value  stack[SL_STACK_MAX];
    sl_value *sp;
    sl_frame  frames[SL_FRAMES_MAX];
    int nframes;

    int64_t live;       // heap objects alive, for leak checks
    char err[256];
    bool failed;
};

int64_t sl_live_objects(const sl_vm *vm) { return vm->live; }

int sl_natives_free(const sl_vm *vm) { return SL_NATIVES_MAX - vm->nregistered; }

static void vm_error(sl_vm *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->err, sizeof(vm->err), fmt, ap);
    va_end(ap);
    vm->failed = true;
}

void sl_fail(sl_vm *vm, const char *msg) {
    snprintf(vm->err, sizeof(vm->err), "%s", msg ? msg : "native failed");
    vm->failed = true;
}

const char *sl_error(const sl_vm *vm) { return vm->err; }

void sl_release(sl_vm *vm, sl_value v) {
    if (v.type != SL_OBJ || !v.as.o) return;
    sl_obj *o = v.as.o;
    if (--o->rc > 0) return;

    // Releasing a container releases what it holds. Deep structures free
    // recursively; a pathological one could go deep, which is the known cost
    // of plain reference counting.
    switch (o->kind) {
        case SL_STR:
            break;
        case SL_ARRAY: {
            sl_arr *a = (sl_arr *)o;
            for (int64_t i = 0; i < a->len; i++) sl_release(vm, a->items[i]);
            free(a->items);
            break;
        }
        case SL_STRUCT: {
            sl_struct *s = (sl_struct *)o;
            for (uint16_t i = 0; i < s->nfields; i++) sl_release(vm, s->fields[i]);
            break;
        }
        case SL_VARIANT: {
            sl_variant *t = (sl_variant *)o;
            for (uint16_t i = 0; i < t->n; i++) sl_release(vm, t->payload[i]);
            break;
        }
    }
    vm->live--;
    free(o);
}

static sl_value obj_value(sl_obj *o) {
    sl_value v;
    v.type = SL_OBJ;
    v.as.o = o;
    return v;
}

sl_value sl_str(sl_vm *vm, const char *bytes, size_t n) {
    sl_string *s = (sl_string *)malloc(sizeof(sl_string) + n);
    if (!s) { vm_error(vm, "out of memory"); return sl_void(); }
    s->head.rc = 1;
    s->head.kind = SL_STR;
    s->len = n;
    if (n) memcpy(s->bytes, bytes, n);
    s->bytes[n] = 0;
    vm->live++;
    return obj_value(&s->head);
}

sl_value sl_cstr(sl_vm *vm, const char *z) { return sl_str(vm, z, strlen(z)); }

const char *sl_as_str(sl_value v, size_t *len_out) {
    if (v.type != SL_OBJ || !v.as.o || v.as.o->kind != SL_STR) {
        if (len_out) *len_out = 0;
        return "";
    }
    sl_string *s = (sl_string *)v.as.o;
    if (len_out) *len_out = s->len;
    return s->bytes;
}

static bool array_push(sl_vm *vm, sl_arr *a, sl_value item);

sl_value sl_array(sl_vm *vm, int64_t cap) {
    sl_arr *a = (sl_arr *)malloc(sizeof(sl_arr));
    if (!a) { vm_error(vm, "out of memory"); return sl_void(); }
    a->head.rc = 1;
    a->head.kind = SL_ARRAY;
    a->len = 0;
    a->cap = cap > 0 ? cap : 0;
    a->items = a->cap ? (sl_value *)malloc(sizeof(sl_value) * (size_t)a->cap) : NULL;
    if (a->cap && !a->items) { free(a); vm_error(vm, "out of memory"); return sl_void(); }
    vm->live++;
    return obj_value(&a->head);
}

bool sl_array_push(sl_vm *vm, sl_value arr, sl_value item) {
    if (arr.type != SL_OBJ || !arr.as.o || arr.as.o->kind != SL_ARRAY) {
        sl_release(vm, item);
        vm_error(vm, "not an array");
        return false;
    }
    return array_push(vm, (sl_arr *)arr.as.o, item);
}

int64_t sl_array_len(sl_value v) {
    if (v.type != SL_OBJ || !v.as.o || v.as.o->kind != SL_ARRAY) return 0;
    return ((sl_arr *)v.as.o)->len;
}

sl_value sl_array_get(sl_value v, int64_t i) {
    sl_arr *a = (sl_arr *)v.as.o;
    if (!a || i < 0 || i >= a->len) return sl_void();
    return a->items[i];
}

// Takes ownership of `item`.
static bool array_push(sl_vm *vm, sl_arr *a, sl_value item) {
    if (a->len == a->cap) {
        int64_t cap = a->cap < 8 ? 8 : a->cap * 2;
        sl_value *n = (sl_value *)realloc(a->items, sizeof(sl_value) * (size_t)cap);
        if (!n) { vm_error(vm, "out of memory"); return false; }
        a->items = n;
        a->cap = cap;
    }
    a->items[a->len++] = item;
    return true;
}

// Structs are values, so binding copies. Nested structs copy too; arrays and
// strings inside stay shared. Caller owns the result.
static sl_value struct_copy(sl_vm *vm, sl_value v) {
    if (v.type != SL_OBJ || !v.as.o || v.as.o->kind != SL_STRUCT) {
        sl_retain(v);
        return v;
    }
    sl_struct *src = (sl_struct *)v.as.o;
    uint16_t n = src->nfields;
    sl_struct *dst = (sl_struct *)malloc(sizeof(sl_struct) + (n ? n - 1 : 0) * sizeof(sl_value));
    if (!dst) { vm_error(vm, "out of memory"); return sl_void(); }
    dst->head.rc = 1;
    dst->head.kind = SL_STRUCT;
    dst->nfields = n;
    for (uint16_t i = 0; i < n; i++) dst->fields[i] = struct_copy(vm, src->fields[i]);
    vm->live++;
    return obj_value(&dst->head);
}

static void append(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t c = (*cap ? *cap : 64);
        while (c < *len + n + 1) c *= 2;
        char *p = (char *)realloc(*buf, c);
        if (!p) return;
        *buf = p;
        *cap = c;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

// Whole numbers keep a trailing .0 so they can't be mistaken for ints;
// everything else gets the shortest round-tripping form.
static void fmt_float(char *out, size_t cap, double d) {
    if (d == (double)(long long)d && d < 1e15 && d > -1e15) {
        snprintf(out, cap, "%.1f", d);
        return;
    }
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(out, cap, "%.*g", prec, d);
        if (strtod(out, NULL) == d) return;
    }
}

static void render(sl_vm *vm, sl_value v, char **buf, size_t *len, size_t *cap) {
    char tmp[64];
    switch (v.type) {
        case SL_INT:
            snprintf(tmp, sizeof tmp, "%lld", (long long)v.as.i);
            append(buf, len, cap, tmp, strlen(tmp));
            return;
        case SL_FLOAT:
            fmt_float(tmp, sizeof tmp, v.as.f);
            append(buf, len, cap, tmp, strlen(tmp));
            return;
        case SL_BOOL:
            append(buf, len, cap, v.as.b ? "true" : "false", v.as.b ? 4 : 5);
            return;
        case SL_VOID:
            append(buf, len, cap, "()", 2);
            return;
        case SL_OBJ: break;
    }
    if (!v.as.o) { append(buf, len, cap, "null", 4); return; }

    switch (v.as.o->kind) {
        case SL_STR: {
            size_t n; const char *p = sl_as_str(v, &n);
            append(buf, len, cap, p, n);
            return;
        }
        case SL_ARRAY: {
            sl_arr *a = (sl_arr *)v.as.o;
            append(buf, len, cap, "[", 1);
            for (int64_t i = 0; i < a->len; i++) {
                if (i) append(buf, len, cap, ", ", 2);
                render(vm, a->items[i], buf, len, cap);
            }
            append(buf, len, cap, "]", 1);
            return;
        }
        case SL_STRUCT: {
            sl_struct *s = (sl_struct *)v.as.o;
            append(buf, len, cap, "{", 1);
            for (uint16_t i = 0; i < s->nfields; i++) {
                if (i) append(buf, len, cap, ", ", 2);
                render(vm, s->fields[i], buf, len, cap);
            }
            append(buf, len, cap, "}", 1);
            return;
        }
        case SL_VARIANT: {
            sl_variant *t = (sl_variant *)v.as.o;
            snprintf(tmp, sizeof tmp, "#%u", (unsigned)t->tag);
            append(buf, len, cap, tmp, strlen(tmp));
            if (t->n) {
                append(buf, len, cap, "(", 1);
                for (uint16_t i = 0; i < t->n; i++) {
                    if (i) append(buf, len, cap, ", ", 2);
                    render(vm, t->payload[i], buf, len, cap);
                }
                append(buf, len, cap, ")", 1);
            }
            return;
        }
    }
}

sl_value sl_to_string(sl_vm *vm, sl_value v) {
    char *buf = NULL; size_t len = 0, cap = 0;
    render(vm, v, &buf, &len, &cap);
    sl_value out = sl_str(vm, buf ? buf : "", len);
    free(buf);
    return out;
}

typedef struct { const uint8_t *p, *end; bool bad; } reader;

static uint8_t rd_u8(reader *r) {
    if (r->p + 1 > r->end) { r->bad = true; return 0; }
    return *r->p++;
}
static uint16_t rd_u16(reader *r) {
    if (r->p + 2 > r->end) { r->bad = true; return 0; }
    uint16_t v = (uint16_t)r->p[0] | ((uint16_t)r->p[1] << 8);
    r->p += 2; return v;
}
static uint32_t rd_u32(reader *r) {
    if (r->p + 4 > r->end) { r->bad = true; return 0; }
    uint32_t v = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) |
                 ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4; return v;
}
static int64_t rd_i64(reader *r) {
    if (r->p + 8 > r->end) { r->bad = true; return 0; }
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | r->p[i];
    r->p += 8; return (int64_t)v;
}
static double rd_f64(reader *r) {
    if (r->p + 8 > r->end) { r->bad = true; return 0; }
    uint64_t bits = 0;
    for (int i = 7; i >= 0; i--) bits = (bits << 8) | r->p[i];
    r->p += 8;
    double d; memcpy(&d, &bits, sizeof d); return d;
}
static char *rd_bytes(reader *r, uint32_t len) {
    if (r->p + len > r->end) { r->bad = true; return NULL; }
    char *s = (char *)malloc(len + 1);
    if (!s) { r->bad = true; return NULL; }
    memcpy(s, r->p, len);
    s[len] = 0;
    r->p += len;
    return s;
}

// Pure arithmetic is pre-registered regardless of what the host grants; I/O
// is always the host's business. A host can shadow either by registering
// the name itself.

static sl_value intrin_sqrt(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 1) { sl_fail(vm, "sqrt expects a number"); return sl_void(); }
    double x = argv[0].type == SL_FLOAT ? argv[0].as.f : (double)argv[0].as.i;
    return sl_float(sqrt(x));
}

static sl_value intrin_abs(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 1) { sl_fail(vm, "abs expects a number"); return sl_void(); }
    if (argv[0].type == SL_FLOAT) return sl_float(argv[0].as.f < 0 ? -argv[0].as.f : argv[0].as.f);
    int64_t i = argv[0].as.i;
    return sl_int(i < 0 ? -i : i);
}

sl_vm *sl_new(void) {
    sl_vm *vm = (sl_vm *)calloc(1, sizeof(sl_vm));
    if (!vm) return NULL;
    vm->sp = vm->stack;
    sl_register(vm, "sqrt", intrin_sqrt);
    sl_register(vm, "abs", intrin_abs);
    return vm;
}

void sl_free(sl_vm *vm) {
    if (!vm) return;
    // Anything still on the stack is ours to drop.
    while (vm->sp > vm->stack) sl_release(vm, *--vm->sp);

    for (uint16_t i = 0; i < vm->nconsts; i++)
        if (vm->consts[i].type == C_STR) free(vm->consts[i].as.s.p);
    free(vm->consts);
    for (uint16_t i = 0; i < vm->nfuncs; i++) { free(vm->funcs[i].name); free(vm->funcs[i].code); }
    free(vm->funcs);
    for (uint16_t i = 0; i < vm->nglobals; i++) sl_release(vm, vm->globals[i]);
    free(vm->globals);
    for (uint16_t i = 0; i < vm->nnative_names; i++) free(vm->native_names[i]);
    free(vm->native_names);
    free(vm->native_bind);
    for (int i = 0; i < vm->nregistered; i++) free(vm->registry[i].name);
    free(vm);
}

bool sl_register(sl_vm *vm, const char *name, sl_native_fn fn) {
    if (vm->nregistered >= SL_NATIVES_MAX) return false;
    size_t n = strlen(name);
    char *copy = (char *)malloc(n + 1);
    if (!copy) return false;
    memcpy(copy, name, n + 1);
    vm->registry[vm->nregistered].name = copy;
    vm->registry[vm->nregistered].fn = fn;
    vm->nregistered++;
    return true;
}

sl_result sl_load(sl_vm *vm, const uint8_t *bytes, size_t len) {
    reader r = { bytes, bytes + len, false };

    if (len < 6 || memcmp(bytes, "SLBC", 4) != 0) {
        vm_error(vm, "not a Solis bytecode image");
        return SL_ERR_BADFILE;
    }
    r.p += 4;
    uint16_t version = rd_u16(&r);
    if (version != 3) {
        vm_error(vm, "bytecode version %u, this runtime understands 3", version);
        return SL_ERR_BADFILE;
    }

    vm->nconsts = rd_u16(&r);
    vm->consts = (sl_const *)calloc(vm->nconsts ? vm->nconsts : 1, sizeof(sl_const));
    if (!vm->consts) return SL_ERR_NOMEM;
    for (uint16_t i = 0; i < vm->nconsts && !r.bad; i++) {
        uint8_t tag = rd_u8(&r);
        switch (tag) {
            case 0: vm->consts[i].type = C_INT;   vm->consts[i].as.i = rd_i64(&r); break;
            case 1: vm->consts[i].type = C_FLOAT; vm->consts[i].as.f = rd_f64(&r); break;
            case 2: {
                uint32_t n = rd_u32(&r);
                vm->consts[i].type = C_STR;
                vm->consts[i].as.s.n = n;
                vm->consts[i].as.s.p = rd_bytes(&r, n);
                break;
            }
            default: vm_error(vm, "unknown constant tag %u", tag); return SL_ERR_BADFILE;
        }
    }

    vm->nnative_names = rd_u16(&r);
    vm->native_names = (char **)calloc(vm->nnative_names ? vm->nnative_names : 1, sizeof(char *));
    vm->native_bind  = (int  *)calloc(vm->nnative_names ? vm->nnative_names : 1, sizeof(int));
    if (!vm->native_names || !vm->native_bind) return SL_ERR_NOMEM;
    for (uint16_t i = 0; i < vm->nnative_names && !r.bad; i++) {
        uint16_t n = rd_u16(&r);
        vm->native_names[i] = rd_bytes(&r, n);
        vm->native_bind[i] = -1;
    }

    // Globals start void; the image's entry function initialises them.
    vm->nglobals = rd_u16(&r);
    vm->globals = (sl_value *)calloc(vm->nglobals ? vm->nglobals : 1, sizeof(sl_value));
    if (!vm->globals) return SL_ERR_NOMEM;
    for (uint16_t i = 0; i < vm->nglobals; i++) vm->globals[i] = sl_void();

    vm->nfuncs = rd_u16(&r);
    vm->funcs = (sl_func *)calloc(vm->nfuncs ? vm->nfuncs : 1, sizeof(sl_func));
    if (!vm->funcs) return SL_ERR_NOMEM;
    for (uint16_t i = 0; i < vm->nfuncs && !r.bad; i++) {
        uint16_t n = rd_u16(&r);
        vm->funcs[i].name = rd_bytes(&r, n);
        vm->funcs[i].arity = rd_u8(&r);
        vm->funcs[i].nslots = rd_u16(&r);
        uint32_t cl = rd_u32(&r);
        vm->funcs[i].code_len = cl;
        if (r.p + cl > r.end) { r.bad = true; break; }
        vm->funcs[i].code = (uint8_t *)malloc(cl ? cl : 1);
        if (!vm->funcs[i].code) return SL_ERR_NOMEM;
        memcpy(vm->funcs[i].code, r.p, cl);
        r.p += cl;
    }

    vm->entry = rd_u16(&r);

    if (r.bad) { vm_error(vm, "bytecode image is truncated or corrupt"); return SL_ERR_BADFILE; }
    if (vm->entry >= vm->nfuncs) { vm_error(vm, "entry function out of range"); return SL_ERR_BADFILE; }

    // Last registration wins, so a host can shadow an intrinsic.
    for (uint16_t i = 0; i < vm->nnative_names; i++)
        for (int j = vm->nregistered - 1; j >= 0; j--)
            if (strcmp(vm->native_names[i], vm->registry[j].name) == 0) { vm->native_bind[i] = j; break; }

    return SL_OK;
}

static int16_t read_i16(uint8_t **ip) {
    int16_t v = (int16_t)((uint16_t)(*ip)[0] | ((uint16_t)(*ip)[1] << 8));
    *ip += 2; return v;
}
static uint16_t read_u16(uint8_t **ip) {
    uint16_t v = (uint16_t)((*ip)[0]) | ((uint16_t)((*ip)[1]) << 8);
    *ip += 2; return v;
}

#define PUSH(v) do { \
        if (vm->sp >= vm->stack + SL_STACK_MAX) { vm_error(vm, "stack overflow"); goto fail; } \
        *vm->sp++ = (v); \
    } while (0)
#define POP() (*(--vm->sp))
#define RELEASE(v) do { sl_value _v = (v); if (_v.type == SL_OBJ) sl_release(vm, _v); } while (0)

sl_result sl_run(sl_vm *vm) {
    if (vm->nfuncs == 0) { vm_error(vm, "nothing loaded"); return SL_ERR_RUNTIME; }

    vm->sp = vm->stack;
    vm->nframes = 0;
    vm->failed = false;
    vm->err[0] = 0;

    sl_func *fn = &vm->funcs[vm->entry];
    sl_frame *fr = &vm->frames[vm->nframes++];
    fr->fn = fn; fr->ip = fn->code; fr->slots = vm->sp;
    for (uint16_t i = 0; i < fn->nslots; i++) *vm->sp++ = sl_void();

    uint8_t *ip = fr->ip;

    for (;;) {
        uint8_t op = *ip++;
        switch (op) {

        case OP_CONST_I8: PUSH(sl_int((int8_t)*ip++)); break;

        case OP_CONST: {
            uint16_t i = read_u16(&ip);
            if (i >= vm->nconsts) { vm_error(vm, "bad constant index"); goto fail; }
            sl_const *c = &vm->consts[i];
            PUSH(c->type == C_INT ? sl_int(c->as.i) : sl_float(c->as.f));
            break;
        }

        case OP_CONST_STR: {
            uint16_t i = read_u16(&ip);
            if (i >= vm->nconsts || vm->consts[i].type != C_STR) {
                vm_error(vm, "bad string constant"); goto fail;
            }
            sl_value s = sl_str(vm, vm->consts[i].as.s.p, vm->consts[i].as.s.n);
            if (vm->failed) goto fail;
            PUSH(s);
            break;
        }

        case OP_TRUE:  PUSH(sl_bool(true));  break;
        case OP_FALSE: PUSH(sl_bool(false)); break;
        case OP_POP:   RELEASE(POP()); break;

        case OP_DUP: { sl_value v = vm->sp[-1]; sl_retain(v); PUSH(v); break; }

        case OP_LOAD:  { uint8_t s = *ip++; sl_value v = fr->slots[s]; sl_retain(v); PUSH(v); break; }
        case OP_STORE: {
            uint8_t s = *ip++;
            sl_value old = fr->slots[s];
            fr->slots[s] = POP();      // the stack's reference moves into the slot
            RELEASE(old);
            break;
        }

        case OP_INCR_I: {
            uint8_t s = *ip++;
            int8_t imm = (int8_t)*ip++;
            fr->slots[s].as.i += imm;
            break;
        }
        case OP_MOVE: {
            uint8_t dst = *ip++, src = *ip++;
            fr->slots[dst] = fr->slots[src];
            break;
        }
        case OP_ADD_RR_I: {
            uint8_t dst = *ip++, s1 = *ip++, s2 = *ip++;
            fr->slots[dst] = sl_int(fr->slots[s1].as.i + fr->slots[s2].as.i);
            break;
        }

        case OP_CONCAT: {
            sl_value b = POP(), a = POP();
            size_t an, bn;
            const char *ap = sl_as_str(a, &an), *bp = sl_as_str(b, &bn);
            char *joined = (char *)malloc(an + bn + 1);
            if (!joined) { sl_release(vm, a); sl_release(vm, b); vm_error(vm, "out of memory"); goto fail; }
            memcpy(joined, ap, an);
            memcpy(joined + an, bp, bn);
            sl_value out = sl_str(vm, joined, an + bn);
            free(joined);
            sl_release(vm, a); sl_release(vm, b);
            if (vm->failed) goto fail;
            PUSH(out);
            break;
        }

        // These trust the opcode, not the tag, the same way the arithmetic
        // does. The type check already happened in the compiler. A backend
        // that emits the wrong one gets visibly wrong output instead of being
        // quietly rescued here.
        case OP_STR_I: {
            int64_t i = POP().as.i;
            char tmp[32];
            int n = snprintf(tmp, sizeof tmp, "%lld", (long long)i);
            PUSH(sl_str(vm, tmp, n > 0 ? (size_t)n : 0));
            break;
        }

        case OP_STR_F: {
            double d = POP().as.f;
            char tmp[64];
            fmt_float(tmp, sizeof tmp, d);
            PUSH(sl_str(vm, tmp, strlen(tmp)));
            break;
        }

        case OP_STR_B: {
            bool b = POP().as.b;
            PUSH(b ? sl_str(vm, "true", 4) : sl_str(vm, "false", 5));
            break;
        }

        // The generic case: arrays, structs, enums. Rendering those is a
        // structural walk whatever the static type says, so there's nothing to
        // specialise. Only emitted for reference types.
        case OP_STR_OBJ: {
            sl_value v = POP();
            sl_value s = sl_to_string(vm, v);
            sl_release(vm, v);
            if (vm->failed) goto fail;
            PUSH(s);
            break;
        }

        case OP_ADD_I: { int64_t b = POP().as.i, a = POP().as.i; PUSH(sl_int(a + b)); break; }
        case OP_SUB_I: { int64_t b = POP().as.i, a = POP().as.i; PUSH(sl_int(a - b)); break; }
        case OP_MUL_I: { int64_t b = POP().as.i, a = POP().as.i; PUSH(sl_int(a * b)); break; }
        case OP_DIV_I: {
            int64_t b = POP().as.i, a = POP().as.i;
            if (b == 0) { vm_error(vm, "division by zero"); goto fail; }
            PUSH(sl_int(a / b)); break;
        }
        case OP_REM_I: {
            int64_t b = POP().as.i, a = POP().as.i;
            if (b == 0) { vm_error(vm, "remainder by zero"); goto fail; }
            PUSH(sl_int(a % b)); break;
        }
        case OP_NEG_I: { int64_t a = POP().as.i; PUSH(sl_int(-a)); break; }

        case OP_ADD_F: { double b = POP().as.f, a = POP().as.f; PUSH(sl_float(a + b)); break; }
        case OP_SUB_F: { double b = POP().as.f, a = POP().as.f; PUSH(sl_float(a - b)); break; }
        case OP_MUL_F: { double b = POP().as.f, a = POP().as.f; PUSH(sl_float(a * b)); break; }
        case OP_DIV_F: { double b = POP().as.f, a = POP().as.f; PUSH(sl_float(a / b)); break; }
        case OP_REM_F: { double b = POP().as.f, a = POP().as.f; PUSH(sl_float(fmod(a, b))); break; }
        case OP_NEG_F: { double a = POP().as.f; PUSH(sl_float(-a)); break; }

        case OP_EQ_I: { int64_t b = POP().as.i, a = POP().as.i; PUSH(sl_bool(a == b)); break; }
        case OP_NE_I: { int64_t b = POP().as.i, a = POP().as.i; PUSH(sl_bool(a != b)); break; }
        case OP_LT_I: { int64_t b = POP().as.i, a = POP().as.i; PUSH(sl_bool(a <  b)); break; }
        case OP_LE_I: { int64_t b = POP().as.i, a = POP().as.i; PUSH(sl_bool(a <= b)); break; }
        case OP_GT_I: { int64_t b = POP().as.i, a = POP().as.i; PUSH(sl_bool(a >  b)); break; }
        case OP_GE_I: { int64_t b = POP().as.i, a = POP().as.i; PUSH(sl_bool(a >= b)); break; }

        case OP_EQ_F: { double b = POP().as.f, a = POP().as.f; PUSH(sl_bool(a == b)); break; }
        case OP_NE_F: { double b = POP().as.f, a = POP().as.f; PUSH(sl_bool(a != b)); break; }
        case OP_LT_F: { double b = POP().as.f, a = POP().as.f; PUSH(sl_bool(a <  b)); break; }
        case OP_LE_F: { double b = POP().as.f, a = POP().as.f; PUSH(sl_bool(a <= b)); break; }
        case OP_GT_F: { double b = POP().as.f, a = POP().as.f; PUSH(sl_bool(a >  b)); break; }
        case OP_GE_F: { double b = POP().as.f, a = POP().as.f; PUSH(sl_bool(a >= b)); break; }

        case OP_EQ_S: case OP_NE_S: {
            sl_value b = POP(), a = POP();
            size_t an, bn;
            const char *ap = sl_as_str(a, &an), *bp = sl_as_str(b, &bn);
            bool eq = (an == bn) && memcmp(ap, bp, an) == 0;
            sl_release(vm, a); sl_release(vm, b);
            PUSH(sl_bool(op == OP_EQ_S ? eq : !eq));
            break;
        }

        case OP_EQ_B: { bool b = POP().as.b, a = POP().as.b; PUSH(sl_bool(a == b)); break; }
        case OP_NE_B: { bool b = POP().as.b, a = POP().as.b; PUSH(sl_bool(a != b)); break; }
        case OP_NOT:  { bool a = POP().as.b; PUSH(sl_bool(!a)); break; }

        case OP_JUMP: { int16_t d = read_i16(&ip); ip += d; break; }
        case OP_JUMP_IF_FALSE: {
            int16_t d = read_i16(&ip);
            if (!POP().as.b) ip += d;
            break;
        }

        case OP_NEW_ARRAY: {
            uint16_t n = read_u16(&ip);
            sl_value av = sl_array(vm, n);
            if (vm->failed) goto fail;
            sl_arr *a = (sl_arr *)av.as.o;
            // The elements are on the stack in order; move them in.
            sl_value *base = vm->sp - n;
            for (uint16_t i = 0; i < n; i++) {
                if (!array_push(vm, a, base[i])) { sl_release(vm, av); goto fail; }
            }
            vm->sp = base;
            PUSH(av);
            break;
        }

        case OP_AGET: {
            sl_value iv = POP(), av = POP();
            if (av.type != SL_OBJ || !av.as.o || av.as.o->kind != SL_ARRAY) {
                sl_release(vm, av); vm_error(vm, "not an array"); goto fail;
            }
            sl_arr *a = (sl_arr *)av.as.o;
            int64_t i = iv.as.i;
            if (i < 0 || i >= a->len) {
                vm_error(vm, "index %lld is out of bounds for an array of length %lld",
                         (long long)i, (long long)a->len);
                sl_release(vm, av); goto fail;
            }
            sl_value e = a->items[i];
            sl_retain(e);
            sl_release(vm, av);
            PUSH(e);
            break;
        }

        case OP_ASET: {
            sl_value val = POP(), iv = POP(), av = POP();
            if (av.type != SL_OBJ || !av.as.o || av.as.o->kind != SL_ARRAY) {
                sl_release(vm, val); sl_release(vm, av);
                vm_error(vm, "not an array"); goto fail;
            }
            sl_arr *a = (sl_arr *)av.as.o;
            int64_t i = iv.as.i;
            if (i < 0 || i >= a->len) {
                vm_error(vm, "index %lld is out of bounds for an array of length %lld",
                         (long long)i, (long long)a->len);
                sl_release(vm, val); sl_release(vm, av); goto fail;
            }
            sl_release(vm, a->items[i]);
            a->items[i] = val;          // ownership moves into the array
            sl_release(vm, av);
            break;
        }

        case OP_ALEN: {
            sl_value av = POP();
            int64_t n;
            if (av.type == SL_OBJ && av.as.o && av.as.o->kind == SL_STR) {
                // len() on a string counts UTF-8 characters, not bytes.
                size_t bn; const char *p = sl_as_str(av, &bn);
                n = 0;
                for (size_t k = 0; k < bn; k++) if ((p[k] & 0xC0) != 0x80) n++;
            } else {
                n = sl_array_len(av);
            }
            sl_release(vm, av);
            PUSH(sl_int(n));
            break;
        }

        case OP_APUSH: {
            sl_value val = POP(), av = POP();
            if (av.type != SL_OBJ || !av.as.o || av.as.o->kind != SL_ARRAY) {
                sl_release(vm, val); sl_release(vm, av);
                vm_error(vm, "not an array"); goto fail;
            }
            if (!array_push(vm, (sl_arr *)av.as.o, val)) { sl_release(vm, av); goto fail; }
            sl_release(vm, av);
            break;
        }

        case OP_NEW_STRUCT: {
            uint16_t n = read_u16(&ip);
            sl_struct *s = (sl_struct *)malloc(sizeof(sl_struct) + (n ? n - 1 : 0) * sizeof(sl_value));
            if (!s) { vm_error(vm, "out of memory"); goto fail; }
            s->head.rc = 1; s->head.kind = SL_STRUCT; s->nfields = n;
            sl_value *base = vm->sp - n;
            for (uint16_t i = 0; i < n; i++) s->fields[i] = base[i];
            vm->sp = base;
            vm->live++;
            PUSH(obj_value(&s->head));
            break;
        }

        case OP_FGET: {
            uint8_t idx = *ip++;
            sl_value sv = POP();
            if (sv.type != SL_OBJ || !sv.as.o || sv.as.o->kind != SL_STRUCT) {
                sl_release(vm, sv); vm_error(vm, "not a struct"); goto fail;
            }
            sl_struct *s = (sl_struct *)sv.as.o;
            if (idx >= s->nfields) { sl_release(vm, sv); vm_error(vm, "bad field index"); goto fail; }
            sl_value f = s->fields[idx];
            sl_retain(f);
            sl_release(vm, sv);
            PUSH(f);
            break;
        }

        case OP_FSET: {
            uint8_t idx = *ip++;
            sl_value val = POP(), sv = POP();
            if (sv.type != SL_OBJ || !sv.as.o || sv.as.o->kind != SL_STRUCT) {
                sl_release(vm, val); sl_release(vm, sv);
                vm_error(vm, "not a struct"); goto fail;
            }
            sl_struct *s = (sl_struct *)sv.as.o;
            if (idx >= s->nfields) {
                sl_release(vm, val); sl_release(vm, sv);
                vm_error(vm, "bad field index"); goto fail;
            }
            sl_release(vm, s->fields[idx]);
            s->fields[idx] = val;
            sl_release(vm, sv);
            break;
        }

        case OP_NEW_VARIANT: {
            uint16_t tag = read_u16(&ip);
            uint8_t n = *ip++;
            sl_variant *t = (sl_variant *)malloc(sizeof(sl_variant) + (n ? n - 1 : 0) * sizeof(sl_value));
            if (!t) { vm_error(vm, "out of memory"); goto fail; }
            t->head.rc = 1; t->head.kind = SL_VARIANT; t->tag = tag; t->n = n;
            sl_value *base = vm->sp - n;
            for (uint8_t i = 0; i < n; i++) t->payload[i] = base[i];
            vm->sp = base;
            vm->live++;
            PUSH(obj_value(&t->head));
            break;
        }

        case OP_VTAG: {
            sl_value v = POP();
            if (v.type != SL_OBJ || !v.as.o || v.as.o->kind != SL_VARIANT) {
                sl_release(vm, v); vm_error(vm, "not an enum value"); goto fail;
            }
            uint16_t tag = ((sl_variant *)v.as.o)->tag;
            sl_release(vm, v);
            PUSH(sl_int(tag));
            break;
        }

        case OP_VGET: {
            uint8_t idx = *ip++;
            sl_value v = POP();
            if (v.type != SL_OBJ || !v.as.o || v.as.o->kind != SL_VARIANT) {
                sl_release(vm, v); vm_error(vm, "not an enum value"); goto fail;
            }
            sl_variant *t = (sl_variant *)v.as.o;
            if (idx >= t->n) { sl_release(vm, v); vm_error(vm, "bad payload index"); goto fail; }
            sl_value e = t->payload[idx];
            sl_retain(e);
            sl_release(vm, v);
            PUSH(e);
            break;
        }

        case OP_COPY: {
            sl_value v = POP();
            sl_value c = struct_copy(vm, v);
            sl_release(vm, v);
            PUSH(c);
            break;
        }

        case OP_DUP2: {
            sl_value a = vm->sp[-2], b = vm->sp[-1];
            sl_retain(a); sl_retain(b);
            PUSH(a); PUSH(b);
            break;
        }

        case OP_NULL: {
            sl_value v;
            v.type = SL_OBJ;
            v.as.o = NULL;
            PUSH(v);
            break;
        }

        case OP_LOADG: {
            uint16_t g = read_u16(&ip);
            if (g >= vm->nglobals) { vm_error(vm, "bad global index"); goto fail; }
            sl_value v = vm->globals[g];
            sl_retain(v);
            PUSH(v);
            break;
        }

        case OP_STOREG: {
            uint16_t g = read_u16(&ip);
            if (g >= vm->nglobals) { vm_error(vm, "bad global index"); goto fail; }
            sl_value v = POP();
            sl_release(vm, vm->globals[g]);
            vm->globals[g] = v;         // ownership moves out of the stack
            break;
        }

        case OP_CALL: {
            uint16_t id = read_u16(&ip);
            uint8_t argc = *ip++;
            if (id >= vm->nfuncs) { vm_error(vm, "bad function index"); goto fail; }
            if (vm->nframes >= SL_FRAMES_MAX) {
                vm_error(vm, "call depth exceeded %d (infinite recursion?)", SL_FRAMES_MAX);
                goto fail;
            }
            sl_func *callee = &vm->funcs[id];
            fr->ip = ip;
            sl_value *args = vm->sp - argc;
            if (args + callee->nslots > vm->stack + SL_STACK_MAX) {
                vm_error(vm, "stack overflow"); goto fail;
            }
            sl_frame *nf = &vm->frames[vm->nframes++];
            nf->fn = callee; nf->ip = callee->code; nf->slots = args;
            for (uint16_t s = argc; s < callee->nslots; s++) args[s] = sl_void();
            vm->sp = args + callee->nslots;
            fr = nf;
            ip = nf->ip;
            break;
        }

        case OP_NATIVE: {
            uint16_t idx = read_u16(&ip);
            uint8_t argc = *ip++;
            if (idx >= vm->nnative_names) { vm_error(vm, "bad native index"); goto fail; }
            int slot = vm->native_bind[idx];
            if (slot < 0) {
                vm_error(vm, "`%s` is declared extern but the host did not register it",
                         vm->native_names[idx]);
                goto fail;
            }
            sl_value *argv = vm->sp - argc;
            sl_value result = vm->registry[slot].fn(vm, argc, argv);
            for (uint8_t i = 0; i < argc; i++) sl_release(vm, argv[i]);
            vm->sp = argv;
            if (vm->failed) { sl_release(vm, result); goto fail; }
            if (result.type != SL_VOID) PUSH(result);
            break;
        }

        case OP_RET:
        case OP_RET_VOID: {
            sl_value result = (op == OP_RET) ? POP() : sl_void();
            // Drop everything this frame owned before unwinding.
            for (sl_value *p = fr->slots; p < vm->sp; p++) sl_release(vm, *p);
            vm->nframes--;
            vm->sp = fr->slots;
            if (vm->nframes == 0) { sl_release(vm, result); return SL_OK; }
            fr = &vm->frames[vm->nframes - 1];
            ip = fr->ip;
            if (op == OP_RET) PUSH(result);
            break;
        }

        case OP_HALT: goto done;

        default:
            vm_error(vm, "unknown opcode %u", op);
            goto fail;
        }
    }

done:
    while (vm->sp > vm->stack) sl_release(vm, *--vm->sp);
    return SL_OK;

fail:
    // Unwind every frame so a fault does not leak the whole stack.
    while (vm->sp > vm->stack) sl_release(vm, *--vm->sp);
    vm->nframes = 0;
    return SL_ERR_RUNTIME;
}
