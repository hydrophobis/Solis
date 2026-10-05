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
    SL_VARIANT
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
static inline sl_value sl_bool(bool v)    { sl_value x; x.type = SL_BOOL;  x.as.b = v; return x; }
static inline sl_value sl_void(void)      { sl_value x; x.type = SL_VOID;  x.as.i = 0; return x; }

typedef struct sl_vm sl_vm;

// Arguments are borrowed; retain anything you keep. The return is owned by
// the VM.
typedef sl_value (*sl_native_fn)(sl_vm *vm, int argc, sl_value *argv);

typedef enum {
    SL_OK = 0,
    SL_ERR_BADFILE,     // not Solis bytecode, or an unknown version
    SL_ERR_RUNTIME,     // the script faulted
    SL_ERR_NOMEM
} sl_result;

sl_vm      *sl_new(void);
void        sl_free(sl_vm *vm);

bool        sl_register(sl_vm *vm, const char *name, sl_native_fn fn);
sl_result   sl_load(sl_vm *vm, const uint8_t *bytes, size_t len);
sl_result   sl_run(sl_vm *vm);

const char *sl_error(const sl_vm *vm);
void        sl_fail(sl_vm *vm, const char *msg);

// Reference counting. Call sl_retain on anything you keep past a native.
void        sl_retain(sl_value v);
void        sl_release(sl_vm *vm, sl_value v);

// Strings. sl_str copies `n` bytes; the result is owned by the caller.
sl_value    sl_str(sl_vm *vm, const char *bytes, size_t n);
sl_value    sl_cstr(sl_vm *vm, const char *z);
const char *sl_as_str(sl_value v, size_t *len_out);

// sl_array_push takes ownership of `item`; don't release it afterwards.
sl_value    sl_array(sl_vm *vm, int64_t count);
int64_t     sl_array_len(sl_value v);
sl_value    sl_array_get(sl_value v, int64_t i);
bool        sl_array_push(sl_vm *vm, sl_value arr, sl_value item);

// Render any value the way `print` does. Returns a VM-owned string value.
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
