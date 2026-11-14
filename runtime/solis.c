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
#define SL_NATIVES_MAX 256
#define SL_POOL_FIELDS 8   // structs up to this many fields are recycled, not freed

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

    OP_JUMP = 60, OP_JUMP_IF_FALSE, OP_JUMP_IF_TRUE,

    OP_CALL = 70, OP_NATIVE, OP_RET, OP_RET_VOID,

    OP_HALT = 90,

    OP_NEW_ARRAY = 100, OP_AGET, OP_ASET, OP_ALEN, OP_APUSH,
    OP_NEW_STRUCT = 110, OP_FGET, OP_FSET,
    OP_NEW_VARIANT = 120, OP_VTAG, OP_VGET,

    OP_LOADG = 130, OP_STOREG,

    OP_DUP2 = 140, OP_COPY,

    OP_NULL = 142,

    OP_INCR_I = 150, OP_MOVE = 151, OP_ADD_RR_I = 152,
    OP_SLICE = 153,
    OP_NEW_MAP = 154, OP_MGET = 155, OP_MSET = 156
};

typedef struct {
    sl_obj   head;
    size_t   len;       // bytes, not characters
    char     bytes[1];  // NUL-terminated, over-allocated
} sl_string;

// Arrays store their elements packed when they are all ints, all floats or all
// bools, and as tagged values otherwise. The kind is decided by the first
// element and falls back to tagged values if a different kind ever arrives.
enum { AK_NONE, AK_INT, AK_FLT, AK_BOOL, AK_GEN };

typedef struct {
    sl_obj    head;
    int64_t   len;
    int64_t   cap;
    void     *data;
    uint8_t   ek;
} sl_arr;

static inline size_t ak_size(int ek) {
    static const uint8_t sizes[] = { 0, 8, 8, 1, sizeof(sl_value) };
    return sizes[ek];
}
static inline int ak_of(sl_value v) {
    switch (v.type) {
        case SL_INT:   return AK_INT;
        case SL_FLOAT: return AK_FLT;
        case SL_BOOL:  return AK_BOOL;
        default:       return AK_GEN;
    }
}

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

typedef struct { sl_value key, val; uint64_t hash; } map_pair;

typedef struct {
    sl_obj    head;
    int64_t   len;
    int64_t   cap;
    map_pair *pairs;        // dense, in insertion order
    uint32_t *index;        // open-addressed, pair index + 1, 0 = empty; NULL while small
    uint32_t  index_cap;    // a power of two
} sl_map_obj;

typedef enum { C_INT, C_FLOAT, C_STR } const_type;

typedef struct {
    const_type type;
    union { int64_t i; double f; struct { char *p; size_t n; } s; } as;
} sl_const;

typedef struct {
    char    *name;
    uint8_t  arity;
    uint16_t nslots;
    uint8_t *code;          // stack code, from the image; freed once translated
    uint32_t code_len;
    uint16_t *rcode;        // register code, see below
    uint32_t rlen;
    uint16_t nregs;         // frame size: locals plus temporaries
} sl_func;

typedef struct {
    sl_func  *fn;
    uint16_t *ip;       // where to resume; only meaningful for a frame that has called
    sl_value *base;
} sl_frame;

struct sl_vm {
    sl_const *consts;       uint16_t nconsts;
    sl_func  *funcs;        uint16_t nfuncs;
    char    **native_names; uint16_t nnative_names;
    char    **native_sigs;
    sl_value *globals;      uint16_t nglobals;

    struct { char *name; char *sig; sl_native_fn fn; } registry[SL_NATIVES_MAX];
    int nregistered;
    int *native_bind;

    uint16_t entry;
    uint16_t nstructs;
    uint8_t (*struct_desc)[33];     // per struct type: field count, then field kinds

    sl_value  stack[SL_STACK_MAX];
    sl_value *sp;
    sl_frame  frames[SL_FRAMES_MAX];
    int nframes;

    sl_value  ret;      // what the function that just returned gave back; owned by the VM until sl_call hands it over
    bool      running;  // inside sl_run or sl_call; they cannot be nested
    bool      ready;    // sl_run has finished, so the globals are initialised and functions can be called by name

    int64_t live;       // heap objects alive, for leak checks
    sl_value *kstr;     // shared string objects for the image's string constants, made on first use
    int64_t   nkstr;    // how many of them exist, so leak checks can ignore them
    void   *free_struct[SL_POOL_FIELDS + 1];   // recycled small structs, by field count
    char err[1024];
    bool failed;
};

int64_t sl_live_objects(const sl_vm *vm) { return vm->live - vm->nkstr; }

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

// Values move as a 4-byte tag and an 8-byte payload, never as one 16-byte
// copy. Most writes are scalar, and a vector load that spans two recent
// scalar stores cannot be forwarded from the store buffer, which stalls every
// read that follows a write to the same register.
static inline sl_value vload(const sl_value *p) {
    sl_value v;
    v.type = p->type;
    v.as.i = p->as.i;
    return v;
}
static inline void vstore(sl_value *p, sl_value v) {
    p->type = v.type;
    p->as.i = v.as.i;
}

static inline sl_value arr_get(const sl_arr *a, int64_t i) {
    sl_value v;
    switch (a->ek) {
        case AK_INT:  v.type = SL_INT;   v.as.i = ((const int64_t *)a->data)[i]; break;
        case AK_FLT:  v.type = SL_FLOAT; v.as.f = ((const double *)a->data)[i]; break;
        case AK_BOOL: v.type = SL_BOOL;  v.as.i = ((const uint8_t *)a->data)[i]; break;
        default:      v = vload(&((const sl_value *)a->data)[i]);
    }
    return v;
}
// Store a value of the array's own kind.
static inline void arr_put(sl_arr *a, int64_t i, sl_value v) {
    switch (a->ek) {
        case AK_INT:  ((int64_t *)a->data)[i] = v.as.i; break;
        case AK_FLT:  ((double *)a->data)[i] = v.as.f; break;
        case AK_BOOL: ((uint8_t *)a->data)[i] = (uint8_t)(v.as.i != 0); break;
        default:      vstore(&((sl_value *)a->data)[i], v);
    }
}

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
            if (a->ek == AK_GEN)
                for (int64_t i = 0; i < a->len; i++) sl_release(vm, ((sl_value *)a->data)[i]);
            free(a->data);
            break;
        }
        case SL_STRUCT: {
            sl_struct *s = (sl_struct *)o;
            for (uint16_t i = 0; i < s->nfields; i++)
                if (s->fields[i].type == SL_OBJ) sl_release(vm, s->fields[i]);
            if (s->nfields <= SL_POOL_FIELDS) {
                *(void **)s = vm->free_struct[s->nfields];
                vm->free_struct[s->nfields] = s;
                vm->live--;
                return;
            }
            break;
        }
        case SL_VARIANT: {
            sl_variant *t = (sl_variant *)o;
            for (uint16_t i = 0; i < t->n; i++) sl_release(vm, t->payload[i]);
            break;
        }
        case SL_MAP: {
            sl_map_obj *m = (sl_map_obj *)o;
            for (int64_t i = 0; i < m->len; i++) {
                sl_release(vm, m->pairs[i].key);
                sl_release(vm, m->pairs[i].val);
            }
            free(m->pairs);
            free(m->index);
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

// Structs are small and short-lived, and malloc dominates building one, so
// freed ones are kept on a list per field count and handed back out.
static sl_struct *struct_alloc(sl_vm *vm, uint16_t n) {
    if (n <= SL_POOL_FIELDS && vm->free_struct[n]) {
        sl_struct *s = (sl_struct *)vm->free_struct[n];
        vm->free_struct[n] = *(void **)s;
        return s;
    }
    return (sl_struct *)malloc(sizeof(sl_struct) + (n ? n - 1 : 0) * sizeof(sl_value));
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

// A string of `n` bytes whose contents the caller fills in (and NUL-terminates).
static sl_string *str_alloc(sl_vm *vm, size_t n) {
    sl_string *s = (sl_string *)malloc(sizeof(sl_string) + n);
    if (!s) { vm_error(vm, "out of memory"); return NULL; }
    s->head.rc = 1;
    s->head.kind = SL_STR;
    s->len = n;
    s->bytes[n] = 0;
    vm->live++;
    return s;
}

// Strings never change after they are made, so a constant is one shared object.
static sl_value const_str(sl_vm *vm, uint16_t k) {
    if (!vm->kstr) vm->kstr = (sl_value *)calloc(vm->nconsts ? vm->nconsts : 1, sizeof(sl_value));
    if (vm->kstr && vm->kstr[k].type == SL_OBJ) {
        sl_value v = vm->kstr[k];
        v.as.o->rc++;
        return v;
    }
    sl_value v = sl_str(vm, vm->consts[k].as.s.p, vm->consts[k].as.s.n);
    if (vm->failed || !vm->kstr) return v;
    v.as.o->rc++;            // the cache's own reference
    vm->kstr[k] = v;
    vm->nkstr++;
    return v;
}

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
    a->cap = cap > 0 ? cap : 0;      // storage is allocated by the first element
    a->data = NULL;
    a->ek = AK_NONE;
    vm->live++;
    return obj_value(&a->head);
}

// Switch to tagged storage.
static bool arr_generify(sl_vm *vm, sl_arr *a) {
    if (a->ek == AK_GEN) return true;
    int64_t cap = a->cap > a->len ? a->cap : (a->len ? a->len : 1);
    sl_value *n = (sl_value *)malloc(sizeof(sl_value) * (size_t)cap);
    if (!n) { vm_error(vm, "out of memory"); return false; }
    for (int64_t i = 0; i < a->len; i++) vstore(&n[i], arr_get(a, i));
    free(a->data);
    a->data = n; a->cap = cap; a->ek = AK_GEN;
    return true;
}

// Make room for one more element of `kind` (an AK_ value).
static bool arr_room(sl_vm *vm, sl_arr *a, int kind) {
    if (a->ek == AK_NONE) {
        a->ek = (uint8_t)kind;
        int64_t cap = a->cap > 0 ? a->cap : 8;
        a->data = malloc(ak_size(kind) * (size_t)cap);
        if (!a->data) { a->ek = AK_NONE; vm_error(vm, "out of memory"); return false; }
        a->cap = cap;
        return true;
    }
    if (a->ek != kind && a->ek != AK_GEN && !arr_generify(vm, a)) return false;
    if (a->len == a->cap) {
        int64_t cap = a->cap < 8 ? 8 : a->cap * 2;
        void *n = realloc(a->data, ak_size(a->ek) * (size_t)cap);
        if (!n) { vm_error(vm, "out of memory"); return false; }
        a->data = n;
        a->cap = cap;
    }
    return true;
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
    return arr_get(a, i);
}

bool sl_array_set(sl_vm *vm, sl_value arr, int64_t i, sl_value item) {
    if (arr.type != SL_OBJ || !arr.as.o || arr.as.o->kind != SL_ARRAY) {
        sl_release(vm, item);
        vm_error(vm, "not an array");
        return false;
    }
    sl_arr *a = (sl_arr *)arr.as.o;
    if (i < 0 || i >= a->len) {
        sl_release(vm, item);
        vm_error(vm, "index %lld is out of bounds for an array of length %lld",
                 (long long)i, (long long)a->len);
        return false;
    }
    if (a->ek != AK_GEN && a->ek != ak_of(item) && !arr_generify(vm, a)) { sl_release(vm, item); return false; }
    sl_value old = arr_get(a, i);
    arr_put(a, i, item);
    sl_release(vm, old);
    return true;
}

bool sl_array_pop(sl_vm *vm, sl_value arr, sl_value *out) {
    if (arr.type != SL_OBJ || !arr.as.o || arr.as.o->kind != SL_ARRAY) {
        vm_error(vm, "not an array");
        return false;
    }
    sl_arr *a = (sl_arr *)arr.as.o;
    if (a->len == 0) {
        vm_error(vm, "pop on an empty array");
        return false;
    }
    *out = arr_get(a, --a->len);
    return true;
}

bool sl_array_remove(sl_vm *vm, sl_value arr, int64_t i, sl_value *out) {
    if (arr.type != SL_OBJ || !arr.as.o || arr.as.o->kind != SL_ARRAY) {
        vm_error(vm, "not an array");
        return false;
    }
    sl_arr *a = (sl_arr *)arr.as.o;
    if (i < 0 || i >= a->len) {
        vm_error(vm, "index %lld is out of bounds for an array of length %lld",
                 (long long)i, (long long)a->len);
        return false;
    }
    *out = arr_get(a, i);
    size_t sz = ak_size(a->ek);
    memmove((char *)a->data + (size_t)i * sz, (char *)a->data + (size_t)(i + 1) * sz, (size_t)(a->len - i - 1) * sz);
    a->len--;
    return true;
}

bool sl_array_insert(sl_vm *vm, sl_value arr, int64_t i, sl_value item) {
    if (arr.type != SL_OBJ || !arr.as.o || arr.as.o->kind != SL_ARRAY) {
        sl_release(vm, item);
        vm_error(vm, "not an array");
        return false;
    }
    sl_arr *a = (sl_arr *)arr.as.o;
    if (i < 0 || i > a->len) {
        sl_release(vm, item);
        vm_error(vm, "index %lld is out of bounds for an array of length %lld",
                 (long long)i, (long long)a->len);
        return false;
    }
    if (!arr_room(vm, a, ak_of(item))) { sl_release(vm, item); return false; }
    size_t sz = ak_size(a->ek);
    memmove((char *)a->data + (size_t)(i + 1) * sz, (char *)a->data + (size_t)i * sz, (size_t)(a->len - i) * sz);
    a->len++;
    arr_put(a, i, item);
    return true;
}

static bool sl_value_eq(sl_value a, sl_value b);

// Maps up to this size are scanned; past it, lookups go through `index`.
#define MAP_SCAN_MAX 8

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// Equal keys must hash equal: see sl_value_eq. Keys that are never equal to
// anything (structs, arrays) can hash to anything.
static uint64_t map_hash(sl_value k) {
    switch (k.type) {
        case SL_INT:   return mix64((uint64_t)k.as.i);
        case SL_FLOAT: {
            if (k.as.f == 0) return mix64(0);       // -0.0 == 0.0
            uint64_t bits;
            memcpy(&bits, &k.as.f, sizeof bits);
            return mix64(bits ^ 0x9e3779b97f4a7c15ULL);
        }
        case SL_BOOL:  return mix64((uint64_t)(k.as.b ? 1 : 2) + 0x1234567ULL);
        case SL_OBJ: {
            if (!k.as.o) return mix64(0xdeadbeefULL);
            if (k.as.o->kind != SL_STR) return 0;
            const sl_string *st = (const sl_string *)k.as.o;
            uint64_t h = 1469598103934665603ULL;     // FNV-1a
            for (size_t i = 0; i < st->len; i++) h = (h ^ (uint8_t)st->bytes[i]) * 1099511628211ULL;
            return mix64(h);
        }
        default: return 0;
    }
}

static void map_index_insert(sl_map_obj *mo, int64_t at) {
    uint32_t mask = mo->index_cap - 1;
    uint32_t slot = (uint32_t)mo->pairs[at].hash & mask;
    while (mo->index[slot]) slot = (slot + 1) & mask;
    mo->index[slot] = (uint32_t)at + 1;
}

// Rebuild the index from the pairs. False only if memory ran out, in which
// case the map is still correct, just scanned.
static bool map_reindex(sl_map_obj *mo) {
    free(mo->index);
    mo->index = NULL;
    mo->index_cap = 0;
    if (mo->len <= MAP_SCAN_MAX) return true;
    uint32_t cap = 16;
    while (cap < (uint64_t)mo->len * 2) cap <<= 1;
    mo->index = (uint32_t *)calloc(cap, sizeof(uint32_t));
    if (!mo->index) return false;
    mo->index_cap = cap;
    for (int64_t i = 0; i < mo->len; i++) map_index_insert(mo, i);
    return true;
}

// Position of `key` in mo->pairs, or -1.
static int64_t map_find(const sl_map_obj *mo, sl_value key, uint64_t h) {
    if (key.type == SL_INT) {            // the common case, without the generic comparison
        int64_t k = key.as.i;
        if (mo->index) {
            uint32_t mask = mo->index_cap - 1;
            for (uint32_t slot = (uint32_t)h & mask; mo->index[slot]; slot = (slot + 1) & mask) {
                const map_pair *p = &mo->pairs[mo->index[slot] - 1];
                if (p->key.type == SL_INT && p->key.as.i == k) return mo->index[slot] - 1;
            }
            return -1;
        }
        for (int64_t i = 0; i < mo->len; i++)
            if (mo->pairs[i].key.type == SL_INT && mo->pairs[i].key.as.i == k) return i;
        return -1;
    }
    if (mo->index) {
        uint32_t mask = mo->index_cap - 1;
        for (uint32_t slot = (uint32_t)h & mask; mo->index[slot]; slot = (slot + 1) & mask) {
            const map_pair *p = &mo->pairs[mo->index[slot] - 1];
            if (p->hash == h && sl_value_eq(p->key, key)) return mo->index[slot] - 1;
        }
        return -1;
    }
    for (int64_t i = 0; i < mo->len; i++)
        if (mo->pairs[i].hash == h && sl_value_eq(mo->pairs[i].key, key)) return i;
    return -1;
}

sl_value sl_map(sl_vm *vm, int64_t cap) {
    sl_map_obj *m = (sl_map_obj *)malloc(sizeof(sl_map_obj));
    if (!m) { vm_error(vm, "out of memory"); return sl_void(); }
    m->head.rc = 1;
    m->head.kind = SL_MAP;
    m->len = 0;
    m->cap = cap > 0 ? cap : 0;
    m->index = NULL;
    m->index_cap = 0;
    m->pairs = m->cap ? (map_pair *)malloc(sizeof(map_pair) * (size_t)m->cap) : NULL;
    if (m->cap && !m->pairs) { free(m); vm_error(vm, "out of memory"); return sl_void(); }
    vm->live++;
    return obj_value(&m->head);
}

int64_t sl_map_len(sl_value v) {
    if (v.type != SL_OBJ || !v.as.o || v.as.o->kind != SL_MAP) return 0;
    return ((sl_map_obj *)v.as.o)->len;
}

bool sl_map_get(sl_value m, sl_value key, sl_value *out) {
    if (m.type != SL_OBJ || !m.as.o || m.as.o->kind != SL_MAP) return false;
    sl_map_obj *mo = (sl_map_obj *)m.as.o;
    int64_t i = map_find(mo, key, map_hash(key));
    if (i < 0) return false;
    *out = mo->pairs[i].val;
    return true;
}

bool sl_map_set(sl_vm *vm, sl_value m, sl_value key, sl_value val) {
    if (m.type != SL_OBJ || !m.as.o || m.as.o->kind != SL_MAP) {
        sl_release(vm, key);
        sl_release(vm, val);
        vm_error(vm, "not a map");
        return false;
    }
    sl_map_obj *mo = (sl_map_obj *)m.as.o;
    uint64_t h = map_hash(key);
    int64_t found = map_find(mo, key, h);
    if (found >= 0) {
        sl_release(vm, mo->pairs[found].val);
        mo->pairs[found].val = val;
        sl_release(vm, key);   // an equal key is already stored; this one is spare
        return true;
    }
    if (mo->len == mo->cap) {
        int64_t cap = mo->cap < 8 ? 8 : mo->cap * 2;
        map_pair *n = (map_pair *)realloc(mo->pairs, sizeof(map_pair) * (size_t)cap);
        if (!n) { sl_release(vm, key); sl_release(vm, val); vm_error(vm, "out of memory"); return false; }
        mo->pairs = n;
        mo->cap = cap;
    }
    mo->pairs[mo->len].key = key;
    mo->pairs[mo->len].val = val;
    mo->pairs[mo->len].hash = h;
    mo->len++;
    if (mo->index && (uint64_t)mo->len * 2 <= mo->index_cap) map_index_insert(mo, mo->len - 1);
    else if (mo->len > MAP_SCAN_MAX) map_reindex(mo);
    return true;
}

bool sl_map_remove(sl_vm *vm, sl_value m, sl_value key, sl_value *out) {
    if (m.type != SL_OBJ || !m.as.o || m.as.o->kind != SL_MAP) return false;
    sl_map_obj *mo = (sl_map_obj *)m.as.o;
    int64_t i = map_find(mo, key, map_hash(key));
    if (i < 0) return false;
    *out = mo->pairs[i].val;
    sl_release(vm, mo->pairs[i].key);
    for (int64_t k = i; k < mo->len - 1; k++) mo->pairs[k] = mo->pairs[k + 1];
    mo->len--;
    map_reindex(mo);
    return true;
}

bool sl_map_pair_at(sl_value m, int64_t i, sl_value *key_out, sl_value *val_out) {
    if (m.type != SL_OBJ || !m.as.o || m.as.o->kind != SL_MAP) return false;
    sl_map_obj *mo = (sl_map_obj *)m.as.o;
    if (i < 0 || i >= mo->len) return false;
    *key_out = mo->pairs[i].key;
    *val_out = mo->pairs[i].val;
    return true;
}

// Takes ownership of `item`.
static bool array_push(sl_vm *vm, sl_arr *a, sl_value item) {
    if (a->ek == AK_NONE || a->len == a->cap || a->ek != ak_of(item)) {
        if (!arr_room(vm, a, ak_of(item))) { sl_release(vm, item); return false; }
    }
    arr_put(a, a->len++, item);
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
    sl_struct *dst = struct_alloc(vm, n);
    if (!dst) { vm_error(vm, "out of memory"); return sl_void(); }
    dst->head.rc = 1;
    dst->head.kind = SL_STRUCT;
    dst->nfields = n;
    for (uint16_t i = 0; i < n; i++) {
        sl_value f = vload(&src->fields[i]);
        vstore(&dst->fields[i], f.type == SL_OBJ ? struct_copy(vm, f) : f);
    }
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
                render(vm, arr_get(a, i), buf, len, cap);
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
        case SL_MAP: {
            sl_map_obj *m = (sl_map_obj *)v.as.o;
            append(buf, len, cap, "{", 1);
            for (int64_t i = 0; i < m->len; i++) {
                if (i) append(buf, len, cap, ", ", 2);
                render(vm, m->pairs[i].key, buf, len, cap);
                append(buf, len, cap, ": ", 2);
                render(vm, m->pairs[i].val, buf, len, cap);
            }
            append(buf, len, cap, "}", 1);
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

static bool want_array_arg(sl_vm *vm, sl_value v, const char *who) {
    if (v.type != SL_OBJ || !v.as.o || v.as.o->kind != SL_ARRAY) {
        sl_fail(vm, who);
        return false;
    }
    return true;
}

static bool sl_value_eq(sl_value a, sl_value b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case SL_INT: return a.as.i == b.as.i;
        case SL_FLOAT: return a.as.f == b.as.f;
        case SL_BOOL: return a.as.b == b.as.b;
        case SL_OBJ: {
            if (!a.as.o || !b.as.o) return a.as.o == b.as.o;
            if (a.as.o->kind != SL_STR || b.as.o->kind != SL_STR) return false;
            size_t an, bn;
            const char *ap = sl_as_str(a, &an), *bp = sl_as_str(b, &bn);
            return an == bn && memcmp(ap, bp, an) == 0;
        }
        default: return false;
    }
}

// `==`/`!=` on a bare generic type param: the compiler doesn't know which
// typed opcode to pick at that call site, so it calls this instead, which
// decides from the values' own runtime tags. Unspellable name: not reachable
// except from codegen's own emission.
static sl_value intrin_eq_any(sl_vm *vm, int argc, sl_value *argv) {
    (void)vm;
    if (argc < 2) return sl_bool(false);
    return sl_bool(sl_value_eq(argv[0], argv[1]));
}

static sl_value intrin_pop(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 1 || !want_array_arg(vm, argv[0], "pop expects an array")) return sl_void();
    sl_value out;
    if (!sl_array_pop(vm, argv[0], &out)) return sl_void();
    return out;
}

static sl_value intrin_index_of(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 2 || !want_array_arg(vm, argv[0], "indexOf expects an array")) return sl_void();
    int64_t n = sl_array_len(argv[0]);
    for (int64_t i = 0; i < n; i++)
        if (sl_value_eq(sl_array_get(argv[0], i), argv[1])) return sl_int(i);
    return sl_int(-1);
}

static sl_value intrin_contains(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 2) { sl_fail(vm, "contains expects an array or a map"); return sl_void(); }
    if (argv[0].type == SL_OBJ && argv[0].as.o && argv[0].as.o->kind == SL_MAP) {
        sl_value unused;
        return sl_bool(sl_map_get(argv[0], argv[1], &unused));
    }
    if (!want_array_arg(vm, argv[0], "contains expects an array or a map")) return sl_void();
    int64_t n = sl_array_len(argv[0]);
    for (int64_t i = 0; i < n; i++)
        if (sl_value_eq(sl_array_get(argv[0], i), argv[1])) return sl_bool(true);
    return sl_bool(false);
}

static sl_value intrin_remove_key(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 2) { sl_fail(vm, "removeKey expects a map and a key"); return sl_void(); }
    sl_value out;
    if (!sl_map_remove(vm, argv[0], argv[1], &out)) { sl_fail(vm, "key not found"); return sl_void(); }
    return out;
}

static sl_value intrin_keys(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 1) { sl_fail(vm, "keys expects a map"); return sl_void(); }
    int64_t n = sl_map_len(argv[0]);
    sl_value out = sl_array(vm, n);
    if (vm->failed) return sl_void();
    for (int64_t i = 0; i < n; i++) {
        sl_value k, v;
        sl_map_pair_at(argv[0], i, &k, &v);
        sl_retain(k);
        if (!sl_array_push(vm, out, k)) return sl_void();
    }
    return out;
}

static sl_value intrin_values(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 1) { sl_fail(vm, "values expects a map"); return sl_void(); }
    int64_t n = sl_map_len(argv[0]);
    sl_value out = sl_array(vm, n);
    if (vm->failed) return sl_void();
    for (int64_t i = 0; i < n; i++) {
        sl_value k, v;
        sl_map_pair_at(argv[0], i, &k, &v);
        sl_retain(v);
        if (!sl_array_push(vm, out, v)) return sl_void();
    }
    return out;
}

static sl_value intrin_reverse(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 1 || !want_array_arg(vm, argv[0], "reverse expects an array")) return sl_void();
    int64_t n = sl_array_len(argv[0]);
    for (int64_t i = 0; i < n / 2; i++) {
        sl_value a = sl_array_get(argv[0], i);
        sl_value b = sl_array_get(argv[0], n - 1 - i);
        sl_retain(a);
        sl_retain(b);
        sl_array_set(vm, argv[0], i, b);
        sl_array_set(vm, argv[0], n - 1 - i, a);
    }
    return sl_void();
}

static sl_value intrin_remove_at(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 2 || argv[1].type != SL_INT) {
        sl_fail(vm, "removeAt expects an array and an index");
        return sl_void();
    }
    sl_value out;
    if (!sl_array_remove(vm, argv[0], argv[1].as.i, &out)) return sl_void();
    return out;
}

static sl_value intrin_insert_at(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 3 || argv[1].type != SL_INT) {
        sl_fail(vm, "insertAt expects an array, an index and a value");
        return sl_void();
    }
    sl_retain(argv[2]);
    sl_array_insert(vm, argv[0], argv[1].as.i, argv[2]);
    return sl_void();
}

// -1/0/1, or `*ok = false` for a pair sort has no defined order for.
static int value_cmp(sl_value a, sl_value b, bool *ok) {
    *ok = true;
    if (a.type != b.type) { *ok = false; return 0; }
    switch (a.type) {
        case SL_INT: return a.as.i < b.as.i ? -1 : a.as.i > b.as.i ? 1 : 0;
        case SL_FLOAT: return a.as.f < b.as.f ? -1 : a.as.f > b.as.f ? 1 : 0;
        case SL_OBJ: {
            if (!a.as.o || !b.as.o || a.as.o->kind != SL_STR || b.as.o->kind != SL_STR) {
                *ok = false;
                return 0;
            }
            size_t an, bn;
            const char *ap = sl_as_str(a, &an), *bp = sl_as_str(b, &bn);
            size_t n = an < bn ? an : bn;
            int c = n ? memcmp(ap, bp, n) : 0;
            if (c != 0) return c;
            return an < bn ? -1 : an > bn ? 1 : 0;
        }
        default: *ok = false; return 0;
    }
}

static sl_value intrin_sort(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 1 || !want_array_arg(vm, argv[0], "sort expects an array")) return sl_void();
    int64_t n = sl_array_len(argv[0]);
    for (int64_t i = 1; i < n; i++) {
        int64_t j = i;
        for (;;) {
            if (j == 0) break;
            bool ok;
            int c = value_cmp(sl_array_get(argv[0], j - 1), sl_array_get(argv[0], j), &ok);
            if (!ok) { sl_fail(vm, "sort only orders int/float/str elements"); return sl_void(); }
            if (c <= 0) break;
            sl_value a = sl_array_get(argv[0], j - 1);
            sl_value b = sl_array_get(argv[0], j);
            sl_retain(a);
            sl_retain(b);
            sl_array_set(vm, argv[0], j - 1, b);
            sl_array_set(vm, argv[0], j, a);
            j--;
        }
    }
    return sl_void();
}

sl_vm *sl_new(void) {
    sl_vm *vm = (sl_vm *)calloc(1, sizeof(sl_vm));
    if (!vm) return NULL;
    vm->sp = vm->stack;
    sl_register(vm, "sqrt", intrin_sqrt);
    sl_register(vm, "abs", intrin_abs);
    sl_register(vm, "pop", intrin_pop);
    sl_register(vm, "indexOf", intrin_index_of);
    sl_register(vm, "contains", intrin_contains);
    sl_register(vm, "reverse", intrin_reverse);
    sl_register(vm, "removeAt", intrin_remove_at);
    sl_register(vm, "insertAt", intrin_insert_at);
    sl_register(vm, "sort", intrin_sort);
    sl_register(vm, " eq", intrin_eq_any);
    sl_register(vm, "removeKey", intrin_remove_key);
    sl_register(vm, "keys", intrin_keys);
    sl_register(vm, "values", intrin_values);
    return vm;
}

void sl_free(sl_vm *vm) {
    if (!vm) return;
    // Anything still on the stack is ours to drop.
    while (vm->sp > vm->stack) sl_release(vm, *--vm->sp);

    for (uint16_t i = 0; i < vm->nconsts; i++)
        if (vm->consts[i].type == C_STR) free(vm->consts[i].as.s.p);
    free(vm->consts);
    for (uint16_t i = 0; i < vm->nfuncs; i++) { free(vm->funcs[i].name); free(vm->funcs[i].code); free(vm->funcs[i].rcode); }
    free(vm->funcs);
    for (uint16_t i = 0; i < vm->nglobals; i++) sl_release(vm, vm->globals[i]);
    free(vm->globals);
    for (uint16_t i = 0; i < vm->nnative_names; i++) {
        free(vm->native_names[i]);
        if (vm->native_sigs) free(vm->native_sigs[i]);
    }
    free(vm->native_names);
    free(vm->native_sigs);
    free(vm->native_bind);
    if (vm->kstr) {
        for (uint16_t i = 0; i < vm->nconsts; i++) if (vm->kstr[i].type == SL_OBJ) sl_release(vm, vm->kstr[i]);
        free(vm->kstr);
    }
    free(vm->struct_desc);
    for (int n = 0; n <= SL_POOL_FIELDS; n++)
        while (vm->free_struct[n]) { void *next = *(void **)vm->free_struct[n]; free(vm->free_struct[n]); vm->free_struct[n] = next; }
    for (int i = 0; i < vm->nregistered; i++) { free(vm->registry[i].name); free(vm->registry[i].sig); }
    free(vm);
}

static char *dup_z(const char *z) {
    size_t n = strlen(z);
    char *copy = (char *)malloc(n + 1);
    if (copy) memcpy(copy, z, n + 1);
    return copy;
}

static bool register_native(sl_vm *vm, const char *name, const char *sig, sl_native_fn fn) {
    if (vm->nregistered >= SL_NATIVES_MAX) return false;
    char *copy = dup_z(name);
    char *sig_copy = sig ? dup_z(sig) : NULL;
    if (!copy || (sig && !sig_copy)) { free(copy); free(sig_copy); return false; }
    vm->registry[vm->nregistered].name = copy;
    vm->registry[vm->nregistered].sig = sig_copy;
    vm->registry[vm->nregistered].fn = fn;
    vm->nregistered++;
    return true;
}

bool sl_register(sl_vm *vm, const char *name, sl_native_fn fn) {
    return register_native(vm, name, NULL, fn);
}

bool sl_register_sig(sl_vm *vm, const char *name, const char *sig, sl_native_fn fn) {
    return register_native(vm, name, sig, fn);
}

typedef struct { char *p; size_t n, cap; } sbuf;

static bool sb_put(sbuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char *q = (char *)realloc(b->p, cap);
        if (!q) return false;
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
    return true;
}

static bool sb_puts(sbuf *b, const char *s) { return sb_put(b, s, strlen(s)); }

static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

// user structs and enums belong to the script that defines them; a host module cannot spell them
static bool plain_type(const char *t, size_t n) {
    for (size_t i = 0; i < n;) {
        if (is_ident_start(t[i])) {
            size_t j = i;
            while (j < n && (is_ident_char(t[j]) || t[j] == '.')) j++;
            size_t len = j - i;
            bool ok = (len == 3 && memcmp(t + i, "int", 3) == 0) || (len == 5 && memcmp(t + i, "float", 5) == 0) ||
                      (len == 4 && memcmp(t + i, "bool", 4) == 0) || (len == 3 && memcmp(t + i, "str", 3) == 0);
            if (!ok) return false;
            i = j;
        } else if (t[i] == '[' || t[i] == ']' || t[i] == '{' || t[i] == '}' || t[i] == ':' || t[i] == ' ') {
            i++;
        } else {
            return false;
        }
    }
    return true;
}

// "(str,int)->str" -> "extern func name(p0: str, p1: int): str;"; false if malformed or not plain
static bool decl_from_sig(sbuf *out, const char *name, const char *sig) {
    const char *p = sig;
    while (*p == ' ') p++;
    if (*p != '(') return false;
    const char *open = p++;
    int depth = 1;
    const char *close = NULL;
    for (const char *q = p; *q; q++) {
        if (*q == '(' || *q == '[' || *q == '{') depth++;
        else if (*q == ')' || *q == ']' || *q == '}') {
            if (--depth == 0) { close = q; break; }
        }
    }
    if (!close) return false;
    const char *arrow = close + 1;
    while (*arrow == ' ') arrow++;
    if (arrow[0] != '-' || arrow[1] != '>') return false;
    const char *ret = arrow + 2;
    while (*ret == ' ') ret++;
    size_t ret_len = strlen(ret);
    while (ret_len && ret[ret_len - 1] == ' ') ret_len--;
    if (!ret_len) return false;
    bool no_result = ret_len == 2 && memcmp(ret, "()", 2) == 0;
    if (!no_result && !plain_type(ret, ret_len)) return false;
    (void)open;

    sbuf tmp = { 0 };
    bool ok = sb_puts(&tmp, "extern func ") && sb_puts(&tmp, name) && sb_puts(&tmp, "(");
    int index = 0;
    const char *start = p;
    depth = 0;
    for (const char *q = p;; q++) {
        bool end = q == close;
        if (!end && (*q == '[' || *q == '{' || *q == '(')) depth++;
        else if (!end && (*q == ']' || *q == '}' || *q == ')')) depth--;
        if (end || (*q == ',' && depth == 0)) {
            size_t len = (size_t)(q - start);
            while (len && *start == ' ') { start++; len--; }
            while (len && start[len - 1] == ' ') len--;
            if (len) {
                char label[24];
                snprintf(label, sizeof label, "%sp%d: ", index ? ", " : "", index);
                if (!plain_type(start, len) || !sb_puts(&tmp, label) || !sb_put(&tmp, start, len)) { ok = false; break; }
                index++;
            }
            start = q + 1;
        }
        if (end) break;
    }
    ok = ok && sb_puts(&tmp, ")");
    if (ok && !no_result) ok = sb_puts(&tmp, ": ") && sb_put(&tmp, ret, ret_len);
    ok = ok && sb_puts(&tmp, ";\n") && sb_put(out, tmp.p, tmp.n);
    free(tmp.p);
    return ok;
}

char *sl_declarations(const sl_vm *vm) {
    sbuf b = { 0 };
    bool ok = sb_puts(&b, "// Generated from the natives this host registered with a signature.\n");
    for (int i = 0; ok && i < vm->nregistered; i++) {
        const char *name = vm->registry[i].name;
        const char *sig = vm->registry[i].sig;
        if (!sig || !is_ident_start(name[0])) continue;
        bool plain = true;
        for (const char *c = name; *c; c++) if (!is_ident_char(*c)) plain = false;
        if (!plain) continue;
        bool shadowed = false;      // the last registration of a name wins
        for (int j = i + 1; j < vm->nregistered; j++)
            if (strcmp(vm->registry[j].name, name) == 0) shadowed = true;
        if (shadowed) continue;
        if (!decl_from_sig(&b, name, sig)) {
            ok = sb_puts(&b, "// `") && sb_puts(&b, name) &&
                 sb_puts(&b, "` is not declared here: its signature names a type only a script can define.\n");
        }
    }
    if (!ok) { free(b.p); return NULL; }
    return b.p;
}

// ignores spaces: {str:int} equals {str: int}
static bool sig_equal(const char *a, const char *b) {
    for (;;) {
        while (*a == ' ') a++;
        while (*b == ' ') b++;
        if (*a != *b) return false;
        if (!*a) return true;
        a++; b++;
    }
}

// reports every unresolved or mismatched native, not just the first
// ---------------------------------------------------------------------------
// Register code
//
// The image holds stack code (compiler/emit.h). sl_load translates every
// function once into the form below and sl_run executes that. A function's
// frame is a run of registers: its locals first, then temporaries that stand
// in for the stack positions of the original code. Every operand is a 16-bit
// register number or immediate.
//
// Ownership: a register holding a heap value owns one reference. Dead
// registers hold void or a scalar, never a stale heap pointer, so releasing a
// whole frame (return, fault) is a plain scan. An instruction that consumes a
// heap value out of a register sets it to void.
// ---------------------------------------------------------------------------

enum {
    R_LDI, R_LDC_I, R_LDC_F, R_LDS, R_LDB, R_LDNULL,
    R_MOV, R_MOVS, R_STORE, R_STORE_L, R_DROP,
    R_ADD_I, R_SUB_I, R_MUL_I, R_DIV_I, R_REM_I, R_ADDI, R_NEG_I,
    R_ADD_F, R_SUB_F, R_MUL_F, R_DIV_F, R_REM_F, R_NEG_F,
    R_EQ_I, R_NE_I, R_LT_I, R_LE_I, R_GT_I, R_GE_I,
    R_EQ_F, R_NE_F, R_LT_F, R_LE_F, R_GT_F, R_GE_F,
    R_EQ_B, R_NE_B, R_NOT, R_EQ_S, R_NE_S,
    R_CONCAT, R_STR_I, R_STR_F, R_STR_B, R_STR_OBJ,
    R_JMP, R_JT, R_JF,
    // Compare-and-branch: the jump is taken when the comparison holds (JC),
    // or when it does not (JN, floats only: not the same thing, because NaN).
    R_JC_EQ_I, R_JC_NE_I, R_JC_LT_I, R_JC_LE_I, R_JC_GT_I, R_JC_GE_I,
    R_JC_EQ_F, R_JC_NE_F, R_JC_LT_F, R_JC_LE_F, R_JC_GT_F, R_JC_GE_F,
    R_JN_EQ_F, R_JN_NE_F, R_JN_LT_F, R_JN_LE_F, R_JN_GT_F, R_JN_GE_F,
    R_JCI_EQ, R_JCI_NE, R_JCI_LT, R_JCI_LE, R_JCI_GT, R_JCI_GE,
    R_CALL, R_NATIVE, R_RET, R_RETV, R_HALT,
    // The _L forms read a local register without consuming it; the others
    // consume a temporary.
    R_NEWARR, R_AGET, R_AGET_L, R_ASET, R_ASET_L, R_ALEN, R_ALEN_L,
    R_APUSH, R_APUSH_L, R_SLICE,
    R_NEWSTRUCT, R_FGET, R_FGET_L, R_FSET, R_FSET_L,
    R_NEWVAR, R_VTAG, R_VGET,
    R_LOADG, R_STOREG, R_COPY, R_NEWMAP, R_MGET, R_MSET,
    // x += step, then jump if x < limit (a register, or an immediate for the I form)
    R_AJLT, R_AJLTI,
    // jump if x < len(a)
    R_JLTLEN,
    // interpreter-only fusions (the C emitter works from the plain forms)
    R_MOV2, R_MOV3, R_AGET_S, R_ASET_LC, R_APUSH_LC,
    // the same array accesses with the bounds check proved away by the optimizer
    R_AGET_LU, R_ASET_LU, R_AGET_SU, R_ASET_LCU,
    R_COUNT
};

typedef enum { E_TEMP, E_LOCAL, E_IMM, E_CMP } ekind;

// What a stack position holds while translating. Locals, immediates and
// comparisons stay lazy until something needs them in a register.
typedef struct {
    uint8_t  kind;
    bool     scalar;      // E_TEMP: known to hold an int, float or bool
    bool     is_float;    // E_CMP
    bool     b_imm;       // E_CMP: right-hand side is `b`, an immediate
    uint8_t  cmp;         // E_CMP: 0..5 = EQ NE LT LE GT GE
    uint16_t reg;         // E_LOCAL: slot. E_CMP: left operand
    int32_t  b;           // E_IMM: value. E_CMP: right operand
} tent;

typedef struct { size_t at; int target; } tfix;

#define TX_MAX_HOIST 24

typedef struct {
    sl_vm   *vm;
    const uint8_t *in;
    size_t   n;
    uint16_t *out;
    size_t   nout, cap;
    tent    *st;
    int      depth, maxdepth;
    uint16_t nslots;
    int     *newpos, *depth_at;
    uint8_t *is_target;
    tfix    *fix;
    int      nfix;
    bool    *obj_slot;    // slots that may hold a heap value
    // Constants that sit inside a loop are loaded once, on entry, into
    // registers after the locals, and read from there afterwards.
    uint64_t *kbits;      // shared by both passes
    uint8_t  *kfloat;
    int      *nk;
    uint16_t tbase;       // first temporary: nslots plus one register per hoisted constant
    uint8_t  *in_loop;
    bool     direct;      // second pass: obj_slot is final, write scalars in place
    bool     reachable;
    bool     last_def_valid;
    size_t   last_def_pos, last_def_end;
    size_t   ins_start;          // where the current stack instruction began emitting
    size_t   last_aget_at;       // start of the last AGET_L, to fold a following STORE into it
    int      last_def_depth;
    bool     bad;
} tx;

#define TREG(t, d) ((uint16_t)((t)->tbase + (d)))

static int stack_op_len(uint8_t op) {
    switch (op) {
        case OP_TRUE: case OP_FALSE: case OP_POP: case OP_DUP: case OP_CONCAT:
        case OP_ADD_I: case OP_SUB_I: case OP_MUL_I: case OP_DIV_I: case OP_REM_I: case OP_NEG_I:
        case OP_STR_I: case OP_STR_F: case OP_STR_B: case OP_STR_OBJ:
        case OP_ADD_F: case OP_SUB_F: case OP_MUL_F: case OP_DIV_F: case OP_REM_F: case OP_NEG_F:
        case OP_EQ_I: case OP_NE_I: case OP_LT_I: case OP_LE_I: case OP_GT_I: case OP_GE_I:
        case OP_EQ_F: case OP_NE_F: case OP_LT_F: case OP_LE_F: case OP_GT_F: case OP_GE_F:
        case OP_EQ_S: case OP_NE_S: case OP_EQ_B: case OP_NE_B: case OP_NOT:
        case OP_RET: case OP_RET_VOID: case OP_HALT:
        case OP_AGET: case OP_ASET: case OP_ALEN: case OP_APUSH: case OP_VTAG:
        case OP_DUP2: case OP_NULL: case OP_SLICE: case OP_MGET: case OP_MSET:
            return 1;
        case OP_CONST_I8: case OP_LOAD: case OP_STORE: case OP_FGET: case OP_FSET: case OP_VGET:
            return 2;
        case OP_CONST: case OP_CONST_STR: case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE:
        case OP_NEW_ARRAY: case OP_NEW_MAP: case OP_LOADG: case OP_STOREG:
        case OP_INCR_I: case OP_MOVE: case OP_COPY:
            return 3;
        case OP_NEW_VARIANT: case OP_ADD_RR_I:
            return 4;
        case OP_CALL: case OP_NATIVE: case OP_NEW_STRUCT:
            return 5;
        default:
            return 0;
    }
}

static void tx_word(tx *t, uint16_t w) {
    if (t->nout == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 256;
        uint16_t *o = (uint16_t *)realloc(t->out, cap * sizeof *o);
        if (!o) { t->bad = true; return; }
        t->out = o; t->cap = cap;
    }
    t->out[t->nout++] = w;
}

// Append one instruction (operands end at the -1 sentinel); returns the index
// of its first word. Anything that is not a scalar definition invalidates the
// "last definition" record.
static size_t tx_emitv(tx *t, ...) {
    size_t at = t->nout;
    va_list ap;
    va_start(ap, t);
    for (int w = va_arg(ap, int); w != -1; w = va_arg(ap, int)) tx_word(t, (uint16_t)w);
    va_end(ap);
    t->last_def_valid = false;
    return at;
}
#define EMIT(t, ...) tx_emitv((t), __VA_ARGS__, -1)

// Record that the instruction at `at` just defined a scalar into the register
// at word `dst_word`, standing for stack position `depth`.
static void tx_def(tx *t, size_t dst_word, int depth) {
    t->last_def_valid = true;
    t->last_def_pos = dst_word;
    t->last_def_depth = depth;
    t->last_def_end = t->nout;
}

// Registers T(0) .. T(count-1) are in use.
static void tx_need(tx *t, int count) { if (count > t->maxdepth) t->maxdepth = count; }

static void tx_materialize(tx *t, int k) {
    tent *e = &t->st[k];
    uint16_t dst = TREG(t, k);
    switch (e->kind) {
        case E_TEMP: return;
        case E_LOCAL:
            tx_need(t, k + 1);
#ifndef SL_AOT_EMITTER
            // Argument setup is a run of copies: one instruction for two or three of them.
            if (t->nout >= t->ins_start + 3 && t->nout >= 3 && t->out[t->nout - 3] == R_MOV) {
                t->out[t->nout - 3] = R_MOV2;
                tx_word(t, dst); tx_word(t, e->reg);
                t->last_def_valid = false;
                e->kind = E_TEMP; e->scalar = false;
                return;
            }
            if (t->nout >= t->ins_start + 5 && t->nout >= 5 && t->out[t->nout - 5] == R_MOV2) {
                t->out[t->nout - 5] = R_MOV3;
                tx_word(t, dst); tx_word(t, e->reg);
                t->last_def_valid = false;
                e->kind = E_TEMP; e->scalar = false;
                return;
            }
#endif
            EMIT(t, R_MOV, dst, e->reg);
            e->kind = E_TEMP; e->scalar = false;
            return;
        case E_IMM: {
            tx_need(t, k + 1);
            size_t at = EMIT(t, R_LDI, dst, (uint16_t)(int16_t)e->b);
            tx_def(t, at + 1, k);
            e->kind = E_TEMP; e->scalar = true;
            return;
        }
        case E_CMP: {
            uint16_t rb;
            if (e->b_imm) {
                tx_need(t, k + 2);
                rb = TREG(t, k + 1);
                EMIT(t, R_LDI, rb, (uint16_t)(int16_t)e->b);
            } else {
                rb = (uint16_t)e->b;
            }
            tx_need(t, k + 1);
            size_t at = EMIT(t, (e->is_float ? R_EQ_F : R_EQ_I) + e->cmp, dst, e->reg, rb);
            tx_def(t, at + 1, k);
            e->kind = E_TEMP; e->scalar = true;
            return;
        }
    }
}

static void tx_flush(tx *t, int upto) { for (int k = 0; k < upto; k++) tx_materialize(t, k); }

// The register to read stack position k from as a scalar.
static uint16_t tx_sreg(tx *t, int k) {
    tent *e = &t->st[k];
    if (e->kind == E_LOCAL) return e->reg;
    tx_materialize(t, k);
    return TREG(t, k);
}

static void tx_push(tx *t, tent e) {
    if (t->depth + 1 >= (int)(t->n + 8)) { t->bad = true; return; }
    t->st[t->depth++] = e;
    tx_need(t, t->depth);
}
static uint16_t tx_push_temp(tx *t, bool scalar) {
    tent e; memset(&e, 0, sizeof e);
    e.kind = E_TEMP; e.scalar = scalar;
    uint16_t r = TREG(t, t->depth);
    tx_push(t, e);
    return r;
}

// Make `s` safe to overwrite: any lazy reference to it is copied out first.
// Returns true if that emitted code.
static bool tx_invalidate(tx *t, uint16_t s) {
    bool emitted = false;
    for (int k = 0; k < t->depth; k++)
        if (t->st[k].kind == E_LOCAL && t->st[k].reg == s) { tx_materialize(t, k); emitted = true; }
    return emitted;
}

static void tx_jump_fix(tx *t, size_t at, int target_off) {
    if (!t->fix) { t->bad = true; return; }
    t->fix[t->nfix].at = at;
    t->fix[t->nfix].target = target_off;
    t->nfix++;
}

static const uint8_t cmp_inverse[6] = { 1, 0, 5, 4, 3, 2 };

// Length in words of a register instruction that is safe to copy: no jump, no
// side effect beyond writing its destination register. 0 for anything else.
static int tx_copyable_len(uint16_t op) {
    switch (op) {
        case R_LDI: case R_MOVS: case R_ALEN_L: return 3;
        case R_ADDI: case R_AGET_L: return 4;
        case R_LDC_I: case R_LDC_F: return 6;
        default: return 0;
    }
}

// If the code at `target` (a loop head) is a few simple instructions and then
// one conditional branch that leaves the loop to `exit_off`, copy them to the
// bottom with the branch inverted and aimed at the loop body. Returns false and
// emits nothing if the shape is not that.
static bool tx_invert_loop(tx *t, int target, int exit_off) {
    size_t head = (size_t)t->newpos[target];
    size_t pos = head;
    while (pos < t->nout && tx_copyable_len(t->out[pos]) && pos - head < 12) pos += (size_t)tx_copyable_len(t->out[pos]);
    if (pos >= t->nout) return false;
    uint16_t op = t->out[pos];
    int len;
    if (op == R_JT || op == R_JF) len = 3;
    else if ((op >= R_JC_EQ_I && op <= R_JN_GE_F) || (op >= R_JCI_EQ && op <= R_JCI_GE)) len = 4;
    else return false;
    if (pos + (size_t)len > t->nout) return false;
    // no other entry into the middle of that block, and its jump must be the loop exit
    for (size_t k = head + 1; k <= pos; k++) {
        bool inner = false;
        for (int tg = 0; tg <= (int)t->n; tg++) if (t->newpos[tg] == (int)k && t->is_target[tg]) inner = true;
        if (inner) return false;
    }
    bool exits = false;
    for (int i = 0; i < t->nfix; i++)
        if (t->fix[i].at == pos + (size_t)len - 1 && t->fix[i].target == exit_off) exits = true;
    if (!exits) return false;
    if (t->is_target[exit_off - 3]) return false;       // something else lands on this jump

    uint16_t inv;
    if (op == R_JT) inv = R_JF;
    else if (op == R_JF) inv = R_JT;
    else if (op >= R_JC_EQ_I && op <= R_JC_GE_I) inv = (uint16_t)(R_JC_EQ_I + cmp_inverse[op - R_JC_EQ_I]);
    else if (op >= R_JC_EQ_F && op <= R_JC_GE_F) inv = (uint16_t)(R_JN_EQ_F + (op - R_JC_EQ_F));
    else if (op >= R_JN_EQ_F && op <= R_JN_GE_F) inv = (uint16_t)(R_JC_EQ_F + (op - R_JN_EQ_F));
    else inv = (uint16_t)(R_JCI_EQ + cmp_inverse[op - R_JCI_EQ]);

    size_t body = pos + (size_t)len;
    // `x += k` right before a `<` test of x fuses into one instruction
    if (pos == head && (inv == R_JC_LT_I || inv == R_JCI_LT) && t->nout >= 4 && t->out[t->nout - 4] == R_ADDI &&
        t->out[t->nout - 3] == t->out[t->nout - 2] && t->out[t->nout - 3] == t->out[pos + 1]) {
        uint16_t x = t->out[pos + 1], step = t->out[t->nout - 1], lim = t->out[pos + 2];
        t->nout -= 4;
        size_t at = t->nout;
        tx_word(t, inv == R_JC_LT_I ? R_AJLT : R_AJLTI);
        tx_word(t, x); tx_word(t, lim); tx_word(t, step);
        tx_word(t, (uint16_t)(int16_t)((long)body - (long)(at + 5)));
        t->last_def_valid = false;
        return true;
    }
    // `i < len(a)`: the length read and the test are one instruction
    if (pos == head + 3 && t->out[head] == R_ALEN_L && inv == R_JC_LT_I && t->out[pos + 2] == t->out[head + 1] &&
        t->out[pos + 1] != t->out[head + 1]) {
        size_t at = t->nout;
        tx_word(t, R_JLTLEN);
        tx_word(t, t->out[pos + 1]);
        tx_word(t, t->out[head + 2]);
        tx_word(t, (uint16_t)(int16_t)((long)body - (long)(at + 4)));
        t->last_def_valid = false;
        return true;
    }
    for (size_t k = head; k < pos; k++) tx_word(t, t->out[k]);      // the simple instructions
    size_t at = t->nout;
    tx_word(t, inv);
    for (int k = 1; k < len - 1; k++) tx_word(t, t->out[pos + (size_t)k]);
    tx_word(t, (uint16_t)(int16_t)((long)body - (long)(at + (size_t)len)));
    t->last_def_valid = false;
    return true;
}

// Translate one function. Run twice: the first pass only discovers which
// slots can hold heap values, so the second can write scalars straight into
// the rest.
static bool translate_pass(sl_vm *vm, sl_func *f, bool *obj_slot, bool direct, tx *t,
                           uint64_t *kbits, uint8_t *kfloat, int *nk) {
    memset(t, 0, sizeof *t);
    t->kbits = kbits; t->kfloat = kfloat; t->nk = nk;
    t->tbase = (uint16_t)(f->nslots + (direct ? *nk : 0));
    t->vm = vm; t->in = f->code; t->n = f->code_len;
    t->nslots = f->nslots; t->obj_slot = obj_slot; t->direct = direct;
    size_t n = f->code_len;
    t->st = (tent *)calloc(n + 16, sizeof(tent));
    t->newpos = (int *)malloc((n + 1) * sizeof(int));
    t->depth_at = (int *)malloc((n + 1) * sizeof(int));
    t->is_target = (uint8_t *)calloc(n + 1, 1);
    t->in_loop = (uint8_t *)calloc(n + 1, 1);
    t->fix = (tfix *)malloc((n + 1) * sizeof(tfix));
    if (!t->st || !t->newpos || !t->depth_at || !t->is_target || !t->in_loop || !t->fix) { t->bad = true; return false; }
    for (size_t i = 0; i <= n; i++) { t->newpos[i] = -1; t->depth_at[i] = -1; }

    // Jump targets.
    for (size_t at = 0; at < n;) {
        int len = stack_op_len(f->code[at]);
        if (!len || at + (size_t)len > n) { t->bad = true; return false; }
        uint8_t op = f->code[at];
        if (op == OP_JUMP || op == OP_JUMP_IF_FALSE || op == OP_JUMP_IF_TRUE) {
            int16_t d = (int16_t)((uint16_t)f->code[at + 1] | ((uint16_t)f->code[at + 2] << 8));
            long tgt = (long)at + 3 + d;
            if (tgt < 0 || tgt > (long)n) { t->bad = true; return false; }
            t->is_target[tgt] = 1;
            if (tgt <= (long)at)      // a backward jump closes a loop
                for (long k = tgt; k <= (long)at; k++) t->in_loop[k] = 1;
        }
        at += (size_t)len;
    }

    t->reachable = true;
    if (direct) {
        for (int k = 0; k < *nk; k++) {
            uint64_t u = kbits[k];
            EMIT(t, kfloat[k] ? R_LDC_F : R_LDC_I, (uint16_t)(t->nslots + k),
                 (uint16_t)u, (uint16_t)(u >> 16), (uint16_t)(u >> 32), (uint16_t)(u >> 48));
        }
    }
    for (size_t off = 0; off < n && !t->bad;) {
        const uint8_t *p = f->code + off;
        uint8_t op = p[0];
        int len = stack_op_len(op);

        if (t->is_target[off]) {
            if (t->reachable) {
                tx_flush(t, t->depth);
            } else {
                t->depth = t->depth_at[off] >= 0 ? t->depth_at[off] : 0;
                for (int k = 0; k < t->depth; k++) {
                    memset(&t->st[k], 0, sizeof(tent));
                    t->st[k].kind = E_TEMP;
                }
                tx_need(t, t->depth);
            }
            t->newpos[off] = (int)t->nout;
            t->reachable = true;
            t->last_def_valid = false;
        } else if (!t->reachable) {
            off += (size_t)len;
            continue;
        }

        t->ins_start = t->nout;

        // A comparison waits for a branch to fuse with; anything else needs it as a value.
        if (t->depth > 0 && t->st[t->depth - 1].kind == E_CMP &&
            op != OP_JUMP_IF_FALSE && op != OP_JUMP_IF_TRUE)
            tx_materialize(t, t->depth - 1);

#define NEED(k) do { if (t->depth < (k)) { t->bad = true; goto out; } } while (0)
#define TOP (t->depth - 1)
        switch (op) {
        case OP_CONST_I8: {
            tent e; memset(&e, 0, sizeof e);
            e.kind = E_IMM; e.b = (int8_t)p[1];
            tx_push(t, e);
            break;
        }
        case OP_CONST: {
            uint16_t i = (uint16_t)(p[1] | (p[2] << 8));
            if (i >= vm->nconsts || vm->consts[i].type == C_STR) { t->bad = true; goto out; }
            bool is_int = vm->consts[i].type == C_INT;
            uint64_t u;
            if (is_int) u = (uint64_t)vm->consts[i].as.i;
            else memcpy(&u, &vm->consts[i].as.f, sizeof u);
            if (is_int) {
                int64_t v = vm->consts[i].as.i;
                if (v >= -32768 && v <= 32767) {
                    tent e; memset(&e, 0, sizeof e);
                    e.kind = E_IMM; e.b = (int32_t)v;
                    tx_push(t, e);
                    break;
                }
            }
            if (t->in_loop[off]) {
                int idx = -1;
                for (int k = 0; k < *t->nk; k++)
                    if (t->kbits[k] == u && t->kfloat[k] == !is_int) { idx = k; break; }
                if (idx < 0 && *t->nk < TX_MAX_HOIST) {
                    idx = (*t->nk)++;
                    t->kbits[idx] = u; t->kfloat[idx] = !is_int;
                }
                if (idx >= 0) {
                    tent e; memset(&e, 0, sizeof e);
                    e.kind = E_LOCAL; e.reg = (uint16_t)(t->nslots + idx);
                    tx_push(t, e);
                    break;
                }
            }
            uint16_t r = tx_push_temp(t, true);
            size_t at = EMIT(t, is_int ? R_LDC_I : R_LDC_F, r, (uint16_t)u, (uint16_t)(u >> 16), (uint16_t)(u >> 32), (uint16_t)(u >> 48));
            tx_def(t, at + 1, t->depth - 1);
            break;
        }
        case OP_CONST_STR: {
            uint16_t i = (uint16_t)(p[1] | (p[2] << 8));
            if (i >= vm->nconsts || vm->consts[i].type != C_STR) { t->bad = true; goto out; }
            uint16_t r = tx_push_temp(t, false);
            EMIT(t, R_LDS, r, i);
            break;
        }
        case OP_TRUE: case OP_FALSE: {
            uint16_t r = tx_push_temp(t, true);
            size_t at = EMIT(t, R_LDB, r, op == OP_TRUE ? 1 : 0);
            tx_def(t, at + 1, t->depth - 1);
            break;
        }
        case OP_NULL: {
            uint16_t r = tx_push_temp(t, false);
            EMIT(t, R_LDNULL, r);
            break;
        }
        case OP_POP: {
            NEED(1);
            tent e = t->st[TOP];
            t->depth--;
            if (e.kind == E_TEMP && !e.scalar) EMIT(t, R_DROP, TREG(t, t->depth));
            break;
        }
        case OP_LOAD: {
            tent e; memset(&e, 0, sizeof e);
            e.kind = E_LOCAL; e.reg = p[1];
            tx_push(t, e);
            break;
        }
        case OP_DUP: {
            NEED(1);
            tent e = t->st[TOP];
            if (e.kind == E_TEMP) {
                uint16_t r = tx_push_temp(t, e.scalar);
                EMIT(t, R_MOV, r, TREG(t, t->depth - 2));
            } else {
                tx_push(t, e);
            }
            break;
        }
        case OP_DUP2: {
            NEED(2);
            tent a = t->st[t->depth - 2], b = t->st[t->depth - 1];
            for (int i = 0; i < 2; i++) {
                tent e = i ? b : a;
                if (e.kind == E_TEMP) {
                    uint16_t src = TREG(t, t->depth - 2);
                    uint16_t r = tx_push_temp(t, e.scalar);
                    EMIT(t, R_MOV, r, src);
                } else {
                    tx_push(t, e);
                }
            }
            break;
        }
        case OP_STORE: {
            NEED(1);
            uint16_t s = p[1];
            if (s >= t->nslots) { t->bad = true; goto out; }
            tent e = t->st[TOP];
            t->depth--;
            tx_invalidate(t, s);
            bool hoisted = e.kind == E_LOCAL && e.reg >= t->nslots;      // a loop-invariant constant
            bool scalar_known = e.kind == E_IMM || e.kind == E_CMP || hoisted || (e.kind == E_TEMP && e.scalar);
            if (!scalar_known) t->obj_slot[s] = true;
            uint16_t tr = TREG(t, t->depth);
            if (t->direct && !t->obj_slot[s]) {
                // The slot only ever holds scalars: write it without releasing.
                if (e.kind == E_IMM) {
                    EMIT(t, R_LDI, s, (uint16_t)(int16_t)e.b);
                } else if (hoisted) {
                    EMIT(t, R_MOVS, s, e.reg);
                } else {
                    if (e.kind == E_CMP) {
                        t->st[t->depth] = e; t->depth++;
                        tx_materialize(t, t->depth - 1);
                        t->depth--;
                    }
                    if (t->last_def_valid && t->last_def_depth == t->depth && t->last_def_end == t->nout) {
                        t->out[t->last_def_pos] = s;      // retarget the instruction that just defined it
                        t->last_def_valid = false;
                    } else {
                        EMIT(t, R_MOVS, s, tr);
                    }
                }
            } else if (e.kind == E_LOCAL) {
                EMIT(t, R_STORE_L, s, e.reg);
            } else {
                t->st[t->depth] = e; t->depth++;
                tx_materialize(t, t->depth - 1);
                t->depth--;
#ifndef SL_AOT_EMITTER
                // `let x = a[i]`: read the element straight into the variable
                if (t->nout >= 4 && t->last_aget_at == t->nout - 4 && t->out[t->nout - 4] == R_AGET_L &&
                    t->out[t->nout - 3] == tr) {
                    uint16_t arr = t->out[t->nout - 2], idx = t->out[t->nout - 1];
                    t->nout -= 4;
                    EMIT(t, R_AGET_S, s, arr, idx);
                    break;
                }
#endif
                EMIT(t, R_STORE, s, tr);
            }
            break;
        }
        case OP_INCR_I: {
            uint16_t s = p[1];
            if (s >= t->nslots) { t->bad = true; goto out; }
            tx_invalidate(t, s);
            EMIT(t, R_ADDI, s, s, (uint16_t)(int16_t)(int8_t)p[2]);
            break;
        }
        case OP_MOVE: {
            uint16_t dst = p[1], src = p[2];
            if (dst >= t->nslots || src >= t->nslots) { t->bad = true; goto out; }
            tx_invalidate(t, dst);
            EMIT(t, R_MOVS, dst, src);
            break;
        }
        case OP_ADD_RR_I: {
            uint16_t dst = p[1], s1 = p[2], s2 = p[3];
            if (dst >= t->nslots || s1 >= t->nslots || s2 >= t->nslots) { t->bad = true; goto out; }
            tx_invalidate(t, dst);
            EMIT(t, R_ADD_I, dst, s1, s2);
            break;
        }

        case OP_ADD_I: case OP_SUB_I: case OP_MUL_I: case OP_DIV_I: case OP_REM_I: {
            NEED(2);
            int a = t->depth - 2, b = t->depth - 1;
            bool is_add = op == OP_ADD_I, is_sub = op == OP_SUB_I;
            tent A = t->st[a], B = t->st[b];
            uint16_t dst = TREG(t, a);
            size_t at;
            if ((is_add || is_sub) && B.kind == E_IMM && (is_sub ? -B.b : B.b) >= -32768 && (is_sub ? -B.b : B.b) <= 32767) {
                uint16_t ra = tx_sreg(t, a);
                at = EMIT(t, R_ADDI, dst, ra, (uint16_t)(int16_t)(is_sub ? -B.b : B.b));
            } else if (is_add && A.kind == E_IMM) {
                uint16_t rb = tx_sreg(t, b);
                at = EMIT(t, R_ADDI, dst, rb, (uint16_t)(int16_t)A.b);
            } else {
                uint16_t ra = tx_sreg(t, a), rb = tx_sreg(t, b);
                at = EMIT(t, R_ADD_I + (op - OP_ADD_I), dst, ra, rb);
            }
            t->depth--;
            t->st[a].kind = E_TEMP; t->st[a].scalar = true;
            tx_def(t, at + 1, a);
            break;
        }
        case OP_ADD_F: case OP_SUB_F: case OP_MUL_F: case OP_DIV_F: case OP_REM_F:
        case OP_EQ_B: case OP_NE_B: {
            NEED(2);
            int a = t->depth - 2, b = t->depth - 1;
            uint16_t ra = tx_sreg(t, a), rb = tx_sreg(t, b);
            uint16_t rop = op >= OP_ADD_F && op <= OP_REM_F ? (uint16_t)(R_ADD_F + (op - OP_ADD_F))
                                                           : (uint16_t)(R_EQ_B + (op - OP_EQ_B));
            size_t at = EMIT(t, rop, TREG(t, a), ra, rb);
            t->depth--;
            t->st[a].kind = E_TEMP; t->st[a].scalar = true;
            tx_def(t, at + 1, a);
            break;
        }
        case OP_NEG_I: case OP_NEG_F: case OP_NOT: {
            NEED(1);
            uint16_t ra = tx_sreg(t, TOP);
            uint16_t rop = op == OP_NEG_I ? R_NEG_I : op == OP_NEG_F ? R_NEG_F : R_NOT;
            size_t at = EMIT(t, rop, TREG(t, TOP), ra);
            t->st[TOP].kind = E_TEMP; t->st[TOP].scalar = true;
            tx_def(t, at + 1, TOP);
            break;
        }
        case OP_EQ_I: case OP_NE_I: case OP_LT_I: case OP_LE_I: case OP_GT_I: case OP_GE_I: {
            NEED(2);
            int a = t->depth - 2, b = t->depth - 1;
            tent A = t->st[a], B = t->st[b];
            tent c; memset(&c, 0, sizeof c);
            c.kind = E_CMP; c.cmp = (uint8_t)(op - OP_EQ_I);
            if (A.kind == E_IMM) tx_materialize(t, a);
            c.reg = tx_sreg(t, a);
            if (B.kind == E_IMM) { c.b_imm = true; c.b = B.b; }
            else c.b = tx_sreg(t, b);
            t->depth--;
            t->st[a] = c;
            break;
        }
        case OP_EQ_F: case OP_NE_F: case OP_LT_F: case OP_LE_F: case OP_GT_F: case OP_GE_F: {
            NEED(2);
            int a = t->depth - 2, b = t->depth - 1;
            tent c; memset(&c, 0, sizeof c);
            c.kind = E_CMP; c.is_float = true; c.cmp = (uint8_t)(op - OP_EQ_F);
            c.reg = tx_sreg(t, a);
            c.b = tx_sreg(t, b);
            t->depth--;
            t->st[a] = c;
            break;
        }

        case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE: {
            NEED(1);
            bool when_true = op == OP_JUMP_IF_TRUE;
            tent c = t->st[TOP];
            t->depth--;
            tx_flush(t, t->depth);
            int16_t d = (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
            int target = (int)off + 3 + d;
            size_t at;
            if (c.kind == E_CMP) {
                if (c.b_imm) {
                    uint8_t cm = when_true ? c.cmp : cmp_inverse[c.cmp];
                    at = EMIT(t, R_JCI_EQ + cm, c.reg, (uint16_t)(int16_t)c.b, 0);
                } else if (!c.is_float) {
                    uint8_t cm = when_true ? c.cmp : cmp_inverse[c.cmp];
                    at = EMIT(t, R_JC_EQ_I + cm, c.reg, (uint16_t)c.b, 0);
                } else if (when_true) {
                    at = EMIT(t, R_JC_EQ_F + c.cmp, c.reg, (uint16_t)c.b, 0);
                } else {
                    at = EMIT(t, R_JN_EQ_F + c.cmp, c.reg, (uint16_t)c.b, 0);
                }
                (void)at;
                tx_jump_fix(t, t->nout - 1, target);
            } else {
                uint16_t r;
                if (c.kind == E_LOCAL) r = c.reg;
                else { t->st[t->depth] = c; t->depth++; r = tx_sreg(t, t->depth - 1); t->depth--; }
                EMIT(t, when_true ? R_JT : R_JF, r, 0);
                tx_jump_fix(t, t->nout - 1, target);
            }
            if (target <= (int)n && t->depth_at[target] < 0) t->depth_at[target] = t->depth;
            break;
        }
        case OP_JUMP: {
            tx_flush(t, t->depth);
            int16_t d = (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
            int target = (int)off + 3 + d;
            // A loop whose condition is a single branch at the top is closed with
            // the opposite branch at the bottom instead of a jump back to the top:
            // one dispatch per iteration less.
            if (target <= (int)off && t->newpos[target] >= 0 && tx_invert_loop(t, target, (int)off + 3)) {
                if (target <= (int)n && t->depth_at[off + 3] < 0) t->depth_at[off + 3] = t->depth;
                break;
            }
            EMIT(t, R_JMP, 0);
            tx_jump_fix(t, t->nout - 1, target);
            if (target <= (int)n && t->depth_at[target] < 0) t->depth_at[target] = t->depth;
            t->reachable = false;
            break;
        }

        case OP_CONCAT: case OP_EQ_S: case OP_NE_S: {
            NEED(2);
            int a = t->depth - 2, b = t->depth - 1;
            tx_materialize(t, a); tx_materialize(t, b);
            uint16_t rop = op == OP_CONCAT ? R_CONCAT : op == OP_EQ_S ? R_EQ_S : R_NE_S;
            EMIT(t, rop, TREG(t, a), TREG(t, a), TREG(t, b));
            t->depth--;
            t->st[a].kind = E_TEMP; t->st[a].scalar = op != OP_CONCAT;
            break;
        }
        case OP_STR_I: case OP_STR_F: case OP_STR_B: {
            NEED(1);
            uint16_t ra = tx_sreg(t, TOP);
            EMIT(t, R_STR_I + (op - OP_STR_I), TREG(t, TOP), ra);
            t->st[TOP].kind = E_TEMP; t->st[TOP].scalar = false;
            break;
        }
        case OP_STR_OBJ: {
            NEED(1);
            tx_materialize(t, TOP);
            EMIT(t, R_STR_OBJ, TREG(t, TOP), TREG(t, TOP));
            t->st[TOP].scalar = false;
            break;
        }

        case OP_CALL: case OP_NATIVE: {
            uint16_t id = (uint16_t)(p[1] | (p[2] << 8));
            int argc = p[3];
            bool nres = p[4] != 0;
            NEED(argc);
            if (op == OP_CALL && id >= vm->nfuncs) { t->bad = true; goto out; }
            if (op == OP_NATIVE && id >= vm->nnative_names) { t->bad = true; goto out; }
            int first = t->depth - argc;
            for (int k = first; k < t->depth; k++) tx_materialize(t, k);
            uint16_t base = TREG(t, first);
            if (op == OP_CALL) EMIT(t, R_CALL, id, base);
            else EMIT(t, R_NATIVE, id, base, (uint16_t)argc, nres ? 1 : 0);
            t->depth = first;
            if (nres) tx_push_temp(t, false);
            break;
        }
        case OP_RET: {
            NEED(1);
            // The frame is about to die, so a local can be handed over as it is.
            if (t->st[TOP].kind == E_LOCAL) {
                EMIT(t, R_RET, t->st[TOP].reg);
            } else {
                tx_materialize(t, TOP);
                EMIT(t, R_RET, TREG(t, TOP));
            }
            t->reachable = false;
            break;
        }
        case OP_RET_VOID: EMIT(t, R_RETV); t->reachable = false; break;
        case OP_HALT:     EMIT(t, R_HALT); t->reachable = false; break;

        case OP_NEW_ARRAY: case OP_NEW_STRUCT: case OP_NEW_MAP: case OP_NEW_VARIANT: {
            uint16_t cnt, tag = 0;
            if (op == OP_NEW_VARIANT) { tag = (uint16_t)(p[1] | (p[2] << 8)); cnt = p[3]; }
            else cnt = (uint16_t)(p[1] | (p[2] << 8));
            uint16_t tid = op == OP_NEW_STRUCT ? (uint16_t)(p[3] | (p[4] << 8)) : 0;
            int items = op == OP_NEW_MAP ? 2 * cnt : cnt;
            NEED(items);
            int first = t->depth - items;
            for (int k = first; k < t->depth; k++) tx_materialize(t, k);
            uint16_t base = TREG(t, first);
            switch (op) {
                case OP_NEW_ARRAY:   EMIT(t, R_NEWARR, base, cnt); break;
                case OP_NEW_STRUCT:  EMIT(t, R_NEWSTRUCT, base, cnt, tid); break;
                case OP_NEW_MAP:     EMIT(t, R_NEWMAP, base, cnt); break;
                default:             EMIT(t, R_NEWVAR, base, tag, cnt); break;
            }
            t->depth = first;
            tx_push_temp(t, false);
            break;
        }

        case OP_AGET: {
            NEED(2);
            int a = t->depth - 2, b = t->depth - 1;
            uint16_t ri = tx_sreg(t, b);
            if (t->st[a].kind == E_LOCAL) {
                t->last_aget_at = EMIT(t, R_AGET_L, TREG(t, a), t->st[a].reg, ri);
            } else {
                tx_materialize(t, a);
                EMIT(t, R_AGET, TREG(t, a), TREG(t, a), ri);
            }
            t->depth--;
            t->st[a].kind = E_TEMP; t->st[a].scalar = false;
            break;
        }
        case OP_ASET: {
            NEED(3);
            int a = t->depth - 3, i = t->depth - 2, v = t->depth - 1;
#ifndef SL_AOT_EMITTER
            if (t->st[v].kind == E_LOCAL && t->st[a].kind == E_LOCAL) {
                uint16_t ri2 = tx_sreg(t, i);
                EMIT(t, R_ASET_LC, t->st[a].reg, ri2, t->st[v].reg);
                t->depth -= 3;
                break;
            }
#endif
            tx_materialize(t, v);
            uint16_t ri = tx_sreg(t, i);
            if (t->st[a].kind == E_LOCAL) EMIT(t, R_ASET_L, t->st[a].reg, ri, TREG(t, v));
            else { tx_materialize(t, a); EMIT(t, R_ASET, TREG(t, a), ri, TREG(t, v)); }
            t->depth -= 3;
            break;
        }
        case OP_ALEN: {
            NEED(1);
            if (t->st[TOP].kind == E_LOCAL) EMIT(t, R_ALEN_L, TREG(t, TOP), t->st[TOP].reg);
            else { tx_materialize(t, TOP); EMIT(t, R_ALEN, TREG(t, TOP), TREG(t, TOP)); }
            t->st[TOP].kind = E_TEMP; t->st[TOP].scalar = true;
            break;
        }
        case OP_APUSH: {
            NEED(2);
            int a = t->depth - 2, v = t->depth - 1;
#ifndef SL_AOT_EMITTER
            if (t->st[v].kind == E_LOCAL && t->st[a].kind == E_LOCAL) {
                EMIT(t, R_APUSH_LC, t->st[a].reg, t->st[v].reg);
                t->depth -= 2;
                break;
            }
#endif
            tx_materialize(t, v);
            if (t->st[a].kind == E_LOCAL) EMIT(t, R_APUSH_L, t->st[a].reg, TREG(t, v));
            else { tx_materialize(t, a); EMIT(t, R_APUSH, TREG(t, a), TREG(t, v)); }
            t->depth -= 2;
            break;
        }
        case OP_SLICE: {
            NEED(3);
            int a = t->depth - 3, lo = t->depth - 2, hi = t->depth - 1;
            tx_materialize(t, a);
            uint16_t rl = tx_sreg(t, lo), rh = tx_sreg(t, hi);
            EMIT(t, R_SLICE, TREG(t, a), TREG(t, a), rl, rh);
            t->depth -= 2;
            t->st[a].kind = E_TEMP; t->st[a].scalar = false;
            break;
        }
        case OP_FGET: {
            NEED(1);
            uint16_t idx = p[1];
            if (t->st[TOP].kind == E_LOCAL) EMIT(t, R_FGET_L, TREG(t, TOP), t->st[TOP].reg, idx);
            else { tx_materialize(t, TOP); EMIT(t, R_FGET, TREG(t, TOP), TREG(t, TOP), idx); }
            t->st[TOP].kind = E_TEMP; t->st[TOP].scalar = false;
            break;
        }
        case OP_FSET: {
            NEED(2);
            uint16_t idx = p[1];
            int a = t->depth - 2, v = t->depth - 1;
            tx_materialize(t, v);
            if (t->st[a].kind == E_LOCAL) EMIT(t, R_FSET_L, t->st[a].reg, idx, TREG(t, v));
            else { tx_materialize(t, a); EMIT(t, R_FSET, TREG(t, a), idx, TREG(t, v)); }
            t->depth -= 2;
            break;
        }
        case OP_VTAG: {
            NEED(1);
            tx_materialize(t, TOP);
            EMIT(t, R_VTAG, TREG(t, TOP), TREG(t, TOP));
            t->st[TOP].scalar = true;
            break;
        }
        case OP_VGET: {
            NEED(1);
            tx_materialize(t, TOP);
            EMIT(t, R_VGET, TREG(t, TOP), TREG(t, TOP), p[1]);
            t->st[TOP].scalar = false;
            break;
        }
        case OP_COPY: {
            NEED(1);
            tx_materialize(t, TOP);
            EMIT(t, R_COPY, TREG(t, TOP), TREG(t, TOP), (uint16_t)(p[1] | (p[2] << 8)));
            t->st[TOP].scalar = false;
            break;
        }
        case OP_LOADG: {
            uint16_t g = (uint16_t)(p[1] | (p[2] << 8));
            if (g >= vm->nglobals) { t->bad = true; goto out; }
            uint16_t r = tx_push_temp(t, false);
            EMIT(t, R_LOADG, r, g);
            break;
        }
        case OP_STOREG: {
            NEED(1);
            uint16_t g = (uint16_t)(p[1] | (p[2] << 8));
            if (g >= vm->nglobals) { t->bad = true; goto out; }
            tx_materialize(t, TOP);
            EMIT(t, R_STOREG, g, TREG(t, TOP));
            t->depth--;
            break;
        }
        case OP_MGET: {
            NEED(2);
            int a = t->depth - 2, b = t->depth - 1;
            tx_materialize(t, a); tx_materialize(t, b);
            EMIT(t, R_MGET, TREG(t, a), TREG(t, a), TREG(t, b));
            t->depth--;
            t->st[a].kind = E_TEMP; t->st[a].scalar = false;
            break;
        }
        case OP_MSET: {
            NEED(3);
            int a = t->depth - 3;
            tx_materialize(t, a); tx_materialize(t, a + 1); tx_materialize(t, a + 2);
            EMIT(t, R_MSET, TREG(t, a), TREG(t, a + 1), TREG(t, a + 2));
            t->depth -= 3;
            break;
        }
        default:
            t->bad = true;
            goto out;
        }
#undef NEED
#undef TOP
        off += (size_t)len;
    }
    if (t->is_target[n]) {
        if (t->reachable) tx_flush(t, t->depth);
        t->newpos[n] = (int)t->nout;
    }
out:
    return !t->bad;
}

static void tx_free(tx *t) {
    free(t->st); free(t->newpos); free(t->depth_at); free(t->is_target); free(t->in_loop); free(t->fix);
}

#include "opt.inc"

static bool translate_function(sl_vm *vm, sl_func *f) {
    size_t nslots = (size_t)f->nslots + 1;
    bool *obj_slot = (bool *)calloc(nslots, sizeof(bool));
    if (!obj_slot) return false;
    tx t;
    uint64_t kbits[TX_MAX_HOIST];
    uint8_t kfloat[TX_MAX_HOIST];
    int nk = 0;
    bool ok = translate_pass(vm, f, obj_slot, false, &t, kbits, kfloat, &nk);
    if (ok) {
        free(t.out); tx_free(&t);
        ok = translate_pass(vm, f, obj_slot, true, &t, kbits, kfloat, &nk);
    }
    free(obj_slot);
    if (!ok) { free(t.out); tx_free(&t); return false; }

    for (int i = 0; i < t.nfix; i++) {
        int tgt = t.newpos[t.fix[i].target];
        if (tgt < 0) { ok = false; break; }
        long rel = (long)tgt - (long)(t.fix[i].at + 1);
        if (rel < -32768 || rel > 32767) { ok = false; break; }
        t.out[t.fix[i].at] = (uint16_t)(int16_t)rel;
    }
    long nregs = (long)t.tbase + t.maxdepth;
    if (!ok || nregs > 60000) { free(t.out); tx_free(&t); return false; }
    f->rcode = t.out;
    f->rlen = (uint32_t)t.nout;
    f->nregs = (uint16_t)nregs;
    tx_free(&t);
    return true;
}

static sl_result link_check(sl_vm *vm) {
    enum { SHOWN = 6 };
    int problems = 0;
    size_t used = 0;
    used += (size_t)snprintf(vm->err, sizeof vm->err, "cannot link the program to this host:");
    for (uint16_t i = 0; i < vm->nnative_names; i++) {
        int j = vm->native_bind[i];
        const char *name = vm->native_names[i];
        const char *want = vm->native_sigs[i] ? vm->native_sigs[i] : "";
        const char *have = j >= 0 ? vm->registry[j].sig : NULL;
        char line[320] = "";
        if (j < 0)
            snprintf(line, sizeof line, "\n  `%s`%s%s is declared extern but the host did not register it",
                     name, *want ? " " : "", want);
        else if (*want && have && !sig_equal(want, have))
            snprintf(line, sizeof line, "\n  `%s`: the script declares %s but the host registered %s",
                     name, want, have);
        else
            continue;
        problems++;
        if (problems <= SHOWN && used < sizeof vm->err) used += (size_t)snprintf(vm->err + used, sizeof vm->err - used, "%s", line);
    }
    if (!problems) { vm->err[0] = 0; return SL_OK; }
    if (problems > SHOWN && used < sizeof vm->err)
        snprintf(vm->err + used, sizeof vm->err - used, "\n  ... and %d more", problems - SHOWN);
    vm->failed = true;
    return SL_ERR_LINK;
}

sl_result sl_load(sl_vm *vm, const uint8_t *bytes, size_t len) {
    reader r = { bytes, bytes + len, false };

    if (len < 6 || memcmp(bytes, "SLBC", 4) != 0) {
        vm_error(vm, "not a Solis bytecode image");
        return SL_ERR_BADFILE;
    }
    r.p += 4;
    // A VM may load another image. The shared string constants belong to the old one.
    if (vm->kstr) {
        for (uint16_t i = 0; i < vm->nconsts; i++) if (vm->kstr[i].type == SL_OBJ) sl_release(vm, vm->kstr[i]);
        free(vm->kstr);
        vm->kstr = NULL;
        vm->nkstr = 0;
    }
    uint16_t version = rd_u16(&r);
    if (version != 9) {
        vm_error(vm, "bytecode version %u, this runtime understands 9", version);
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
    vm->native_sigs  = (char **)calloc(vm->nnative_names ? vm->nnative_names : 1, sizeof(char *));
    vm->native_bind  = (int  *)calloc(vm->nnative_names ? vm->nnative_names : 1, sizeof(int));
    if (!vm->native_names || !vm->native_sigs || !vm->native_bind) return SL_ERR_NOMEM;
    for (uint16_t i = 0; i < vm->nnative_names && !r.bad; i++) {
        uint16_t n = rd_u16(&r);
        vm->native_names[i] = rd_bytes(&r, n);
        uint16_t sn = rd_u16(&r);
        vm->native_sigs[i] = rd_bytes(&r, sn);
        vm->native_bind[i] = -1;
    }

    vm->nstructs = rd_u16(&r);
    vm->struct_desc = (uint8_t (*)[33])calloc(vm->nstructs ? vm->nstructs : 1, 33);
    if (!vm->struct_desc) return SL_ERR_NOMEM;
    for (uint16_t i = 0; i < vm->nstructs && !r.bad; i++) {
        uint8_t n = rd_u8(&r);
        vm->struct_desc[i][0] = n;
        for (uint8_t k = 0; k < n && k < 32; k++) vm->struct_desc[i][1 + k] = rd_u8(&r);
    }

    vm->ready = false;

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

    for (uint16_t i = 0; i < vm->nfuncs; i++) {
        if (!translate_function(vm, &vm->funcs[i])) {
            vm_error(vm, "function '%s' is malformed or too large", vm->funcs[i].name);
            return SL_ERR_BADFILE;
        }
        free(vm->funcs[i].code);
        vm->funcs[i].code = NULL;
    }
    for (uint16_t i = 0; i < vm->nfuncs; i++) opt_function(vm, &vm->funcs[i], i);

    // Last registration wins, so a host can shadow an intrinsic.
    for (uint16_t i = 0; i < vm->nnative_names; i++)
        for (int j = vm->nregistered - 1; j >= 0; j--)
            if (strcmp(vm->native_names[i], vm->registry[j].name) == 0) { vm->native_bind[i] = j; break; }

    return link_check(vm);
}

static inline void set_int(sl_value *p, int64_t v)  { p->type = SL_INT;   p->as.i = v; }
static inline void set_flt(sl_value *p, double v)   { p->type = SL_FLOAT; p->as.f = v; }
static inline void set_bool(sl_value *p, bool v)    { p->type = SL_BOOL;  p->as.i = v; }

#if defined(__GNUC__)
#define SL_NOINLINE __attribute__((noinline))
#else
#define SL_NOINLINE
#endif

// Kept out of line on purpose. If gcc vectorises this loop inside sl_run, its
// constants end up in the shared dispatch block and gcc stops giving each
// handler its own indirect jump.
static SL_NOINLINE int64_t utf8_length(sl_value s) {
    size_t bn;
    const char *p = sl_as_str(s, &bn);
    int64_t n = 0;
    for (size_t k = 0; k < bn; k++) if ((p[k] & 0xC0) != 0x80) n++;
    return n;
}

static sl_value str_of_int(sl_vm *vm, int64_t v) {
    char tmp[24];
    char *e = tmp + sizeof tmp, *p = e;
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    do { *--p = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) *--p = '-';
    sl_string *s = str_alloc(vm, (size_t)(e - p));
    if (!s) return sl_void();
    memcpy(s->bytes, p, (size_t)(e - p));
    return obj_value(&s->head);
}

// Dropping a reference that is not the last one needs no call.
static inline void rel(sl_vm *vm, sl_value v) {
    if (v.type != SL_OBJ || !v.as.o) return;
    if (v.as.o->rc > 1) v.as.o->rc--;
    else sl_release(vm, v);
}
// Release a heap value held in a register and leave the register void.
static inline void drop(sl_vm *vm, sl_value *p) {
    if (p->type == SL_OBJ) { sl_value v = vload(p); p->type = SL_VOID; rel(vm, v); }
}
// The register's reference moved elsewhere.
static inline void moved(sl_value *p) { if (p->type == SL_OBJ) p->type = SL_VOID; }

// Release everything the stack still owns, up to `end`.
static void release_stack(sl_vm *vm, sl_value *end) {
    for (sl_value *p = vm->stack; p < end; p++) drop(vm, p);
}

// Threaded dispatch: every handler ends in its own indirect jump, so the
// branch predictor sees one site per opcode instead of one shared site.
// Compilers without labels-as-values (or -DSL_NO_THREADED) get a plain switch.
#if defined(__GNUC__) && !defined(SL_NO_THREADED) && !defined(__cplusplus)
#define SL_THREADED 1
// Labels-as-values and range designators are GNU extensions.
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Woverride-init"
#define OPC(name)  L_##name
#define NEXT()     goto *dispatch[*ip++]
#define SL_DISPATCH_BEGIN  NEXT(); {
#define SL_DISPATCH_END    }
#else
#define OPC(name)  case name
#define NEXT()     break
#define SL_DISPATCH_BEGIN  for (;;) { switch (*ip++) {
#define SL_DISPATCH_END    } }
#endif

#define RR(i)   R[ip[i]]
#define IMM(i)  ((int16_t)ip[i])
#define OFFSET(i) ((int16_t)ip[i])

// Long handlers that are rarely hot live out of line, which keeps the hot
// handlers small enough for the compiler to allocate registers well.
static SL_NOINLINE bool opf_CONCAT(sl_vm *vm, sl_value *R, uint16_t *ip) {
            sl_value a = vload(&RR(1)), b = vload(&RR(2));
            size_t an, bn;
            const char *ap = sl_as_str(a, &an), *bp = sl_as_str(b, &bn);
            sl_string *joined = str_alloc(vm, an + bn);
            if (!joined) return false;
            memcpy(joined->bytes, ap, an);
            memcpy(joined->bytes + an, bp, bn);
            drop(vm, &RR(1)); drop(vm, &RR(2));
            vstore(&RR(0), obj_value(&joined->head));
    return true;
}

static SL_NOINLINE bool opf_STR_F(sl_vm *vm, sl_value *R, uint16_t *ip) {
            char tmp[64];
            fmt_float(tmp, sizeof tmp, RR(1).as.f);
            sl_value s = sl_str(vm, tmp, strlen(tmp));
            if (vm->failed) return false;
            vstore(&RR(0), s);
    return true;
}

static SL_NOINLINE bool opf_STR_B(sl_vm *vm, sl_value *R, uint16_t *ip) {
            sl_value s = RR(1).as.b ? sl_str(vm, "true", 4) : sl_str(vm, "false", 5);
            if (vm->failed) return false;
            vstore(&RR(0), s);
    return true;
}

static SL_NOINLINE bool opf_STR_OBJ(sl_vm *vm, sl_value *R, uint16_t *ip) {
            sl_value v = vload(&RR(1));
            sl_value s = sl_to_string(vm, v);
            drop(vm, &RR(1));
            if (vm->failed) return false;
            vstore(&RR(0), s);
    return true;
}

static SL_NOINLINE bool opf_STR_I(sl_vm *vm, sl_value *R, uint16_t *ip) {
            sl_value s = str_of_int(vm, RR(1).as.i);
            if (vm->failed) return false;
            vstore(&RR(0), s);
    return true;
}

static SL_NOINLINE bool opf_SLICE(sl_vm *vm, sl_value *R, uint16_t *ip) {
            sl_value av = vload(&RR(1));
            if (av.type != SL_OBJ || !av.as.o || av.as.o->kind != SL_ARRAY) { vm_error(vm, "not an array"); return false; }
            sl_arr *a = (sl_arr *)av.as.o;
            int64_t n = a->len;
            int64_t lo = RR(2).as.i, hi = RR(3).as.i, lo0 = lo, hi0 = hi;
            if (lo < 0) lo += n;
            if (hi < 0) hi += n;
            if (lo < 0 || hi > n || lo > hi) {
                vm_error(vm, "slice [%lld:%lld] is out of bounds for an array of length %lld",
                         (long long)lo0, (long long)hi0, (long long)n);
                return false;
            }
            sl_value out = sl_array(vm, hi - lo);
            if (vm->failed) return false;
            sl_arr *oa = (sl_arr *)out.as.o;
            for (int64_t k = lo; k < hi; k++) {
                sl_value item = arr_get(a, k);
                sl_retain(item);
                if (!array_push(vm, oa, item)) { rel(vm, out); return false; }
            }
            drop(vm, &RR(1));
            vstore(&RR(0), out);
    return true;
}

static SL_NOINLINE bool opf_NEWVAR(sl_vm *vm, sl_value *R, uint16_t *ip) {
            uint16_t tag = ip[1], n = ip[2];
            sl_variant *t = (sl_variant *)malloc(sizeof(sl_variant) + (n ? n - 1 : 0) * sizeof(sl_value));
            if (!t) { vm_error(vm, "out of memory"); return false; }
            t->head.rc = 1; t->head.kind = SL_VARIANT; t->tag = tag; t->n = n;
            sl_value *items = &RR(0);
            for (uint16_t i = 0; i < n; i++) { vstore(&t->payload[i], vload(&items[i])); items[i].type = SL_VOID; }
            vm->live++;
            vstore(&RR(0), obj_value(&t->head));
    return true;
}

static SL_NOINLINE bool opf_NEWMAP(sl_vm *vm, sl_value *R, uint16_t *ip) {
            uint16_t n = ip[1];
            sl_value mv = sl_map(vm, n);
            if (vm->failed) return false;
            sl_value *items = &RR(0);
            for (uint16_t i = 0; i < n; i++) {
                sl_value k = vload(&items[2 * i]), v = vload(&items[2 * i + 1]);
                items[2 * i].type = SL_VOID; items[2 * i + 1].type = SL_VOID;
                if (!sl_map_set(vm, mv, k, v)) { rel(vm, mv); return false; }
            }
            vstore(&RR(0), mv);
    return true;
}

static SL_NOINLINE bool opf_MGET(sl_vm *vm, sl_value *R, uint16_t *ip) {
            sl_value mv = vload(&RR(1)), kv = vload(&RR(2));
            if (mv.type != SL_OBJ || !mv.as.o || mv.as.o->kind != SL_MAP) { vm_error(vm, "not a map"); return false; }
            sl_value out;
            if (!sl_map_get(mv, kv, &out)) { vm_error(vm, "key not found"); return false; }
            sl_retain(out);
            drop(vm, &RR(2)); drop(vm, &RR(1));
            vstore(&RR(0), out);
    return true;
}

static SL_NOINLINE bool opf_MSET(sl_vm *vm, sl_value *R, uint16_t *ip) {
            sl_value mv = vload(&RR(0)), kv = vload(&RR(1)), val = vload(&RR(2));
            if (mv.type != SL_OBJ || !mv.as.o || mv.as.o->kind != SL_MAP) { vm_error(vm, "not a map"); return false; }
            moved(&RR(1)); moved(&RR(2));
            if (!sl_map_set(vm, mv, kv, val)) return false;
            drop(vm, &RR(0));
    return true;
}

static SL_NOINLINE bool opf_VGET(sl_vm *vm, sl_value *R, uint16_t *ip) {
            sl_value v = vload(&RR(1));
            uint16_t idx = ip[2];
            if (v.type != SL_OBJ || !v.as.o || v.as.o->kind != SL_VARIANT) { vm_error(vm, "not an enum value"); return false; }
            sl_variant *t = (sl_variant *)v.as.o;
            if (idx >= t->n) { vm_error(vm, "bad payload index"); return false; }
            sl_value e = vload(&t->payload[idx]);
            sl_retain(e);
            drop(vm, &RR(1));
            vstore(&RR(0), e);
    return true;
}

static SL_NOINLINE bool opf_VTAG(sl_vm *vm, sl_value *R, uint16_t *ip) {
            sl_value v = vload(&RR(1));
            if (v.type != SL_OBJ || !v.as.o || v.as.o->kind != SL_VARIANT) { vm_error(vm, "not an enum value"); return false; }
            uint16_t tag = ((sl_variant *)v.as.o)->tag;
            drop(vm, &RR(1));
            set_int(&RR(0), tag);
    return true;
}

static SL_NOINLINE bool opf_COPY(sl_vm *vm, sl_value *R, uint16_t *ip) {
            sl_value v = vload(&RR(1));
            sl_value c = struct_copy(vm, v);
            drop(vm, &RR(1));
            if (vm->failed) return false;
            vstore(&RR(0), c);
    return true;
}

static SL_NOINLINE bool opf_NEWSTRUCT(sl_vm *vm, sl_value *R, uint16_t *ip) {
            uint16_t n = ip[1];
            sl_struct *s = struct_alloc(vm, n);
            if (!s) { vm_error(vm, "out of memory"); return false; }
            s->head.rc = 1; s->head.kind = SL_STRUCT; s->nfields = n;
            sl_value *items = &RR(0);
            for (uint16_t i = 0; i < n; i++) { vstore(&s->fields[i], vload(&items[i])); items[i].type = SL_VOID; }
            vm->live++;
            vstore(&RR(0), obj_value(&s->head));
    return true;
}

static SL_NOINLINE bool opf_NEWARR(sl_vm *vm, sl_value *R, uint16_t *ip) {
            uint16_t n = ip[1];
            sl_value av = sl_array(vm, n);
            if (vm->failed) return false;
            sl_arr *a = (sl_arr *)av.as.o;
            sl_value *items = &RR(0);
            for (uint16_t i = 0; i < n; i++) {
                bool ok = array_push(vm, a, vload(&items[i]));
                items[i].type = SL_VOID;
                if (!ok) { rel(vm, av); return false; }
            }
            vstore(&RR(0), av);
    return true;
}

static SL_NOINLINE bool opf_LDS(sl_vm *vm, sl_value *R, uint16_t *ip) {
            sl_value s = const_str(vm, ip[1]);
            if (vm->failed) return false;
            vstore(&RR(0), s);
    return true;
}



// Left alone, gcc merges identical handler tails back into one shared jump,
// which defeats threading, and turns short conditional jumps into conditional
// moves, which makes the next instruction wait on the comparison instead of
// on a branch prediction. These options turn both off for sl_run.
#if defined(SL_THREADED) && defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-gcse", "no-crossjumping", "no-tree-slp-vectorize", "no-if-conversion", "no-if-conversion2")))
#endif
static sl_result run_function(sl_vm *vm, uint16_t id, int argc, const sl_value *argv) {
    if (vm->nfuncs == 0) { vm_error(vm, "nothing loaded"); return SL_ERR_RUNTIME; }

    sl_value *const stack_end = vm->stack + SL_STACK_MAX;
    sl_func *fn = &vm->funcs[id];
    sl_value *R = vm->stack;
    if (R + fn->nregs > stack_end) { vm_error(vm, "stack overflow"); return SL_ERR_RUNTIME; }
    vm->nframes = 0;
    vm->failed = false;
    vm->err[0] = 0;
    vm->ret = sl_void();
    // Parameters are the first registers of a frame. The callee owns and drops them, so give it its own reference.
    for (int i = 0; i < argc; i++) { sl_retain(argv[i]); vstore(&R[i], argv[i]); }
    sl_frame *const frames_first = vm->frames;
    sl_frame *const frames_last = vm->frames + SL_FRAMES_MAX - 1;
    sl_frame *fr = frames_first;
    fr->fn = fn; fr->ip = fn->rcode; fr->base = R;
    uint16_t *ip = fn->rcode;
    bool want_equal, array_owned;

#ifdef SL_THREADED
    static const void *const dispatch[256] = {
        [0 ... 255] = &&L_unknown,
        [R_LDI] = &&L_R_LDI,
        [R_LDC_I] = &&L_R_LDC_I,
        [R_LDC_F] = &&L_R_LDC_F,
        [R_LDS] = &&L_R_LDS,
        [R_LDB] = &&L_R_LDB,
        [R_LDNULL] = &&L_R_LDNULL,
        [R_MOV] = &&L_R_MOV,
        [R_MOVS] = &&L_R_MOVS,
        [R_STORE] = &&L_R_STORE,
        [R_STORE_L] = &&L_R_STORE_L,
        [R_DROP] = &&L_R_DROP,
        [R_ADD_I] = &&L_R_ADD_I,
        [R_SUB_I] = &&L_R_SUB_I,
        [R_MUL_I] = &&L_R_MUL_I,
        [R_DIV_I] = &&L_R_DIV_I,
        [R_REM_I] = &&L_R_REM_I,
        [R_ADDI] = &&L_R_ADDI,
        [R_NEG_I] = &&L_R_NEG_I,
        [R_ADD_F] = &&L_R_ADD_F,
        [R_SUB_F] = &&L_R_SUB_F,
        [R_MUL_F] = &&L_R_MUL_F,
        [R_DIV_F] = &&L_R_DIV_F,
        [R_REM_F] = &&L_R_REM_F,
        [R_NEG_F] = &&L_R_NEG_F,
        [R_EQ_I] = &&L_R_EQ_I,
        [R_NE_I] = &&L_R_NE_I,
        [R_LT_I] = &&L_R_LT_I,
        [R_LE_I] = &&L_R_LE_I,
        [R_GT_I] = &&L_R_GT_I,
        [R_GE_I] = &&L_R_GE_I,
        [R_EQ_F] = &&L_R_EQ_F,
        [R_NE_F] = &&L_R_NE_F,
        [R_LT_F] = &&L_R_LT_F,
        [R_LE_F] = &&L_R_LE_F,
        [R_GT_F] = &&L_R_GT_F,
        [R_GE_F] = &&L_R_GE_F,
        [R_EQ_B] = &&L_R_EQ_B,
        [R_NE_B] = &&L_R_NE_B,
        [R_NOT] = &&L_R_NOT,
        [R_EQ_S] = &&L_R_EQ_S,
        [R_NE_S] = &&L_R_NE_S,
        [R_CONCAT] = &&L_R_CONCAT,
        [R_STR_I] = &&L_R_STR_I,
        [R_STR_F] = &&L_R_STR_F,
        [R_STR_B] = &&L_R_STR_B,
        [R_STR_OBJ] = &&L_R_STR_OBJ,
        [R_JMP] = &&L_R_JMP,
        [R_JT] = &&L_R_JT,
        [R_JF] = &&L_R_JF,
        [R_JC_EQ_I] = &&L_R_JC_EQ_I,
        [R_JC_NE_I] = &&L_R_JC_NE_I,
        [R_JC_LT_I] = &&L_R_JC_LT_I,
        [R_JC_LE_I] = &&L_R_JC_LE_I,
        [R_JC_GT_I] = &&L_R_JC_GT_I,
        [R_JC_GE_I] = &&L_R_JC_GE_I,
        [R_JC_EQ_F] = &&L_R_JC_EQ_F,
        [R_JC_NE_F] = &&L_R_JC_NE_F,
        [R_JC_LT_F] = &&L_R_JC_LT_F,
        [R_JC_LE_F] = &&L_R_JC_LE_F,
        [R_JC_GT_F] = &&L_R_JC_GT_F,
        [R_JC_GE_F] = &&L_R_JC_GE_F,
        [R_JN_EQ_F] = &&L_R_JN_EQ_F,
        [R_JN_NE_F] = &&L_R_JN_NE_F,
        [R_JN_LT_F] = &&L_R_JN_LT_F,
        [R_JN_LE_F] = &&L_R_JN_LE_F,
        [R_JN_GT_F] = &&L_R_JN_GT_F,
        [R_JN_GE_F] = &&L_R_JN_GE_F,
        [R_JCI_EQ] = &&L_R_JCI_EQ,
        [R_JCI_NE] = &&L_R_JCI_NE,
        [R_JCI_LT] = &&L_R_JCI_LT,
        [R_JCI_LE] = &&L_R_JCI_LE,
        [R_JCI_GT] = &&L_R_JCI_GT,
        [R_JCI_GE] = &&L_R_JCI_GE,
        [R_CALL] = &&L_R_CALL,
        [R_NATIVE] = &&L_R_NATIVE,
        [R_RET] = &&L_R_RET,
        [R_RETV] = &&L_R_RETV,
        [R_HALT] = &&L_R_HALT,
        [R_NEWARR] = &&L_R_NEWARR,
        [R_AGET] = &&L_R_AGET,
        [R_AGET_L] = &&L_R_AGET_L,
        [R_ASET] = &&L_R_ASET,
        [R_ASET_L] = &&L_R_ASET_L,
        [R_ALEN] = &&L_R_ALEN,
        [R_ALEN_L] = &&L_R_ALEN_L,
        [R_APUSH] = &&L_R_APUSH,
        [R_APUSH_L] = &&L_R_APUSH_L,
        [R_SLICE] = &&L_R_SLICE,
        [R_NEWSTRUCT] = &&L_R_NEWSTRUCT,
        [R_FGET] = &&L_R_FGET,
        [R_FGET_L] = &&L_R_FGET_L,
        [R_FSET] = &&L_R_FSET,
        [R_FSET_L] = &&L_R_FSET_L,
        [R_NEWVAR] = &&L_R_NEWVAR,
        [R_VTAG] = &&L_R_VTAG,
        [R_VGET] = &&L_R_VGET,
        [R_LOADG] = &&L_R_LOADG,
        [R_STOREG] = &&L_R_STOREG,
        [R_COPY] = &&L_R_COPY,
        [R_NEWMAP] = &&L_R_NEWMAP,
        [R_MGET] = &&L_R_MGET,
        [R_MSET] = &&L_R_MSET,
        [R_AJLT] = &&L_R_AJLT,
        [R_AJLTI] = &&L_R_AJLTI,
        [R_JLTLEN] = &&L_R_JLTLEN,
        [R_MOV2] = &&L_R_MOV2,
        [R_MOV3] = &&L_R_MOV3,
        [R_AGET_S] = &&L_R_AGET_S,
        [R_ASET_LC] = &&L_R_ASET_LC,
        [R_APUSH_LC] = &&L_R_APUSH_LC,
        [R_AGET_LU] = &&L_R_AGET_LU,
        [R_ASET_LU] = &&L_R_ASET_LU,
        [R_AGET_SU] = &&L_R_AGET_SU,
        [R_ASET_LCU] = &&L_R_ASET_LCU,
    };
#endif

    SL_DISPATCH_BEGIN

        OPC(R_LDI):    set_int(&RR(0), IMM(1)); ip += 2; NEXT();
        OPC(R_LDC_I): {
            uint64_t u = (uint64_t)ip[1] | ((uint64_t)ip[2] << 16) | ((uint64_t)ip[3] << 32) | ((uint64_t)ip[4] << 48);
            set_int(&RR(0), (int64_t)u);
            ip += 5; NEXT();
        }
        OPC(R_LDC_F): {
            uint64_t u = (uint64_t)ip[1] | ((uint64_t)ip[2] << 16) | ((uint64_t)ip[3] << 32) | ((uint64_t)ip[4] << 48);
            double d;
            memcpy(&d, &u, sizeof d);
            set_flt(&RR(0), d);
            ip += 5; NEXT();
        }
        OPC(R_LDS): if (!opf_LDS(vm, R, ip)) goto fail; ip += 2; NEXT();
        OPC(R_LDB):    set_bool(&RR(0), ip[1] != 0); ip += 2; NEXT();
        OPC(R_LDNULL): { sl_value v; v.type = SL_OBJ; v.as.o = NULL; vstore(&RR(0), v); ip += 1; NEXT(); }

        OPC(R_MOV): {
            sl_value v = vload(&RR(1));
            sl_retain(v);
            vstore(&RR(0), v);
            ip += 2; NEXT();
        }
        OPC(R_MOVS):   vstore(&RR(0), vload(&RR(1))); ip += 2; NEXT();
        OPC(R_STORE): {
            sl_value old = vload(&RR(0));
            vstore(&RR(0), vload(&RR(1)));
            moved(&RR(1));
            rel(vm, old);
            ip += 2; NEXT();
        }
        OPC(R_STORE_L): {
            sl_value v = vload(&RR(1));
            sl_retain(v);
            sl_value old = vload(&RR(0));
            vstore(&RR(0), v);
            rel(vm, old);
            ip += 2; NEXT();
        }
        OPC(R_DROP):   drop(vm, &RR(0)); ip += 1; NEXT();

        OPC(R_ADD_I): set_int(&RR(0), RR(1).as.i + RR(2).as.i); ip += 3; NEXT();
        OPC(R_SUB_I): set_int(&RR(0), RR(1).as.i - RR(2).as.i); ip += 3; NEXT();
        OPC(R_MUL_I): set_int(&RR(0), RR(1).as.i * RR(2).as.i); ip += 3; NEXT();
        OPC(R_DIV_I): {
            int64_t b = RR(2).as.i;
            if (b == 0) { vm_error(vm, "division by zero"); goto fail; }
            set_int(&RR(0), RR(1).as.i / b);
            ip += 3; NEXT();
        }
        OPC(R_REM_I): {
            int64_t b = RR(2).as.i;
            if (b == 0) { vm_error(vm, "remainder by zero"); goto fail; }
            set_int(&RR(0), RR(1).as.i % b);
            ip += 3; NEXT();
        }
        OPC(R_ADDI):  set_int(&RR(0), RR(1).as.i + IMM(2)); ip += 3; NEXT();
        OPC(R_NEG_I): set_int(&RR(0), -RR(1).as.i); ip += 2; NEXT();

        OPC(R_ADD_F): set_flt(&RR(0), RR(1).as.f + RR(2).as.f); ip += 3; NEXT();
        OPC(R_SUB_F): set_flt(&RR(0), RR(1).as.f - RR(2).as.f); ip += 3; NEXT();
        OPC(R_MUL_F): set_flt(&RR(0), RR(1).as.f * RR(2).as.f); ip += 3; NEXT();
        OPC(R_DIV_F): set_flt(&RR(0), RR(1).as.f / RR(2).as.f); ip += 3; NEXT();
        OPC(R_REM_F): set_flt(&RR(0), fmod(RR(1).as.f, RR(2).as.f)); ip += 3; NEXT();
        OPC(R_NEG_F): set_flt(&RR(0), -RR(1).as.f); ip += 2; NEXT();

#define CMP_HANDLERS(N, OPER) \
        OPC(R_##N##_I): set_bool(&RR(0), RR(1).as.i OPER RR(2).as.i); ip += 3; NEXT(); \
        OPC(R_##N##_F): set_bool(&RR(0), RR(1).as.f OPER RR(2).as.f); ip += 3; NEXT();
        CMP_HANDLERS(EQ, ==) CMP_HANDLERS(NE, !=) CMP_HANDLERS(LT, <)
        CMP_HANDLERS(LE, <=) CMP_HANDLERS(GT, >)  CMP_HANDLERS(GE, >=)
#undef CMP_HANDLERS

        OPC(R_EQ_B): set_bool(&RR(0), RR(1).as.b == RR(2).as.b); ip += 3; NEXT();
        OPC(R_NE_B): set_bool(&RR(0), RR(1).as.b != RR(2).as.b); ip += 3; NEXT();
        OPC(R_NOT):  set_bool(&RR(0), !RR(1).as.b); ip += 2; NEXT();

        OPC(R_EQ_S): want_equal = true;  goto str_compare;
        OPC(R_NE_S): want_equal = false;
        str_compare: {
            sl_value a = vload(&RR(1)), b = vload(&RR(2));
            size_t an, bn;
            const char *ap = sl_as_str(a, &an), *bp = sl_as_str(b, &bn);
            bool eq = (an == bn) && memcmp(ap, bp, an) == 0;
            drop(vm, &RR(1)); drop(vm, &RR(2));
            set_bool(&RR(0), want_equal ? eq : !eq);
            ip += 3; NEXT();
        }

        OPC(R_CONCAT): if (!opf_CONCAT(vm, R, ip)) goto fail; ip += 3; NEXT();
        OPC(R_STR_I): if (!opf_STR_I(vm, R, ip)) goto fail; ip += 2; NEXT();
        OPC(R_STR_F): if (!opf_STR_F(vm, R, ip)) goto fail; ip += 2; NEXT();
        OPC(R_STR_B): if (!opf_STR_B(vm, R, ip)) goto fail; ip += 2; NEXT();
        OPC(R_STR_OBJ): if (!opf_STR_OBJ(vm, R, ip)) goto fail; ip += 2; NEXT();

        OPC(R_JMP): ip += 1 + OFFSET(0); NEXT();
        OPC(R_JT): if (RR(0).as.b) ip += 2 + OFFSET(1); else ip += 2; NEXT();
        OPC(R_JF): if (!RR(0).as.b) ip += 2 + OFFSET(1); else ip += 2; NEXT();

        OPC(R_AJLT): {
            int64_t v = RR(0).as.i + IMM(2);
            set_int(&RR(0), v);
            if (v < RR(1).as.i) ip += 4 + OFFSET(3); else ip += 4;
            NEXT();
        }
        OPC(R_MOV2): {
            sl_value v1 = vload(&RR(1)), v2 = vload(&RR(3));
            sl_retain(v1); sl_retain(v2);
            vstore(&RR(0), v1); vstore(&RR(2), v2);
            ip += 4; NEXT();
        }
        OPC(R_MOV3): {
            sl_value v1 = vload(&RR(1)), v2 = vload(&RR(3)), v3 = vload(&RR(5));
            sl_retain(v1); sl_retain(v2); sl_retain(v3);
            vstore(&RR(0), v1); vstore(&RR(2), v2); vstore(&RR(4), v3);
            ip += 6; NEXT();
        }
        OPC(R_AGET_S): {
            const sl_value *av = &RR(1);
            int64_t iv = RR(2).as.i;
            if (av->type != SL_OBJ || !av->as.o || av->as.o->kind != SL_ARRAY) { vm_error(vm, "not an array"); goto fail; }
            sl_arr *a = (sl_arr *)av->as.o;
            int64_t i = iv < 0 ? iv + a->len : iv;
            if (i < 0 || i >= a->len) {
                vm_error(vm, "index %lld is out of bounds for an array of length %lld", (long long)iv, (long long)a->len);
                goto fail;
            }
            sl_value e = arr_get(a, i);
            sl_retain(e);
            sl_value old = vload(&RR(0));
            vstore(&RR(0), e);
            rel(vm, old);
            ip += 3; NEXT();
        }
        OPC(R_ASET_LC): {
            sl_value av = vload(&RR(0));
            int64_t iv = RR(1).as.i;
            if (av.type != SL_OBJ || !av.as.o || av.as.o->kind != SL_ARRAY) { vm_error(vm, "not an array"); goto fail; }
            sl_arr *a = (sl_arr *)av.as.o;
            int64_t i = iv < 0 ? iv + a->len : iv;
            if (i < 0 || i >= a->len) {
                vm_error(vm, "index %lld is out of bounds for an array of length %lld", (long long)iv, (long long)a->len);
                goto fail;
            }
            sl_value nv = vload(&RR(2));
            if (a->ek != AK_GEN && a->ek != ak_of(nv) && !arr_generify(vm, a)) goto fail;
            sl_retain(nv);
            sl_value old = arr_get(a, i);
            arr_put(a, i, nv);
            rel(vm, old);
            ip += 3; NEXT();
        }
        OPC(R_APUSH_LC): {
            sl_value av = vload(&RR(0));
            if (av.type != SL_OBJ || !av.as.o || av.as.o->kind != SL_ARRAY) { vm_error(vm, "not an array"); goto fail; }
            sl_value val = vload(&RR(1));
            sl_retain(val);
            if (!array_push(vm, (sl_arr *)av.as.o, val)) goto fail;
            ip += 2; NEXT();
        }
        // Unchecked accesses: the optimizer proved 0 <= index < length. With
        // SL_OPT_ASSERT they check anyway and stop if the proof was wrong.
#ifdef SL_OPT_ASSERT
#define OPT_PROVEN(a, i) do { if (!(a) || (i) < 0 || (i) >= (a)->len) { fprintf(stderr, "optimizer: a bound it proved does not hold\n"); abort(); } } while (0)
#else
#define OPT_PROVEN(a, i) ((void)0)
#endif
        OPC(R_AGET_LU): {
            sl_arr *a = (sl_arr *)RR(1).as.o;
            int64_t i = RR(2).as.i;
            OPT_PROVEN(a, i);
            sl_value e = arr_get(a, i);
            sl_retain(e);
            vstore(&RR(0), e);
            ip += 3; NEXT();
        }
        OPC(R_AGET_SU): {
            sl_arr *a = (sl_arr *)RR(1).as.o;
            int64_t i = RR(2).as.i;
            OPT_PROVEN(a, i);
            sl_value e = arr_get(a, i);
            sl_retain(e);
            sl_value old = vload(&RR(0));
            vstore(&RR(0), e);
            rel(vm, old);
            ip += 3; NEXT();
        }
        OPC(R_ASET_LU): {
            sl_arr *a = (sl_arr *)RR(0).as.o;
            int64_t i = RR(1).as.i;
            OPT_PROVEN(a, i);
            sl_value nv = vload(&RR(2));
            if (a->ek != AK_GEN && a->ek != ak_of(nv) && !arr_generify(vm, a)) goto fail;
            sl_value old = arr_get(a, i);
            arr_put(a, i, nv);
            moved(&RR(2));
            rel(vm, old);
            ip += 3; NEXT();
        }
        OPC(R_ASET_LCU): {
            sl_arr *a = (sl_arr *)RR(0).as.o;
            int64_t i = RR(1).as.i;
            OPT_PROVEN(a, i);
            sl_value nv = vload(&RR(2));
            if (a->ek != AK_GEN && a->ek != ak_of(nv) && !arr_generify(vm, a)) goto fail;
            sl_retain(nv);
            sl_value old = arr_get(a, i);
            arr_put(a, i, nv);
            rel(vm, old);
            ip += 3; NEXT();
        }
        OPC(R_JLTLEN): {
            sl_value av = vload(&RR(1));
            int64_t n;
            if (av.type == SL_OBJ && av.as.o && av.as.o->kind == SL_ARRAY) n = ((sl_arr *)av.as.o)->len;
            else if (av.type == SL_OBJ && av.as.o && av.as.o->kind == SL_STR) n = utf8_length(av);
            else if (av.type == SL_OBJ && av.as.o && av.as.o->kind == SL_MAP) n = sl_map_len(av);
            else n = sl_array_len(av);
            if (RR(0).as.i < n) ip += 3 + OFFSET(2); else ip += 3;
            NEXT();
        }
        OPC(R_AJLTI): {
            int64_t v = RR(0).as.i + IMM(2);
            set_int(&RR(0), v);
            if (v < (int64_t)IMM(1)) ip += 4 + OFFSET(3); else ip += 4;
            NEXT();
        }

#define JUMP_HANDLERS(N, OPER) \
        OPC(R_JC_##N##_I): if (RR(0).as.i OPER RR(1).as.i) ip += 3 + OFFSET(2); else ip += 3; NEXT(); \
        OPC(R_JC_##N##_F): if (RR(0).as.f OPER RR(1).as.f) ip += 3 + OFFSET(2); else ip += 3; NEXT(); \
        OPC(R_JN_##N##_F): if (!(RR(0).as.f OPER RR(1).as.f)) ip += 3 + OFFSET(2); else ip += 3; NEXT(); \
        OPC(R_JCI_##N):    if (RR(0).as.i OPER (int64_t)IMM(1)) ip += 3 + OFFSET(2); else ip += 3; NEXT();
        JUMP_HANDLERS(EQ, ==) JUMP_HANDLERS(NE, !=) JUMP_HANDLERS(LT, <)
        JUMP_HANDLERS(LE, <=) JUMP_HANDLERS(GT, >)  JUMP_HANDLERS(GE, >=)
#undef JUMP_HANDLERS

        OPC(R_CALL): {
            uint16_t id = ip[0];
            sl_func *callee = &vm->funcs[id];
            if (fr >= frames_last) {
                vm_error(vm, "call depth exceeded %d (infinite recursion?)", SL_FRAMES_MAX);
                goto fail;
            }
            sl_value *nR = R + ip[1];
            if (nR + callee->nregs > stack_end) { vm_error(vm, "stack overflow"); goto fail; }
            fr->ip = ip + 2;
            fr++;
            fr->fn = callee; fr->base = nR;
            fn = callee; R = nR;
            ip = callee->rcode;
            NEXT();
        }

        OPC(R_NATIVE): {
            uint16_t idx = ip[0];
            int slot = vm->native_bind[idx];
            if (slot < 0) {
                vm_error(vm, "`%s` is declared extern but the host did not register it",
                         vm->native_names[idx]);
                goto fail;
            }
            sl_value *argv = &RR(1);
            uint16_t argc = ip[2];
            bool nres = ip[3] != 0;
            sl_value result = vm->registry[slot].fn(vm, argc, argv);
            for (uint16_t i = 0; i < argc; i++) drop(vm, &argv[i]);
            if (vm->failed) { rel(vm, result); goto fail; }
            if (nres) vstore(&argv[0], result); else rel(vm, result);
            ip += 4; NEXT();
        }

        OPC(R_RET): {
            sl_value result = vload(&RR(0));
            moved(&RR(0));
            for (sl_value *p = R, *end = R + fn->nregs; p < end; p++) drop(vm, p);
            if (fr == frames_first) { vm->ret = result; return SL_OK; }
            vstore(&R[0], result);
            fr--;
            fn = fr->fn; R = fr->base; ip = fr->ip;
            NEXT();
        }
        OPC(R_RETV): {
            for (sl_value *p = R, *end = R + fn->nregs; p < end; p++) drop(vm, p);
            if (fr == frames_first) return SL_OK;
            R[0].type = SL_VOID;
            fr--;
            fn = fr->fn; R = fr->base; ip = fr->ip;
            NEXT();
        }
        OPC(R_HALT): goto done;

        OPC(R_NEWARR): if (!opf_NEWARR(vm, R, ip)) goto fail; ip += 2; NEXT();

        OPC(R_AGET): {
            sl_value av = vload(&RR(1));
            int64_t iv = RR(2).as.i;
            if (av.type != SL_OBJ || !av.as.o || av.as.o->kind != SL_ARRAY) { vm_error(vm, "not an array"); goto fail; }
            sl_arr *a = (sl_arr *)av.as.o;
            int64_t i = iv < 0 ? iv + a->len : iv;
            if (i < 0 || i >= a->len) {
                vm_error(vm, "index %lld is out of bounds for an array of length %lld", (long long)iv, (long long)a->len);
                goto fail;
            }
            sl_value e = arr_get(a, i);
            sl_retain(e);
            RR(1).type = SL_VOID;
            rel(vm, av);
            vstore(&RR(0), e);
            ip += 3; NEXT();
        }
        OPC(R_AGET_L): {
            const sl_value *av = &RR(1);
            int64_t iv = RR(2).as.i;
            if (av->type != SL_OBJ || !av->as.o || av->as.o->kind != SL_ARRAY) { vm_error(vm, "not an array"); goto fail; }
            sl_arr *a = (sl_arr *)av->as.o;
            int64_t i = iv < 0 ? iv + a->len : iv;
            if (i < 0 || i >= a->len) {
                vm_error(vm, "index %lld is out of bounds for an array of length %lld", (long long)iv, (long long)a->len);
                goto fail;
            }
            sl_value e = arr_get(a, i);
            sl_retain(e);
            vstore(&RR(0), e);
            ip += 3; NEXT();
        }
        OPC(R_ASET): array_owned = true;  goto aset_common;
        OPC(R_ASET_L): array_owned = false;
        aset_common: {
            sl_value av = vload(&RR(0));
            int64_t iv = RR(1).as.i;
            if (av.type != SL_OBJ || !av.as.o || av.as.o->kind != SL_ARRAY) { vm_error(vm, "not an array"); goto fail; }
            sl_arr *a = (sl_arr *)av.as.o;
            int64_t i = iv < 0 ? iv + a->len : iv;
            if (i < 0 || i >= a->len) {
                vm_error(vm, "index %lld is out of bounds for an array of length %lld", (long long)iv, (long long)a->len);
                goto fail;
            }
            sl_value nv = vload(&RR(2));
            if (a->ek != AK_GEN && a->ek != ak_of(nv) && !arr_generify(vm, a)) goto fail;
            sl_value old = arr_get(a, i);
            arr_put(a, i, nv);
            moved(&RR(2));
            rel(vm, old);
            if (array_owned) drop(vm, &RR(0));
            ip += 3; NEXT();
        }
        OPC(R_ALEN): array_owned = true;  goto alen_common;
        OPC(R_ALEN_L): array_owned = false;
        alen_common: {
            sl_value av = vload(&RR(1));
            int64_t n;
            if (av.type == SL_OBJ && av.as.o && av.as.o->kind == SL_STR) n = utf8_length(av);
            else if (av.type == SL_OBJ && av.as.o && av.as.o->kind == SL_MAP) n = sl_map_len(av);
            else n = sl_array_len(av);
            if (array_owned) drop(vm, &RR(1));
            set_int(&RR(0), n);
            ip += 2; NEXT();
        }
        OPC(R_APUSH): array_owned = true;  goto apush_common;
        OPC(R_APUSH_L): array_owned = false;
        apush_common: {
            sl_value av = vload(&RR(0));
            if (av.type != SL_OBJ || !av.as.o || av.as.o->kind != SL_ARRAY) { vm_error(vm, "not an array"); goto fail; }
            sl_value val = vload(&RR(1));
            moved(&RR(1));
            if (!array_push(vm, (sl_arr *)av.as.o, val)) goto fail;
            if (array_owned) drop(vm, &RR(0));
            ip += 2; NEXT();
        }
        OPC(R_SLICE): if (!opf_SLICE(vm, R, ip)) goto fail; ip += 4; NEXT();

        OPC(R_NEWSTRUCT): if (!opf_NEWSTRUCT(vm, R, ip)) goto fail; ip += 3; NEXT();
        OPC(R_FGET): array_owned = true;  goto fget_common;
        OPC(R_FGET_L): array_owned = false;
        fget_common: {
            const sl_value *sv = &RR(1);
            uint16_t idx = ip[2];
            if (sv->type != SL_OBJ || !sv->as.o || sv->as.o->kind != SL_STRUCT) { vm_error(vm, "not a struct"); goto fail; }
            sl_struct *s = (sl_struct *)sv->as.o;
            if (idx >= s->nfields) { vm_error(vm, "bad field index"); goto fail; }
            sl_value f = vload(&s->fields[idx]);
            sl_retain(f);
            if (array_owned) drop(vm, &RR(1));
            vstore(&RR(0), f);
            ip += 3; NEXT();
        }
        OPC(R_FSET): array_owned = true;  goto fset_common;
        OPC(R_FSET_L): array_owned = false;
        fset_common: {
            const sl_value *sv = &RR(0);
            uint16_t idx = ip[1];
            if (sv->type != SL_OBJ || !sv->as.o || sv->as.o->kind != SL_STRUCT) { vm_error(vm, "not a struct"); goto fail; }
            sl_struct *s = (sl_struct *)sv->as.o;
            if (idx >= s->nfields) { vm_error(vm, "bad field index"); goto fail; }
            sl_value old = vload(&s->fields[idx]);
            vstore(&s->fields[idx], vload(&RR(2)));
            moved(&RR(2));
            rel(vm, old);
            if (array_owned) drop(vm, &RR(0));
            ip += 3; NEXT();
        }

        OPC(R_NEWVAR): if (!opf_NEWVAR(vm, R, ip)) goto fail; ip += 3; NEXT();
        OPC(R_VTAG): if (!opf_VTAG(vm, R, ip)) goto fail; ip += 2; NEXT();
        OPC(R_VGET): if (!opf_VGET(vm, R, ip)) goto fail; ip += 3; NEXT();

        OPC(R_LOADG): {
            sl_value v = vload(&vm->globals[ip[1]]);
            sl_retain(v);
            vstore(&RR(0), v);
            ip += 2; NEXT();
        }
        OPC(R_STOREG): {
            sl_value old = vload(&vm->globals[ip[0]]);
            vstore(&vm->globals[ip[0]], vload(&RR(1)));
            moved(&RR(1));
            rel(vm, old);
            ip += 2; NEXT();
        }
        OPC(R_COPY): if (!opf_COPY(vm, R, ip)) goto fail; ip += 3; NEXT();

        OPC(R_NEWMAP): if (!opf_NEWMAP(vm, R, ip)) goto fail; ip += 2; NEXT();
        OPC(R_MGET): if (!opf_MGET(vm, R, ip)) goto fail; ip += 3; NEXT();
        OPC(R_MSET): if (!opf_MSET(vm, R, ip)) goto fail; ip += 3; NEXT();

#ifdef SL_THREADED
        L_unknown:
#else
        default:
#endif
            vm_error(vm, "unknown opcode %u", ip[-1]);
            goto fail;

    SL_DISPATCH_END

done:
    release_stack(vm, R + fn->nregs);
    vm->nframes = 0;
    return SL_OK;

fail:
    // Unwind every frame so a fault does not leak the whole stack.
    release_stack(vm, R + fn->nregs);
    vm->nframes = 0;
    return SL_ERR_RUNTIME;
}

sl_result sl_run(sl_vm *vm) {
    if (vm->running) return SL_ERR_CALL;       // from inside a native: refused without disturbing the run in progress
    vm->running = true;
    sl_result r = run_function(vm, vm->entry, 0, NULL);
    vm->running = false;
    rel(vm, vm->ret);
    vm->ret = sl_void();
    if (r == SL_OK) vm->ready = true;
    return r;
}

int sl_function_count(const sl_vm *vm) { return vm->nfuncs; }

const char *sl_function_name(const sl_vm *vm, int index) {
    return index >= 0 && index < vm->nfuncs ? vm->funcs[index].name : NULL;
}

int sl_function_arity(const sl_vm *vm, int index) {
    return index >= 0 && index < vm->nfuncs ? vm->funcs[index].arity : -1;
}

int sl_find_function(const sl_vm *vm, const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < vm->nfuncs; i++)
        if (strcmp(vm->funcs[i].name, name) == 0) return i;
    if (strchr(name, '.')) return -1;
    // A bare name: the one function that is called that in some module.
    size_t n = strlen(name);
    int found = -1;
    for (int i = 0; i < vm->nfuncs; i++) {
        const char *f = vm->funcs[i].name;
        size_t fl = strlen(f);
        if (fl > n + 1 && f[fl - n - 1] == '.' && memcmp(f + fl - n, name, n) == 0) {
            if (found >= 0) return -2;
            found = i;
        }
    }
    return found;
}

sl_result sl_call_index(sl_vm *vm, int index, int argc, const sl_value *argv, sl_value *result) {
    if (result) *result = sl_void();
    if (vm->running) return SL_ERR_CALL;       // from inside a native: refused without disturbing the run in progress
    if (vm->nfuncs == 0) { vm_error(vm, "nothing loaded"); return SL_ERR_CALL; }
    if (index < 0 || index >= vm->nfuncs || index == vm->entry) { vm_error(vm, "no such function"); return SL_ERR_CALL; }
    if (!vm->ready) { vm_error(vm, "call sl_run first: it initialises the script's globals"); return SL_ERR_CALL; }
    const sl_func *fn = &vm->funcs[index];
    if (argc < 0 || argc != fn->arity || (argc > 0 && !argv)) {
        vm_error(vm, "`%s` takes %d argument(s) but %d were given", fn->name, fn->arity, argc);
        return SL_ERR_CALL;
    }
    vm->running = true;
    sl_result r = run_function(vm, (uint16_t)index, argc, argv);
    vm->running = false;
    if (r == SL_OK && result) { *result = vm->ret; vm->ret = sl_void(); }
    else { rel(vm, vm->ret); vm->ret = sl_void(); }
    return r;
}

sl_result sl_call(sl_vm *vm, const char *name, int argc, const sl_value *argv, sl_value *result) {
    if (result) *result = sl_void();
    if (vm->running) return SL_ERR_CALL;
    int index = sl_find_function(vm, name);
    if (index == -1) { vm_error(vm, "no function named `%s`", name ? name : "(null)"); return SL_ERR_CALL; }
    if (index == -2) { vm_error(vm, "`%s` is ambiguous; write it as module.%s", name, name); return SL_ERR_CALL; }
    return sl_call_index(vm, index, argc, argv, result);
}

#ifdef SL_AOT_EMITTER
#include "aot_emit.inc"
#endif
