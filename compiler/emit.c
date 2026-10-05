#include "emit.h"

#include <stdio.h>

void bc_init(bc_program *p, arena *a) {
    memset(p, 0, sizeof *p);
    p->a = a;
    map_init(&p->int_map, a);
    map_init(&p->str_map, a);
    map_init(&p->native_map, a);
}

// The interning maps store index+1, so a zero lookup means "absent".
#define SLOT(x) ((void *)(uintptr_t)((x) + 1))
#define UNSLOT(v) ((uint16_t)((uintptr_t)(v) - 1))

uint16_t bc_add_int(bc_program *p, int64_t v) {
    const char *key = arena_printf(p->a, "%lld", (long long)v);
    void *hit = map_get(&p->int_map, key);
    if (hit) return UNSLOT(hit);

    uint16_t i = (uint16_t)p->consts.len;
    constant c;
    memset(&c, 0, sizeof c);
    c.kind = C_INT;
    c.i = v;
    vec_push(p->a, &p->consts, c);
    map_put(&p->int_map, key, SLOT(i));
    return i;
}

uint16_t bc_add_float(bc_program *p, double v) {
    // Bit-pattern comparison, so NaN and -0.0 behave.
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    for (int i = 0; i < p->consts.len; i++) {
        if (p->consts.at[i].kind != C_FLOAT) continue;
        uint64_t other;
        memcpy(&other, &p->consts.at[i].f, sizeof other);
        if (other == bits) return (uint16_t)i;
    }
    uint16_t i = (uint16_t)p->consts.len;
    constant c;
    memset(&c, 0, sizeof c);
    c.kind = C_FLOAT;
    c.f = v;
    vec_push(p->a, &p->consts, c);
    return i;
}

uint16_t bc_add_str(bc_program *p, const char *s, size_t n) {
    // Length is in the key, so embedded NULs don't collide.
    const char *key = arena_printf(p->a, "%zu:%.*s", n, (int)n, s);
    void *hit = map_get(&p->str_map, key);
    if (hit) return UNSLOT(hit);

    uint16_t i = (uint16_t)p->consts.len;
    constant c;
    memset(&c, 0, sizeof c);
    c.kind = C_STR;
    c.s = arena_strndup(p->a, s, n);
    c.slen = n;
    vec_push(p->a, &p->consts, c);
    map_put(&p->str_map, key, SLOT(i));
    return i;
}

uint16_t bc_add_native(bc_program *p, const char *name) {
    void *hit = map_get(&p->native_map, name);
    if (hit) return UNSLOT(hit);

    uint16_t i = (uint16_t)p->natives.len;
    vec_push(p->a, &p->natives, name);
    map_put(&p->native_map, name, SLOT(i));
    return i;
}

typedef struct {
    uint8_t *at;
    size_t   len;
    size_t   cap;
} obuf;

static void ob_push(obuf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap < 256 ? 256 : b->cap;
        while (b->len + n > cap) cap *= 2;
        uint8_t *out = (uint8_t *)realloc(b->at, cap);
        if (!out) { fprintf(stderr, "solis: out of memory\n"); exit(70); }
        b->at = out;
        b->cap = cap;
    }
    memcpy(b->at + b->len, p, n);
    b->len += n;
}

static void ob_u8(obuf *b, uint8_t v) { ob_push(b, &v, 1); }

static void ob_u16(obuf *b, uint16_t v) {
    uint8_t x[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
    ob_push(b, x, 2);
}

static void ob_u32(obuf *b, uint32_t v) {
    uint8_t x[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                     (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
    ob_push(b, x, 4);
}

static void ob_i64(obuf *b, int64_t v) {
    uint64_t u = (uint64_t)v;
    uint8_t x[8];
    for (int i = 0; i < 8; i++) x[i] = (uint8_t)((u >> (8 * i)) & 0xFF);
    ob_push(b, x, 8);
}

static void ob_f64(obuf *b, double v) {
    uint64_t u;
    memcpy(&u, &v, sizeof u);
    uint8_t x[8];
    for (int i = 0; i < 8; i++) x[i] = (uint8_t)((u >> (8 * i)) & 0xFF);
    ob_push(b, x, 8);
}

uint8_t *bc_serialize(bc_program *p, size_t *len_out) {
    obuf b;
    memset(&b, 0, sizeof b);

    ob_push(&b, SL_MAGIC, 4);
    ob_u16(&b, SL_VERSION);

    ob_u16(&b, (uint16_t)p->consts.len);
    for (int i = 0; i < p->consts.len; i++) {
        constant *c = &p->consts.at[i];
        switch (c->kind) {
            case C_INT: ob_u8(&b, 0); ob_i64(&b, c->i); break;
            case C_FLOAT: ob_u8(&b, 1); ob_f64(&b, c->f); break;
            case C_STR:
                ob_u8(&b, 2);
                ob_u32(&b, (uint32_t)c->slen);
                ob_push(&b, c->s, c->slen);
                break;
        }
    }

    ob_u16(&b, (uint16_t)p->natives.len);
    for (int i = 0; i < p->natives.len; i++) {
        size_t n = strlen(p->natives.at[i]);
        ob_u16(&b, (uint16_t)n);
        ob_push(&b, p->natives.at[i], n);
    }

    ob_u16(&b, p->nglobals);

    ob_u16(&b, (uint16_t)p->funcs.len);
    for (int i = 0; i < p->funcs.len; i++) {
        bc_func *f = &p->funcs.at[i];
        size_t n = strlen(f->name);
        ob_u16(&b, (uint16_t)n);
        ob_push(&b, f->name, n);
        ob_u8(&b, f->arity);
        ob_u16(&b, f->nslots);
        ob_u32(&b, (uint32_t)f->code_len);
        if (f->code_len) ob_push(&b, f->code, f->code_len);
    }

    ob_u16(&b, p->entry);

    *len_out = b.len;
    return b.at;
}

void em_init(emitter *e, arena *a) {
    memset(e, 0, sizeof *e);
    e->a = a;
}

static void em_byte(emitter *e, uint8_t v) {
    if (e->len == e->cap) {
        int cap = e->cap < 64 ? 64 : e->cap * 2;
        uint8_t *out = (uint8_t *)arena_alloc(e->a, (size_t)cap);
        if (e->len) memcpy(out, e->code, (size_t)e->len);
        e->code = out;
        e->cap = cap;
    }
    e->code[e->len++] = v;
}

void em_op(emitter *e, opcode op) { em_byte(e, (uint8_t)op); }
void em_u8(emitter *e, uint8_t v) { em_byte(e, v); }

void em_u16(emitter *e, uint16_t v) {
    em_byte(e, (uint8_t)(v & 0xFF));
    em_byte(e, (uint8_t)(v >> 8));
}

void em_const_int(emitter *e, int64_t v, bc_program *p) {
    // Most integers are small enough to ride in the instruction.
    if (v >= -128 && v <= 127) {
        em_op(e, OP_CONST_I8);
        em_byte(e, (uint8_t)(int8_t)v);
    } else {
        uint16_t i = bc_add_int(p, v);
        em_op(e, OP_CONST);
        em_u16(e, i);
    }
}

int em_jump(emitter *e, opcode op) {
    em_op(e, op);
    em_byte(e, 0);
    em_byte(e, 0);
    return e->len - 2;
}

void em_patch_to(emitter *e, int at, int target) {
    // Offsets are relative to the end of the operand.
    int delta = target - (at + 2);
    int16_t d = (int16_t)delta;
    e->code[at] = (uint8_t)((uint16_t)d & 0xFF);
    e->code[at + 1] = (uint8_t)((uint16_t)d >> 8);
}

void em_patch(emitter *e, int at) { em_patch_to(e, at, e->len); }

void em_jump_back(emitter *e, opcode op, int target) {
    em_op(e, op);
    int delta = target - (e->len + 2);
    int16_t d = (int16_t)delta;
    em_byte(e, (uint8_t)((uint16_t)d & 0xFF));
    em_byte(e, (uint8_t)((uint16_t)d >> 8));
}

int em_here(const emitter *e) { return e->len; }
