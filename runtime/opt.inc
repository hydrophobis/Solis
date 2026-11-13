// Register-code optimizer (experimental).
//
// Runs on each function after translation. Three things, all of which keep the
// program's behaviour exactly:
//
//   1. Rewrite rules from opt/rules.txt (tools/optrules.py checks each one and
//      generates opt_rules.inc). A rule fires only when its side conditions
//      hold; "dead(t)" means t is not read again, from a liveness analysis.
//   2. Removal of pure instructions whose result is never read.
//   3. Bounds-check elimination: an array read or write whose index is known to
//      satisfy 0 <= i < len(a) becomes an unchecked variant. The facts come from a
//      forward dataflow analysis; anything that could change an array's length
//      (calls, natives, pushes) forgets every length fact.
//
// SOLIS_NOOPT=1 turns it off. SOLIS_OPT_STATS=1 reports what it did. Build with
// -DSL_OPT_ASSERT and the unchecked instructions verify their own bounds, so a
// wrong fact shows up as an abort instead of a silent bad read.

typedef struct {
    uint16_t op;
    uint8_t  n;               // operand words
    uint16_t w[8];
    int      id;              // stable identity, survives rewrites
    int      target;          // id of the jump target, -1 none, -2 end of function
    int      jw;              // which operand is the jump offset, -1 none
} oins;

enum { OK_REG, OK_IMM, OK_LIT, OK_JUMP, OK_EXPR };
typedef struct { uint8_t kind; int32_t val; int32_t k[4]; } oarg;
typedef struct { uint16_t op; uint8_t n; oarg a[7]; } opat;
enum { OC_DEAD, OC_NE, OC_EQ, OC_FITS16, OC_NONNEG };
typedef struct { uint8_t kind; int a, b; int32_t c0; int32_t ck[4]; } ocond;
typedef struct { const char *name; uint8_t npat, nrep, ncond; opat pat[3]; opat rep[2]; ocond cond[4]; } orule;

#include "opt_rules.inc"

typedef struct {
    oins *a; int n, cap;
    int next_id;
    int nregs;
    sl_vm *vm;
    // statistics
    int n_rules, n_dce, n_bce;
    int rule_hits[OPT_NRULES];
} olist;

// Operand letters: d result, u read, e read and written (consumed or updated),
// i/k plain constant, j jump offset. NULL: not understood, leave the function alone.
static const char *opt_spec(int op) {
    switch (op) {
        case R_LDI: return "di";
        case R_LDC_I: case R_LDC_F: return "dkkkk";
        case R_LDS: case R_LDB: case R_LOADG: return "dk";
        case R_LDNULL: return "d";
        case R_MOV: case R_MOVS: return "du";
        case R_STORE: case R_STORE_L: return "eu";
        case R_DROP: return "e";
        case R_ADD_I: case R_SUB_I: case R_MUL_I: case R_DIV_I: case R_REM_I: return "duu";
        case R_ADDI: return "dui";
        case R_NEG_I: case R_NEG_F: case R_NOT: return "du";
        case R_ADD_F: case R_SUB_F: case R_MUL_F: case R_DIV_F: case R_REM_F: return "duu";
        case R_EQ_B: case R_NE_B: return "duu";
        case R_EQ_S: case R_NE_S: case R_CONCAT: return "dee";
        case R_STR_I: case R_STR_F: case R_STR_B: return "du";
        case R_STR_OBJ: return "de";
        case R_JMP: return "j";
        case R_JT: case R_JF: return "uj";
        case R_RET: return "u";
        case R_RETV: case R_HALT: return "";
        case R_AGET: return "deu";
        case R_AGET_L: return "duu";
        case R_ASET: return "eue";
        case R_ASET_L: return "uue";
        case R_ALEN: return "de";
        case R_ALEN_L: return "du";
        case R_APUSH: return "ee";
        case R_APUSH_L: return "ue";
        case R_SLICE: return "deuu";
        case R_FGET: return "dek";
        case R_FGET_L: return "duk";
        case R_FSET: return "eke";
        case R_FSET_L: return "uke";
        case R_VTAG: return "de";
        case R_VGET: return "dek";
        case R_STOREG: return "ke";
        case R_COPY: return "dek";
        case R_MGET: return "dee";
        case R_MSET: return "eee";
        case R_AJLT: return "euij";
        case R_AJLTI: return "eiij";
        case R_JLTLEN: return "uuj";
        case R_MOV2: return "dudu";
        case R_MOV3: return "dududu";
        case R_AGET_S: return "euu";
        case R_ASET_LC: return "uuu";
        case R_APUSH_LC: return "uu";
        case R_AGET_LU: return "duu";
        case R_ASET_LU: return "uue";
        case R_AGET_SU: return "euu";
        case R_ASET_LCU: return "uuu";
        // calls and constructors read or write a run of registers
        case R_CALL: return "kk";
        case R_NATIVE: return "kkkk";
        case R_NEWARR: return "dk";
        case R_NEWSTRUCT: return "dkk";
        case R_NEWVAR: return "dkk";
        case R_NEWMAP: return "dk";
        default:
            if ((op >= R_EQ_I && op <= R_GE_I) || (op >= R_EQ_F && op <= R_GE_F)) return "duu";
            if ((op >= R_JC_EQ_I && op <= R_JN_GE_F)) return "uuj";
            if ((op >= R_JCI_EQ && op <= R_JCI_GE)) return "uij";
            return NULL;
    }
}

// Pure: no side effect and cannot fault, so it may be dropped when its result is dead.
static bool opt_pure(int op) {
    switch (op) {
        case R_LDI: case R_LDC_I: case R_LDC_F: case R_LDB: case R_MOVS:
        case R_ADD_I: case R_SUB_I: case R_MUL_I: case R_ADDI: case R_NEG_I:
        case R_ADD_F: case R_SUB_F: case R_MUL_F: case R_DIV_F: case R_REM_F: case R_NEG_F:
        case R_EQ_B: case R_NE_B: case R_NOT:
            return true;
        default:
            return (op >= R_EQ_I && op <= R_GE_I) || (op >= R_EQ_F && op <= R_GE_F);
    }
}

static int opt_len(sl_vm *vm, const uint16_t *code, uint32_t at) {
    const char *sp = opt_spec(code[at]);
    (void)vm;
    if (!sp) return 0;
    return 1 + (int)strlen(sp);
}

static bool opt_decode(sl_vm *vm, sl_func *f, olist *L) {
    uint32_t n = f->rlen;
    int *w2i = (int *)malloc((n + 1) * sizeof(int));
    if (!w2i) return false;
    for (uint32_t i = 0; i <= n; i++) w2i[i] = -1;
    L->cap = 64; L->n = 0; L->a = (oins *)malloc((size_t)L->cap * sizeof(oins));
    for (uint32_t at = 0; at < n;) {
        int len = opt_len(vm, f->rcode, at);
        if (!len || at + (uint32_t)len > n) { free(w2i); return false; }
        if (L->n == L->cap) { L->cap *= 2; L->a = (oins *)realloc(L->a, (size_t)L->cap * sizeof(oins)); }
        oins *x = &L->a[L->n];
        x->op = f->rcode[at]; x->n = (uint8_t)(len - 1); x->id = L->n; x->target = -1; x->jw = -1;
        for (int k = 0; k < len - 1; k++) x->w[k] = f->rcode[at + 1 + (uint32_t)k];
        const char *sp = opt_spec(x->op);
        for (int k = 0; k < len - 1; k++) if (sp[k] == 'j') x->jw = k;
        w2i[at] = L->n;
        L->n++;
        at += (uint32_t)len;
    }
    // second walk, now that word positions are known
    uint32_t at = 0;
    for (int i = 0; i < L->n; i++) {
        oins *x = &L->a[i];
        uint32_t next = at + 1 + x->n;
        if (x->jw >= 0) {
            long tgt = (long)next + (int16_t)x->w[x->jw];
            if (tgt < 0 || tgt > (long)n) { free(w2i); return false; }
            x->target = tgt == (long)n ? -2 : w2i[tgt];
            if (x->target == -1 && tgt != (long)n) { free(w2i); return false; }
        }
        at = next;
    }
    free(w2i);
    L->next_id = L->n;
    return true;
}

static int opt_index_of(const olist *L, int id) {
    for (int i = 0; i < L->n; i++) if (L->a[i].id == id) return i;
    return -1;
}

static bool opt_encode(olist *L, sl_func *f) {
    uint32_t total = 0;
    uint32_t *pos = (uint32_t *)malloc(((size_t)L->n + 1) * sizeof(uint32_t));
    if (!pos) return false;
    for (int i = 0; i < L->n; i++) { pos[i] = total; total += 1 + L->a[i].n; }
    pos[L->n] = total;
    uint16_t *out = (uint16_t *)malloc((size_t)(total ? total : 1) * sizeof(uint16_t));
    if (!out) { free(pos); return false; }
    for (int i = 0; i < L->n; i++) {
        oins *x = &L->a[i];
        uint32_t at = pos[i];
        out[at] = x->op;
        for (int k = 0; k < x->n; k++) out[at + 1 + (uint32_t)k] = x->w[k];
        if (x->jw >= 0) {
            uint32_t tp;
            if (x->target == -2) tp = total;
            else { int ti = opt_index_of(L, x->target); if (ti < 0) { free(pos); free(out); return false; } tp = pos[ti]; }
            long rel = (long)tp - (long)(at + 1 + x->n);
            if (rel < -32768 || rel > 32767) { free(pos); free(out); return false; }
            out[at + 1 + (uint32_t)x->jw] = (uint16_t)(int16_t)rel;
        }
    }
    free(pos);
    free(f->rcode);
    f->rcode = out;
    f->rlen = total;
    return true;
}

// ---- effects and liveness ---------------------------------------------------

typedef struct {
    int nu, nd;
    uint16_t u[16], d[16];
    int clobber_from;       // every register from here up is overwritten (-1 none)
    int ru_lo, ru_hi;       // a run of registers read and consumed [lo,hi)
} oeff;

static void opt_effects(const olist *L, const oins *x, oeff *e) {
    e->nu = e->nd = 0; e->clobber_from = -1; e->ru_lo = e->ru_hi = 0;
    const char *sp = opt_spec(x->op);
    switch (x->op) {
        case R_CALL: {
            int base = x->w[1];
            sl_func *g = &L->vm->funcs[x->w[0]];
            e->ru_lo = base; e->ru_hi = base + g->arity;
            e->clobber_from = base;
            e->d[e->nd++] = (uint16_t)base;
            return;
        }
        case R_NATIVE: {
            int base = x->w[1], argc = x->w[2];
            e->ru_lo = base; e->ru_hi = base + argc;
            if (x->w[3]) e->d[e->nd++] = (uint16_t)base;
            return;
        }
        case R_NEWARR: case R_NEWMAP: {
            int d = x->w[0], cnt = x->w[1] * (x->op == R_NEWMAP ? 2 : 1);
            e->ru_lo = d; e->ru_hi = d + cnt;
            e->d[e->nd++] = (uint16_t)d;
            return;
        }
        case R_NEWSTRUCT: {
            int d = x->w[0], cnt = x->w[1];
            e->ru_lo = d; e->ru_hi = d + cnt;
            e->d[e->nd++] = (uint16_t)d;
            return;
        }
        case R_NEWVAR: {
            int d = x->w[0], cnt = x->w[2];
            e->ru_lo = d; e->ru_hi = d + cnt;
            e->d[e->nd++] = (uint16_t)d;
            return;
        }
        default: break;
    }
    for (int k = 0; k < x->n; k++) {
        char c = sp[k];
        if (c == 'u' || c == 'e') e->u[e->nu++] = x->w[k];
        if (c == 'd' || c == 'e') e->d[e->nd++] = x->w[k];
    }
}

#define BITSW(nb) (((nb) + 63) / 64)
static inline bool bt(const uint64_t *b, int i) { return (b[i / 64] >> (i % 64)) & 1; }
static inline void bset(uint64_t *b, int i) { b[i / 64] |= 1ull << (i % 64); }
static inline void bclr(uint64_t *b, int i) { b[i / 64] &= ~(1ull << (i % 64)); }

static bool opt_falls(const oins *x) {
    return x->op != R_JMP && x->op != R_RET && x->op != R_RETV && x->op != R_HALT;
}

// live_out[i] (a bitset over registers) after every instruction
static uint64_t *opt_liveness(const olist *L) {
    int W = BITSW(L->nregs);
    uint64_t *out = (uint64_t *)calloc((size_t)(L->n + 1) * W, sizeof(uint64_t));
    uint64_t *in = (uint64_t *)calloc((size_t)(L->n + 1) * W, sizeof(uint64_t));
    if (!out || !in) { free(out); free(in); return NULL; }
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = L->n - 1; i >= 0; i--) {
            const oins *x = &L->a[i];
            uint64_t *o = &out[(size_t)i * W];
            if (opt_falls(x) && i + 1 < L->n)
                for (int k = 0; k < W; k++) o[k] |= in[(size_t)(i + 1) * W + k];
            if (x->target >= 0) {
                int ti = opt_index_of(L, x->target);
                if (ti >= 0) for (int k = 0; k < W; k++) o[k] |= in[(size_t)ti * W + k];
            }
            oeff e; opt_effects(L, x, &e);
            uint64_t *iv = &in[(size_t)i * W];
            uint64_t tmp[64];
            if (W > 64) { free(out); free(in); return NULL; }
            for (int k = 0; k < W; k++) tmp[k] = o[k];
            for (int k = 0; k < e.nd; k++) bclr(tmp, e.d[k]);
            if (e.clobber_from >= 0) for (int r = e.clobber_from; r < L->nregs; r++) bclr(tmp, r);
            for (int k = 0; k < e.nu; k++) bset(tmp, e.u[k]);
            for (int r = e.ru_lo; r < e.ru_hi; r++) if (r < L->nregs) bset(tmp, r);
            for (int k = 0; k < W; k++) if (iv[k] != tmp[k]) { iv[k] = tmp[k]; changed = true; }
        }
    }
    free(in);
    return out;
}

static bool opt_is_target(const olist *L, int index) {
    int id = L->a[index].id;
    for (int i = 0; i < L->n; i++) if (L->a[i].target == id) return true;
    return false;
}

static void opt_retarget(olist *L, int from_id, int to_id) {
    for (int i = 0; i < L->n; i++) if (L->a[i].target == from_id) L->a[i].target = to_id;
}

// Replace instructions [i, i+m) with the m2 instructions in `rep`.
static void opt_splice(olist *L, int i, int m, const oins *rep, int m2) {
    int delta = m2 - m;
    int old_first_id = L->a[i].id;
    int after_id = (i + m < L->n) ? L->a[i + m].id : -2;
    if (delta > 0) {
        if (L->n + delta > L->cap) { L->cap = (L->n + delta) * 2; L->a = (oins *)realloc(L->a, (size_t)L->cap * sizeof(oins)); }
    }
    memmove(&L->a[i + m2], &L->a[i + m], (size_t)(L->n - i - m) * sizeof(oins));
    for (int k = 0; k < m2; k++) {
        L->a[i + k] = rep[k];
        L->a[i + k].id = k == 0 ? old_first_id : L->next_id++;
    }
    L->n += delta;
    if (m2 == 0) opt_retarget(L, old_first_id, after_id);
}

// ---- rules --------------------------------------------------------------------

static bool opt_try_rule(olist *L, int i, const orule *r, const uint64_t *live_out, int W) {
    if (i + r->npat > L->n) return false;
    int reg[8], imm[4], jmp[2];
    for (int k = 0; k < 8; k++) reg[k] = -1;
    bool immset[4] = { false, false, false, false };
    for (int k = 0; k < 2; k++) jmp[k] = -3;
    for (int p = 0; p < r->npat; p++) {
        const oins *x = &L->a[i + p];
        const opat *pt = &r->pat[p];
        if (x->op != pt->op || x->n != pt->n) return false;
        if (p > 0 && opt_is_target(L, i + p)) return false;       // something jumps into the middle
        for (int a = 0; a < pt->n; a++) {
            const oarg *g = &pt->a[a];
            switch (g->kind) {
                case OK_REG:
                    if (reg[g->val] < 0) reg[g->val] = x->w[a]; else if (reg[g->val] != x->w[a]) return false;
                    break;
                case OK_IMM: {
                    int v = (int16_t)x->w[a];
                    if (immset[g->val] && imm[g->val] != v) return false;
                    imm[g->val] = v; immset[g->val] = true;
                    break;
                }
                case OK_LIT: if ((int16_t)x->w[a] != g->val) return false; break;
                case OK_JUMP: jmp[g->val] = x->target; break;
            }
        }
    }
    for (int c = 0; c < r->ncond; c++) {
        const ocond *k = &r->cond[c];
        switch (k->kind) {
            case OC_DEAD: if (bt(&live_out[(size_t)(i + r->npat - 1) * W], reg[k->a])) return false; break;
            case OC_NE: if (reg[k->a] == reg[k->b]) return false; break;
            case OC_EQ: if (reg[k->a] != reg[k->b]) return false; break;
            case OC_FITS16: case OC_NONNEG: {
                long v = k->c0;
                for (int q = 0; q < 4; q++) v += (long)k->ck[q] * (immset[q] ? imm[q] : 0);
                if (k->kind == OC_FITS16 ? (v < -32768 || v > 32767) : v < 0) return false;
                break;
            }
        }
    }
    oins rep[2];
    for (int p = 0; p < r->nrep; p++) {
        const opat *pt = &r->rep[p];
        oins *y = &rep[p];
        memset(y, 0, sizeof *y);
        y->op = pt->op; y->n = pt->n; y->target = -1; y->jw = -1;
        const char *sp = opt_spec(pt->op);
        for (int a = 0; a < pt->n; a++) {
            const oarg *g = &pt->a[a];
            if (g->kind == OK_REG) y->w[a] = (uint16_t)reg[g->val];
            else if (g->kind == OK_EXPR) {
                long v = g->val;
                for (int q = 0; q < 4; q++) v += (long)g->k[q] * (immset[q] ? imm[q] : 0);
                y->w[a] = (uint16_t)(int16_t)v;
            } else if (g->kind == OK_JUMP) { y->target = jmp[g->val]; y->jw = a; }
            if (sp[a] == 'j') y->jw = a;
        }
    }
    int hit = (int)(r - opt_rules);
    L->rule_hits[hit]++;
    L->n_rules++;
    opt_splice(L, i, r->npat, rep, r->nrep);
    return true;
}

static bool opt_apply_rules(olist *L) {
    bool any = false;
    for (int guard = 0; guard < 400; guard++) {
        uint64_t *live = opt_liveness(L);
        if (!live) return any;
        int W = BITSW(L->nregs);
        bool fired = false;
        for (int i = 0; i < L->n && !fired; i++)
            for (unsigned r = 0; r < OPT_NRULES && !fired; r++)
                if (opt_try_rule(L, i, &opt_rules[r], live, W)) fired = true;
        if (!fired) {
            // dead pure instructions
            for (int i = 0; i < L->n && !fired; i++) {
                oeff e; opt_effects(L, &L->a[i], &e);
                if (!opt_pure(L->a[i].op) || e.nd != 1) continue;
                if (bt(&live[(size_t)i * W], e.d[0])) continue;
                opt_splice(L, i, 1, NULL, 0);
                L->n_dce++;
                fired = true;
            }
        }
        free(live);
        if (!fired) break;
        any = true;
    }
    return any;
}

// ---- bounds-check elimination ---------------------------------------------------
//
// Facts, per program point:
//   NN(r)      r >= 0
//   LT(x,a)    x < len(a)           (a is a register holding an array)
//   LEN(t,a)   t == len(a)
// An array's length can change through any alias, so every LT and LEN fact is
// dropped at a call, a native, or a push. Array lengths are assumed to stay below
// 2**48, which is what lets "x < len(a)" rule out overflow in `x += small`.

typedef struct {
    int nregs, npairs, nlens;
    uint16_t *px, *pa;          // LT pair table
    uint16_t *lt_, *la_;        // LEN pair table
    int total;
} bfacts;

static int bf_nn(const bfacts *f, int r) { (void)f; return r; }
static int bf_lt(const bfacts *f, int x, int a) {
    for (int i = 0; i < f->npairs; i++) if (f->px[i] == x && f->pa[i] == a) return f->nregs + i;
    return -1;
}
static int bf_len(const bfacts *f, int t, int a) {
    for (int i = 0; i < f->nlens; i++) if (f->lt_[i] == t && f->la_[i] == a) return f->nregs + f->npairs + i;
    return -1;
}

static void bf_kill_reg(const bfacts *f, uint64_t *s, int r) {
    bclr(s, bf_nn(f, r));
    for (int i = 0; i < f->npairs; i++) if (f->px[i] == r || f->pa[i] == r) bclr(s, f->nregs + i);
    for (int i = 0; i < f->nlens; i++) if (f->lt_[i] == r || f->la_[i] == r) bclr(s, f->nregs + f->npairs + i);
}
static void bf_kill_lengths(const bfacts *f, uint64_t *s) {
    for (int i = 0; i < f->npairs; i++) bclr(s, f->nregs + i);
    for (int i = 0; i < f->nlens; i++) bclr(s, f->nregs + f->npairs + i);
}
static bool bf_bounded(const bfacts *f, const uint64_t *s, int x) {
    for (int i = 0; i < f->npairs; i++) if (f->px[i] == x && bt(s, f->nregs + i)) return true;
    return false;
}

// Apply the straight-line effect of instruction x to the fact set.
static void bf_transfer(const bfacts *f, const oins *x, const olist *L, uint64_t *s) {
    int op = x->op;
    oeff e; opt_effects(L, x, &e);
    // what the instruction establishes, judged against the state before it
    int gen_nn = -1, gen_len_t = -1, gen_len_a = -1, copy_from = -1, copy_to = -1;
    if (op == R_LDI && (int16_t)x->w[1] >= 0) gen_nn = x->w[0];
    if (op == R_MOVS && x->w[0] != x->w[1]) { copy_from = x->w[1]; copy_to = x->w[0]; }
    if (op == R_ADDI && (int16_t)x->w[2] >= 0 && bt(s, bf_nn(f, x->w[1])) && bf_bounded(f, s, x->w[1])) gen_nn = x->w[0];
    if ((op == R_AJLT || op == R_AJLTI) && (int16_t)x->w[2] >= 0 && bt(s, bf_nn(f, x->w[0])) && bf_bounded(f, s, x->w[0])) gen_nn = x->w[0];
    if (op == R_ALEN_L) { gen_len_t = x->w[0]; gen_len_a = x->w[1]; }
    uint64_t *copy_nn = NULL;
    uint64_t save[64];
    int W = BITSW(f->total);
    if (copy_from >= 0) { for (int k = 0; k < W; k++) save[k] = s[k]; copy_nn = save; }

    for (int k = 0; k < e.nd; k++) bf_kill_reg(f, s, e.d[k]);
    if (e.clobber_from >= 0) for (int r = e.clobber_from; r < f->nregs; r++) bf_kill_reg(f, s, r);
    for (int r = e.ru_lo; r < e.ru_hi && r < f->nregs; r++) bf_kill_reg(f, s, r);
    if (op == R_CALL || op == R_NATIVE || op == R_APUSH || op == R_APUSH_L || op == R_APUSH_LC) bf_kill_lengths(f, s);
    // a consumed or updated register (the "e" operands) is no longer what it was
    {
        const char *sp = opt_spec(op);
        if (sp) for (int k = 0; k < x->n; k++) if (sp[k] == 'e') bf_kill_reg(f, s, x->w[k]);
    }

    if (gen_nn >= 0) bset(s, bf_nn(f, gen_nn));
    if (copy_from >= 0) {
        if (bt(copy_nn, bf_nn(f, copy_from))) bset(s, bf_nn(f, copy_to));
        for (int i = 0; i < f->npairs; i++)
            if (f->px[i] == copy_from && bt(copy_nn, f->nregs + i)) { int q = bf_lt(f, copy_to, f->pa[i]); if (q >= 0) bset(s, q); }
        for (int i = 0; i < f->nlens; i++)
            if (f->lt_[i] == copy_from && bt(copy_nn, f->nregs + f->npairs + i)) { int q = bf_len(f, copy_to, f->la_[i]); if (q >= 0) bset(s, q); }
    }
    if (gen_len_t >= 0 && gen_len_t != gen_len_a) { int q = bf_len(f, gen_len_t, gen_len_a); if (q >= 0) bset(s, q); }
}

// Facts that hold on the taken (edge=1) or fall-through (edge=0) side of a branch.
static void bf_edge(const bfacts *f, const oins *x, int edge, uint64_t *s) {
    int op = x->op;
    int lt_x = -1, lt_y = -1, ge_x = -1, ge_y = -1;      // "x < y" / "x >= y" on this edge
    if (op >= R_JC_EQ_I && op <= R_JC_GE_I) {
        int c = op - R_JC_EQ_I;     // EQ NE LT LE GT GE
        int a = x->w[0], b = x->w[1];
        bool lt = (c == 2 && edge) || (c == 5 && !edge);        // a < b
        bool ge = (c == 5 && edge) || (c == 2 && !edge);        // a >= b
        bool gt = (c == 4 && edge) || (c == 3 && !edge);        // a > b
        bool le = (c == 3 && edge) || (c == 4 && !edge);        // a <= b
        if (lt) { lt_x = a; lt_y = b; }
        if (gt) { lt_x = b; lt_y = a; ge_x = a; ge_y = b; }       // b < a, and a > b >= 0 means a >= 0
        if (ge) { ge_x = a; ge_y = b; }
        if (le) { ge_x = b; ge_y = a; }                          // b >= a
    } else if (op >= R_JCI_EQ && op <= R_JCI_GE) {
        int c = op - R_JCI_EQ, a = x->w[0];
        int k = (int16_t)x->w[1];
        bool ge = (c == 5 && edge) || (c == 2 && !edge);         // a >= k
        bool gt = (c == 4 && edge) || (c == 3 && !edge);         // a > k
        if ((ge && k >= 0) || (gt && k >= -1)) bset(s, bf_nn(f, a));
    } else if (op == R_JLTLEN) {
        if (edge) { int q = bf_lt(f, x->w[0], x->w[1]); if (q >= 0) bset(s, q); }
    } else if (op == R_AJLT) {
        if (edge) {
            // x += step happened; the test is x < lim
            int len_a = -1;
            for (int i = 0; i < f->nlens; i++) if (f->lt_[i] == x->w[1] && bt(s, f->nregs + f->npairs + i)) len_a = f->la_[i];
            if (len_a >= 0) { int q = bf_lt(f, x->w[0], len_a); if (q >= 0) bset(s, q); }
        }
    }
    if (lt_x >= 0) {
        for (int i = 0; i < f->nlens; i++)
            if (f->lt_[i] == lt_y && bt(s, f->nregs + f->npairs + i)) { int q = bf_lt(f, lt_x, f->la_[i]); if (q >= 0) bset(s, q); }
    }
    if (ge_x >= 0 && bt(s, bf_nn(f, ge_y))) bset(s, bf_nn(f, ge_x));
}

static bool opt_bce(olist *L) {
    // which registers are used as an index or as an array in a checked access
    uint16_t idxr[64], arrr[64]; int ni = 0, na = 0;
    uint16_t lent[64], lena[64]; int nl = 0;
    for (int i = 0; i < L->n; i++) {
        const oins *x = &L->a[i];
        int a = -1, ix = -1;
        switch (x->op) {
            case R_AGET_L: a = x->w[1]; ix = x->w[2]; break;
            case R_ASET_L: a = x->w[0]; ix = x->w[1]; break;
            case R_AGET_S: a = x->w[1]; ix = x->w[2]; break;
            case R_ASET_LC: a = x->w[0]; ix = x->w[1]; break;
            case R_ALEN_L: if (nl < 64) { lent[nl] = x->w[0]; lena[nl] = x->w[1]; nl++; } break;
            default: break;
        }
        if (a >= 0) {
            bool f1 = false, f2 = false;
            for (int k = 0; k < ni; k++) if (idxr[k] == ix) f1 = true;
            for (int k = 0; k < na; k++) if (arrr[k] == a) f2 = true;
            if (!f1 && ni < 64) idxr[ni++] = (uint16_t)ix;
            if (!f2 && na < 64) arrr[na++] = (uint16_t)a;
        }
    }
    if (!ni || !na) return false;
    // lengths carried through plain copies (`k = len(a)` goes via a temporary)
    for (int changed = 1; changed && nl < 60;) {
        changed = 0;
        for (int i = 0; i < L->n && nl < 60; i++) {
            const oins *x = &L->a[i];
            if (x->op != R_MOVS) continue;
            for (int q = 0; q < nl; q++) {
                if (lent[q] != x->w[1]) continue;
                bool have = false;
                for (int r = 0; r < nl; r++) if (lent[r] == x->w[0] && lena[r] == lena[q]) have = true;
                if (!have && nl < 60) { lent[nl] = x->w[0]; lena[nl] = lena[q]; nl++; changed = 1; }
            }
        }
    }
    bfacts f;
    f.nregs = L->nregs; f.npairs = ni * na; f.nlens = nl;
    f.px = (uint16_t *)malloc((size_t)(f.npairs ? f.npairs : 1) * sizeof(uint16_t));
    f.pa = (uint16_t *)malloc((size_t)(f.npairs ? f.npairs : 1) * sizeof(uint16_t));
    f.lt_ = lent; f.la_ = lena;
    for (int a = 0, k = 0; a < na; a++) for (int x = 0; x < ni; x++, k++) { f.px[k] = idxr[x]; f.pa[k] = arrr[a]; }
    f.total = f.nregs + f.npairs + f.nlens;
    int W = BITSW(f.total);
    if (W > 64) { free(f.px); free(f.pa); return false; }
    uint64_t *in = (uint64_t *)malloc((size_t)(L->n + 1) * W * sizeof(uint64_t));
    for (int i = 0; i <= L->n; i++) for (int k = 0; k < W; k++) in[(size_t)i * W + k] = i == 0 ? 0 : ~0ull;
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < L->n; i++) {
            const oins *x = &L->a[i];
            uint64_t base[64];
            for (int k = 0; k < W; k++) base[k] = in[(size_t)i * W + k];
            // fall-through and taken edges
            uint64_t fall[64], taken[64];
            for (int k = 0; k < W; k++) fall[k] = taken[k] = base[k];
            bf_transfer(&f, x, L, fall);
            for (int k = 0; k < W; k++) taken[k] = fall[k];
            if (x->jw >= 0 && x->op != R_JMP) {
                bf_edge(&f, x, 0, fall);
                bf_edge(&f, x, 1, taken);
            }
            if (opt_falls(x) && i + 1 < L->n)
                for (int k = 0; k < W; k++) { uint64_t v = in[(size_t)(i + 1) * W + k] & fall[k]; if (v != in[(size_t)(i + 1) * W + k]) { in[(size_t)(i + 1) * W + k] = v; changed = true; } }
            if (x->target >= 0) {
                int ti = opt_index_of(L, x->target);
                if (ti >= 0)
                    for (int k = 0; k < W; k++) { uint64_t v = in[(size_t)ti * W + k] & taken[k]; if (v != in[(size_t)ti * W + k]) { in[(size_t)ti * W + k] = v; changed = true; } }
            }
        }
    }
    bool any = false;
    for (int i = 0; i < L->n; i++) {
        oins *x = &L->a[i];
        int a, ix, nop = -1;
        switch (x->op) {
            case R_AGET_L: a = x->w[1]; ix = x->w[2]; nop = R_AGET_LU; break;
            case R_ASET_L: a = x->w[0]; ix = x->w[1]; nop = R_ASET_LU; break;
            case R_AGET_S: a = x->w[1]; ix = x->w[2]; nop = R_AGET_SU; break;
            case R_ASET_LC: a = x->w[0]; ix = x->w[1]; nop = R_ASET_LCU; break;
            default: continue;
        }
        const uint64_t *s = &in[(size_t)i * W];
        int q = bf_lt(&f, ix, a);
        if (q >= 0 && bt(s, bf_nn(&f, ix)) && bt(s, q)) { x->op = (uint16_t)nop; L->n_bce++; any = true; }
    }
    free(in); free(f.px); free(f.pa);
    return any;
}

static void opt_function(sl_vm *vm, sl_func *f, int fid) {
#ifdef SL_DEBUG_ENV
    // The command-line tool turns these on; a library build never reads the environment.
    static int enabled = -1, stats = -1, list = -1;
    if (enabled < 0) {
        enabled = !getenv("SOLIS_NOOPT");
        stats = getenv("SOLIS_OPT_STATS") != NULL;
        list = getenv("SOLIS_OPT_LIST") != NULL;
    }
    if (!enabled) return;
#else
    const int stats = 0, list = 0;
#endif
    olist L;
    memset(&L, 0, sizeof L);
    L.vm = vm; L.nregs = f->nregs;
    if (!opt_decode(vm, f, &L)) { free(L.a); return; }
    int before = L.n;
    opt_apply_rules(&L);
    opt_bce(&L);
    if (L.n_rules || L.n_dce || L.n_bce) {
        if (!opt_encode(&L, f) && stats) fprintf(stderr, "opt: function %d not re-encoded\n", fid);
    }
    if (list) {
        fprintf(stderr, "== %s\n", f->name);
        for (int i = 0; i < L.n; i++) {
            fprintf(stderr, "%3d: op%-3d", i, L.a[i].op);
            for (int k = 0; k < L.a[i].n; k++) fprintf(stderr, " %d", (int)(int16_t)L.a[i].w[k]);
            if (L.a[i].jw >= 0) fprintf(stderr, "   -> #%d", L.a[i].target);
            fprintf(stderr, "\n");
        }
    }
    if (stats && (L.n_rules || L.n_dce || L.n_bce))
        fprintf(stderr, "opt: %-18s %3d -> %3d instrs, %d rule rewrites, %d dead removed, %d bounds checks removed\n",
                f->name, before, L.n, L.n_rules, L.n_dce, L.n_bce);
    free(L.a);
}
