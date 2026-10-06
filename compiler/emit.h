// Bytecode: the contract between the compiler and runtime/solis.c. Opcode
// numbers can't be reordered without changing both.
//
// File format, all integers little-endian:
//
//   magic     "SLBC"
//   version   u16
//   nconsts   u16
//   consts[]  tag u8 then payload: 0 = i64, 1 = f64,
//             2 = u32 length followed by that many UTF-8 bytes
//   nnatives  u16
//   natives[] u16 length then that many bytes
//   nglobals  u16
//   nfuncs    u16
//   funcs[]   u16 name length, name, u8 arity, u16 nslots,
//             u32 code length, code
//   entry     u16

#ifndef SOLIS_EMIT_H
#define SOLIS_EMIT_H

#include "common.h"

#define SL_MAGIC   "SLBC"
#define SL_VERSION 3

typedef enum {
    // Push a small integer encoded in the instruction itself.
    OP_CONST_I8 = 0,
    // Push constants[u16].
    OP_CONST = 1,
    OP_TRUE = 2,
    OP_FALSE = 3,
    OP_POP = 4,

    // Locals, addressed by frame slot.
    OP_LOAD = 5,
    OP_STORE = 6,

    OP_DUP = 7,
    OP_CONST_STR = 8,
    OP_CONCAT = 9,

    OP_ADD_I = 10, OP_SUB_I = 11, OP_MUL_I = 12, OP_DIV_I = 13,
    OP_REM_I = 14, OP_NEG_I = 15,

    // Convert to string, for interpolation. Typed, so no tag inspection.
    OP_STR_I = 16, OP_STR_F = 17, OP_STR_B = 18,
    // The generic stringify, for values whose shape is only known at runtime.
    // The compiler emits it only for reference types.
    OP_STR_OBJ = 19,

    OP_ADD_F = 20, OP_SUB_F = 21, OP_MUL_F = 22, OP_DIV_F = 23,
    OP_REM_F = 24, OP_NEG_F = 25,

    OP_EQ_I = 30, OP_NE_I = 31, OP_LT_I = 32,
    OP_LE_I = 33, OP_GT_I = 34, OP_GE_I = 35,

    OP_EQ_F = 40, OP_NE_F = 41, OP_LT_F = 42,
    OP_LE_F = 43, OP_GT_F = 44, OP_GE_F = 45,

    OP_EQ_S = 46, OP_NE_S = 47,

    OP_EQ_B = 50, OP_NE_B = 51, OP_NOT = 52,

    // Signed 16-bit relative jumps, measured from the end of the operand.
    OP_JUMP = 60, OP_JUMP_IF_FALSE = 61,

    OP_CALL = 70,       // u16 function, u8 argc
    OP_NATIVE = 71,     // u16 index, u8 argc
    OP_RET = 72,
    OP_RET_VOID = 73,

    OP_HALT = 90,

    OP_NEW_ARRAY = 100, // u16 count, pops that many values
    OP_AGET = 101, OP_ASET = 102, OP_ALEN = 103, OP_APUSH = 104,

    OP_NEW_STRUCT = 110,// u16 nfields, pops that many, in field order
    OP_FGET = 111, OP_FSET = 112,

    OP_NEW_VARIANT = 120, // u16 tag, u8 npayload
    OP_VTAG = 121, OP_VGET = 122,

    OP_LOADG = 130, OP_STOREG = 131,

    OP_DUP2 = 140,
    OP_COPY = 141,

    // Push a null reference: SL_OBJ with a NULL payload.
    OP_NULL = 142,

    OP_INCR_I = 150,   // u8 slot, i8 imm
    OP_MOVE = 151,     // u8 dst, u8 src
    OP_ADD_RR_I = 152  // u8 dst, u8 src1, u8 src2
} opcode;

typedef enum { C_INT, C_FLOAT, C_STR } const_kind;

typedef struct {
    const_kind kind;
    int64_t    i;
    double     f;
    const char *s;
    size_t      slen;
} constant;

typedef struct {
    const char *name;
    uint8_t     arity;
    uint16_t    nslots;
    uint8_t    *code;
    size_t      code_len;
} bc_func;

typedef struct {
    arena *a;
    VEC(constant)     consts;
    VEC(const char *) natives;
    VEC(bc_func)      funcs;
    uint16_t          entry;
    uint16_t          nglobals;
    // Interning, so repeated literals share one constant slot. The maps hold
    // index+1 so that zero can mean "absent".
    map int_map;
    map str_map;
    map native_map;
} bc_program;

void bc_init(bc_program *p, arena *a);

uint16_t bc_add_int(bc_program *p, int64_t v);
uint16_t bc_add_float(bc_program *p, double v);
uint16_t bc_add_str(bc_program *p, const char *s, size_t n);
uint16_t bc_add_native(bc_program *p, const char *name);

// Serialize to a freshly allocated buffer.
uint8_t *bc_serialize(bc_program *p, size_t *len_out);

// A growable instruction buffer with patchable jumps.
typedef struct {
    arena   *a;
    uint8_t *code;
    int      len;
    int      cap;
} emitter;

void em_init(emitter *e, arena *a);
void em_op(emitter *e, opcode op);
void em_u8(emitter *e, uint8_t v);
void em_u16(emitter *e, uint16_t v);
void em_const_int(emitter *e, int64_t v, bc_program *p);

// Emit a jump with a placeholder offset; returns where to patch.
int  em_jump(emitter *e, opcode op);
// Point a previously emitted jump at the current position.
void em_patch(emitter *e, int at);
// Point a previously emitted jump at `target`, which may be behind it.
void em_patch_to(emitter *e, int at, int target);
// A backwards jump to `target`, for loops.
void em_jump_back(emitter *e, opcode op, int target);
int  em_here(const emitter *e);

#endif // SOLIS_EMIT_H
