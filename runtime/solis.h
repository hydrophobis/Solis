// Solis runtime API
//
//     sl_vm *vm = sl_new();
//     sl_register(vm, "nowMs", my_now_ms);
//     sl_load(vm, bytes, len);
//     sl_run(vm);
//     sl_free(vm);

#ifndef SOLIS_H
#define SOLIS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SL_VOID = 0,
    SL_INT,
    SL_FLOAT,
    SL_BOOL,
    SL_OBJ          // a reference-counted heap value
} sl_type;

typedef enum {
    SL_STR = 0,
    SL_ARRAY,
    SL_STRUCT,
    SL_VARIANT,
    SL_MAP
} sl_obj_kind;

typedef struct sl_obj sl_obj;

typedef struct {
    sl_type type;
    union {
        int64_t i;
        double  f;
        bool    b;
        sl_obj *o;
    } as;
} sl_value;

// Header shared by every heap object.
struct sl_obj {
    uint32_t    rc;
    sl_obj_kind kind;
};

static inline sl_value sl_int(int64_t v)  { sl_value x; x.type = SL_INT;   x.as.i = v; return x; }
static inline sl_value sl_float(double v) { sl_value x; x.type = SL_FLOAT; x.as.f = v; return x; }
// Writes the whole payload word so a later 8-byte read of it forwards from this store.
static inline sl_value sl_bool(bool v)    { sl_value x; x.type = SL_BOOL;  x.as.i = v; return x; }
static inline sl_value sl_void(void)      { sl_value x; x.type = SL_VOID;  x.as.i = 0; return x; }

typedef struct sl_vm sl_vm;

// Arguments are borrowed; retain anything you keep. The return is owned by
// the VM.
typedef sl_value (*sl_native_fn)(sl_vm *vm, int argc, sl_value *argv);

typedef enum {
    SL_OK = 0,
    SL_ERR_BADFILE,     // not Solis bytecode, or an unknown version
    SL_ERR_RUNTIME,     // the script faulted
    SL_ERR_NOMEM,
    SL_ERR_LINK,        // unregistered native or signature mismatch
    SL_ERR_CALL         // sl_call could not start: no such function, wrong argument count, or sl_run has not finished
} sl_result;

sl_vm      *sl_new(void);
void        sl_free(sl_vm *vm);

// `sig` is spelled like "(str,int)->str", or "()->()" for no arguments and no
// result. sl_load checks it against the script's extern; sl_register is unchecked.
bool        sl_register_sig(sl_vm *vm, const char *name, const char *sig, sl_native_fn fn);
bool        sl_register(sl_vm *vm, const char *name, sl_native_fn fn);

// Source of the `host` module for natives registered with sl_register_sig.
// Natives without a signature, or whose signature names a user type, are
// omitted. The result is malloc'd.
char       *sl_declarations(const sl_vm *vm);
sl_result   sl_load(sl_vm *vm, const uint8_t *bytes, size_t len);
sl_result   sl_run(sl_vm *vm);

// Calling one function of a loaded script by name, after sl_run has finished
// (sl_run initialises the script's globals and runs main, which is where a host
// usually lets a script register itself). The globals keep their values between
// calls, so a script can hold state.
//
// Names are written `module.function`, as in the image. A bare name works when
// exactly one module has a function of that name; sl_find_function returns -2
// for an ambiguous one. The image's synthetic entry function cannot be called.
//
// Arguments are borrowed. Their types are not checked at run time, so pass what
// the function declares. On success *result (if given) is the function's return
// value, owned by the caller: sl_release it. A function that returns nothing gives
// sl_void(). Errors read like sl_run's: sl_error(vm). Neither sl_run nor sl_call
// may be called from inside a native; they return SL_ERR_CALL and leave the run in progress alone.
sl_result   sl_call(sl_vm *vm, const char *name, int argc, const sl_value *argv, sl_value *result);
sl_result   sl_call_index(sl_vm *vm, int index, int argc, const sl_value *argv, sl_value *result);

// The functions in the loaded image, imported modules' included. Names are
// `module.function`; the first entry is not necessarily a user function.
int         sl_function_count(const sl_vm *vm);
const char *sl_function_name(const sl_vm *vm, int index);   // NULL out of range; owned by the VM
int         sl_function_arity(const sl_vm *vm, int index);  // -1 out of range
int         sl_find_function(const sl_vm *vm, const char *name);  // index, -1 if none, -2 if ambiguous

const char *sl_error(const sl_vm *vm);
void        sl_fail(sl_vm *vm, const char *msg);

// Reference counting. Call sl_retain on anything you keep past a native.
static inline void sl_retain(sl_value v) {
    if (v.type == SL_OBJ && v.as.o) v.as.o->rc++;
}
void        sl_release(sl_vm *vm, sl_value v);

// Strings. sl_str copies `n` bytes; the result is owned by the caller.
sl_value    sl_str(sl_vm *vm, const char *bytes, size_t n);
sl_value    sl_cstr(sl_vm *vm, const char *z);
const char *sl_as_str(sl_value v, size_t *len_out);

// sl_array_push/set/insert take ownership of `item`; don't release it
// afterwards. sl_array_get/pop/remove hand ownership to the caller instead.
sl_value    sl_array(sl_vm *vm, int64_t count);
int64_t     sl_array_len(sl_value v);
sl_value    sl_array_get(sl_value v, int64_t i);
bool        sl_array_push(sl_vm *vm, sl_value arr, sl_value item);
bool        sl_array_set(sl_vm *vm, sl_value arr, int64_t i, sl_value item);
bool        sl_array_pop(sl_vm *vm, sl_value arr, sl_value *out);
bool        sl_array_remove(sl_vm *vm, sl_value arr, int64_t i, sl_value *out);
bool        sl_array_insert(sl_vm *vm, sl_value arr, int64_t i, sl_value item);

// sl_map_set takes ownership of `key`/`val`. sl_map_get hands the stored
// value back borrowed; sl_map_remove hands both back owned. Lookup is a
// hash lookup (a scan while the map is small); pairs keep insertion order.
sl_value    sl_map(sl_vm *vm, int64_t cap);
int64_t     sl_map_len(sl_value v);
bool        sl_map_get(sl_value m, sl_value key, sl_value *out);
bool        sl_map_set(sl_vm *vm, sl_value m, sl_value key, sl_value val);
bool        sl_map_remove(sl_vm *vm, sl_value m, sl_value key, sl_value *out);
bool        sl_map_pair_at(sl_value m, int64_t i, sl_value *key_out, sl_value *val_out);

// the caller owns the result and must sl_release it
sl_value    sl_to_string(sl_vm *vm, sl_value v);

// How many heap objects are currently alive; for leak checks in tests.
int64_t     sl_live_objects(const sl_vm *vm);

// How many more natives this VM will accept. 
int         sl_natives_free(const sl_vm *vm);

// Backs `math` and `strings`. If you do not call this then math and strings will be unaccessable
void        sl_open_std(sl_vm *vm);

// Backs `io`, `fs`, `os`, `time` and `rand`
// `argc`/`argv` become os.args(); pass 0 and NULL if a script has no
// arguments of its own. They are borrowed, not copied, so they must outlive
// the VM.
void        sl_open_os(sl_vm *vm, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif // SOLIS_H
