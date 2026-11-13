// `solis --emit-c`: translate register code to C.
//
// Each Solis function becomes one C function. The register code's registers
// are split into "webs" (all definitions that can reach a common use), each
// web becomes one C variable, and its type is inferred from how typed
// instructions touch it: ints, floats and bools become plain C locals, and
// only webs that hold heap values (or are never pinned down) stay sl_value.
// Calls pass scalars by value.

#include <inttypes.h>

typedef struct {
    int      op, at, len;
    int      nuse;  uint16_t *use;     // registers read
    int      ndef;  uint16_t  def;     // register written
    long     target;                   // word index jumped to, or -1
    int     *useweb;                   // web per use
    int      defweb;
} ains;

typedef struct {
    sl_func *f;
    ains    *ins;  int n;
    int     *w2i;                      // word index -> instruction
    int      dbase;                    // first defsite/web id
    int      nreg;
    int      retnode;                  // class node standing for the return value
    bool     returns;
    bool     recursive;                // on a call cycle: only these track depth
} afn;

static int aot_oplen(int op) {
    switch (op) {
        case R_RETV: case R_HALT: return 1;
        case R_LDNULL: case R_DROP: case R_JMP: case R_RET: return 2;
        case R_LDC_I: case R_LDC_F: return 6;
        case R_NATIVE: case R_SLICE: case R_NEWVAR: return op == R_NATIVE ? 5 : (op == R_SLICE ? 5 : 4);
        case R_NEWSTRUCT: case R_COPY: return 4;
        case R_AJLT: case R_AJLTI: return 5;
        case R_JLTLEN: return 4;
        case R_LDI: case R_LDS: case R_LDB: case R_MOV: case R_MOVS: case R_STORE: case R_STORE_L:
        case R_NEG_I: case R_NEG_F: case R_NOT: case R_STR_I: case R_STR_F: case R_STR_B: case R_STR_OBJ:
        case R_JT: case R_JF: case R_CALL: case R_NEWARR: case R_NEWMAP:
        case R_ALEN: case R_ALEN_L: case R_APUSH: case R_APUSH_L: case R_VTAG: case R_LOADG:
        case R_STOREG:
            return 3;
        default: return 4;
    }
}

static int *aot_stid;             // struct type id per class: -1 none, -2 conflicting
static sl_vm *aot_vm;
static int *aot_par, *aot_cpar;       // web and class union-find
static uint8_t *aot_mask;

static int fnd(int *p, int x) { while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; } return x; }
static void uni(int *p, int a, int b) { a = fnd(p, a); b = fnd(p, b); if (a != b) p[b] = a; }
static void cuni(int a, int b) {
    a = fnd(aot_cpar, fnd(aot_par, a)); b = fnd(aot_cpar, fnd(aot_par, b));
    if (a == b) return;
    aot_cpar[b] = a;
    aot_mask[a] |= aot_mask[b];
    if (aot_stid[a] == -1) aot_stid[a] = aot_stid[b];
    else if (aot_stid[b] != -1 && aot_stid[b] != aot_stid[a]) aot_stid[a] = -2;
}
static void ctid(int web, int tid) {
    int c = fnd(aot_cpar, fnd(aot_par, web));
    if (tid == 0xFFFF) { aot_stid[c] = -2; return; }
    if (aot_stid[c] == -1) aot_stid[c] = tid;
    else if (aot_stid[c] != tid) aot_stid[c] = -2;
}
static int ctidof(int web) { return aot_stid[fnd(aot_cpar, fnd(aot_par, web))]; }
static void cmask(int web, int bits) { aot_mask[fnd(aot_cpar, fnd(aot_par, web))] |= bits; }
#define MI 1
#define MF 2
#define MB 4
#define MO 8
#define MS 16      // evidence of a struct value (NEWSTRUCT, COPY, FGET/FSET base)
#define MA 32      // may alias a heap struct someone else holds (element, field, global...)
#define MP 64      // a parameter whose fields the callee writes: the caller must see it


typedef enum { K_INT, K_FLT, K_BOOL, K_GEN, K_STRUCT } akind;
static bool struct_ok(int tid) {
    if (tid < 0 || tid >= aot_vm->nstructs) return false;
    int n = aot_vm->struct_desc[tid][0];
    if (n == 0 || n > 16) return false;
    for (int k = 0; k < n; k++) if (aot_vm->struct_desc[tid][1 + k] == 0) return false;
    return true;
}
static akind kind_of(int web) {
    int c = fnd(aot_cpar, fnd(aot_par, web));
    int m = aot_mask[c];
    // Unboxed only if nothing else can see the object (no alias, no mutated param).
    if (m & MS) return m == MS && struct_ok(aot_stid[c]) ? K_STRUCT : K_GEN;
    m &= ~(MA | MP);
    switch (m) {
        case MI: return K_INT;
        case MF: return K_FLT;
        case MB: return K_BOOL;
        default: return K_GEN;
    }
}
static int tid_of(int web) { return aot_stid[fnd(aot_cpar, fnd(aot_par, web))]; }
static int fkind(int tid, int idx) { return aot_vm->struct_desc[tid][1 + idx]; }

// Decode one function into instructions with their uses and defs.
static bool aot_decode(sl_vm *vm, afn *a) {
    sl_func *f = a->f;
    a->w2i = (int *)malloc((f->rlen + 1) * sizeof(int));
    for (uint32_t i = 0; i <= f->rlen; i++) a->w2i[i] = -1;
    int cap = 64; a->ins = (ains *)calloc((size_t)cap, sizeof(ains));
    uint32_t at = 0;
    a->n = 0;
    while (at < f->rlen) {
        if (a->n == cap) { cap *= 2; a->ins = (ains *)realloc(a->ins, (size_t)cap * sizeof(ains)); }
        ains *in = &a->ins[a->n];
        memset(in, 0, sizeof *in);
        in->op = f->rcode[at]; in->at = (int)at; in->len = aot_oplen(in->op); in->target = -1;
        a->w2i[at] = a->n;
        const uint16_t *w = f->rcode + at + 1;
        uint16_t u[2048]; int nu = 0; int hasdef = 0; uint16_t d = 0;
        switch (in->op) {
            case R_LDI: case R_LDC_I: case R_LDC_F: case R_LDS: case R_LDB: case R_LDNULL: case R_LOADG:
                hasdef = 1; d = w[0]; break;
            case R_MOV: case R_MOVS: case R_NEG_I: case R_NEG_F: case R_NOT: case R_STR_I: case R_STR_F:
            case R_STR_B: case R_STR_OBJ: case R_ALEN: case R_ALEN_L: case R_VTAG: case R_COPY:
                hasdef = 1; d = w[0]; u[nu++] = w[1]; break;
            case R_STORE: case R_STORE_L:
                hasdef = 1; d = w[0]; u[nu++] = w[1]; u[nu++] = w[0]; break;
            case R_DROP: case R_RET: u[nu++] = w[0]; break;
            case R_JT: case R_JF: u[nu++] = w[0]; in->target = (long)at + 3 + (int16_t)w[1]; break;
            case R_JMP: in->target = (long)at + 2 + (int16_t)w[0]; break;
            case R_CALL: {
                sl_func *g = &vm->funcs[w[0]];
                for (int k = 0; k < g->arity; k++) u[nu++] = (uint16_t)(w[1] + k);
                hasdef = 1; d = w[1];     // used only if the callee returns a value
                break;
            }
            case R_NATIVE: {
                for (int k = 0; k < w[2]; k++) u[nu++] = (uint16_t)(w[1] + k);
                if (w[3]) { hasdef = 1; d = w[1]; }
                break;
            }
            case R_NEWARR: case R_NEWSTRUCT: case R_NEWMAP: case R_NEWVAR: {
                int cnt = in->op == R_NEWVAR ? w[2] : w[1];
                if (in->op == R_NEWMAP) cnt *= 2;
                for (int k = 0; k < cnt; k++) u[nu++] = (uint16_t)(w[0] + k);
                hasdef = 1; d = w[0];
                break;
            }
            case R_AGET: case R_AGET_L: case R_AGET_LU: hasdef = 1; d = w[0]; u[nu++] = w[1]; u[nu++] = w[2]; break;
            case R_FGET: case R_FGET_L: hasdef = 1; d = w[0]; u[nu++] = w[1]; break;
            case R_VGET: hasdef = 1; d = w[0]; u[nu++] = w[1]; break;
            case R_ASET: case R_ASET_L: case R_ASET_LU: u[nu++] = w[0]; u[nu++] = w[1]; u[nu++] = w[2]; break;
            case R_FSET: case R_FSET_L: u[nu++] = w[0]; u[nu++] = w[2]; break;
            case R_APUSH: case R_APUSH_L: u[nu++] = w[0]; u[nu++] = w[1]; break;
            case R_SLICE: hasdef = 1; d = w[0]; u[nu++] = w[1]; u[nu++] = w[2]; u[nu++] = w[3]; break;
            case R_STOREG: u[nu++] = w[1]; break;
            case R_MGET: hasdef = 1; d = w[0]; u[nu++] = w[1]; u[nu++] = w[2]; break;
            case R_MSET: u[nu++] = w[0]; u[nu++] = w[1]; u[nu++] = w[2]; break;
            case R_ADDI: hasdef = 1; d = w[0]; u[nu++] = w[1]; break;
            case R_AJLT: hasdef = 1; d = w[0]; u[nu++] = w[0]; u[nu++] = w[1]; in->target = (long)at + 5 + (int16_t)w[3]; break;
            case R_JLTLEN: u[nu++] = w[0]; u[nu++] = w[1]; in->target = (long)at + 4 + (int16_t)w[2]; break;
            case R_AJLTI: hasdef = 1; d = w[0]; u[nu++] = w[0]; in->target = (long)at + 5 + (int16_t)w[3]; break;
            case R_RETV: case R_HALT: break;
            default:
                if (in->op >= R_JC_EQ_I && in->op <= R_JCI_GE) {
                    in->target = (long)at + 4 + (int16_t)w[2];
                    u[nu++] = w[0];
                    if (!(in->op >= R_JCI_EQ)) u[nu++] = w[1];
                } else {
                    // three-operand arithmetic, comparisons, EQ_S, CONCAT
                    hasdef = 1; d = w[0]; u[nu++] = w[1]; u[nu++] = w[2];
                }
        }
        in->nuse = nu;
        in->use = (uint16_t *)malloc((size_t)(nu ? nu : 1) * sizeof(uint16_t));
        memcpy(in->use, u, (size_t)nu * sizeof(uint16_t));
        in->useweb = (int *)malloc((size_t)(nu ? nu : 1) * sizeof(int));
        in->ndef = hasdef; in->def = d;
        a->n++;
        at += (uint32_t)in->len;
    }
    return true;
}

typedef uint64_t bits;

static void aot_webs(afn *a, int *next_id) {
    // defsites: one per register at entry, then one per defining instruction
    a->dbase = *next_id;
    a->nreg = a->f->nregs;
    *next_id += a->nreg + a->n;
}

static void aot_reach(afn *a) {
    int nd = a->nreg + a->n;
    int W = (nd + 63) / 64;
    bits *in = (bits *)calloc((size_t)(a->n + 1) * W, sizeof(bits));
    bits *out = (bits *)calloc((size_t)(a->n + 1) * W, sizeof(bits));
    bits *kill = (bits *)calloc((size_t)(a->nreg + 1) * W, sizeof(bits));   // all defsites of a register
    for (int r = 0; r < a->nreg; r++) kill[(size_t)r * W + r / 64] |= 1ull << (r % 64);
    for (int i = 0; i < a->n; i++)
        if (a->ins[i].ndef) { int s = a->nreg + i; int r = a->ins[i].def; kill[(size_t)r * W + s / 64] |= 1ull << (s % 64); }
    for (int r = 0; r < a->nreg; r++) in[r / 64] |= 1ull << (r % 64);   // IN[0] = entry defs
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < a->n; i++) {
            ains *x = &a->ins[i];
            bits *o = &out[(size_t)i * W], *iv = &in[(size_t)i * W];
            for (int k = 0; k < W; k++) {
                bits v = iv[k];
                if (x->ndef) v &= ~kill[(size_t)x->def * W + k];
                if (x->ndef && k == (a->nreg + i) / 64) v |= 1ull << ((a->nreg + i) % 64);
                if (v != o[k]) { o[k] = v; changed = true; }
            }
            // successors
            int succ[2], ns = 0;
            bool falls = x->op != R_JMP && x->op != R_RET && x->op != R_RETV && x->op != R_HALT;
            if (falls && i + 1 < a->n) succ[ns++] = i + 1;
            if (x->target >= 0) succ[ns++] = a->w2i[x->target];
            for (int s = 0; s < ns; s++) {
                if (succ[s] < 0) continue;
                bits *si = &in[(size_t)succ[s] * W];
                for (int k = 0; k < W; k++) {
                    bits v = si[k] | o[k];
                    if (v != si[k]) { si[k] = v; changed = true; }
                }
            }
        }
    }
    // Use webs: every definition of a register that reaches a use joins one web.
    for (int i = 0; i < a->n; i++) {
        ains *x = &a->ins[i];
        for (int k = 0; k < x->nuse; k++) {
            int r = x->use[k], first = -1;
            for (int s = 0; s < nd; s++) {
                if (!(in[(size_t)i * W + s / 64] >> (s % 64) & 1)) continue;
                if (!(kill[(size_t)r * W + s / 64] >> (s % 64) & 1)) continue;
                if (s < a->nreg && s >= a->f->arity) continue;    // an unset register links nothing
                int id = a->dbase + s;
                if (first < 0) first = id; else uni(aot_par, first, id);
            }
            x->useweb[k] = first < 0 ? a->dbase + r : first;
        }
        if (x->ndef) x->defweb = a->dbase + a->nreg + i;
    }
    // A STORE overwrites its target's old value, so they share a variable.
    for (int i = 0; i < a->n; i++) {
        ains *x = &a->ins[i];
        if ((x->op == R_STORE || x->op == R_STORE_L) && x->useweb[1] != a->dbase + x->def) uni(aot_par, x->defweb, x->useweb[1]);
    }
    free(in); free(out); free(kill);
}

#define ROOT(w) fnd(aot_par, (w))
#define UW(x, k) ((x)->useweb[k])

static void aot_constrain(sl_vm *vm, afn *afns, afn *a) {
    for (int i = 0; i < a->n; i++) {
        ains *x = &a->ins[i];
        int op = x->op;
        int D = x->defweb;
        const uint16_t *w = a->f->rcode + x->at + 1;
        #define DEFM(bits) do { if (x->ndef) cmask(D, bits); } while (0)
        #define USEM(k, bits) cmask(UW(x, k), bits)
        switch (op) {
            case R_LDI: case R_LDC_I: DEFM(MI); break;
            case R_LOADG: DEFM(MA); break;
            case R_LDC_F: DEFM(MF); break;
            case R_LDB: DEFM(MB); break;
            case R_LDNULL: case R_LDS: DEFM(MO); break;
            case R_ADD_I: case R_SUB_I: case R_MUL_I: case R_DIV_I: case R_REM_I:
                DEFM(MI); USEM(0, MI); USEM(1, MI); break;
            case R_ADDI: DEFM(MI); USEM(0, MI); break;
            case R_AJLT: DEFM(MI); USEM(0, MI); USEM(1, MI); break;
            case R_AJLTI: DEFM(MI); USEM(0, MI); break;
            case R_JLTLEN: USEM(0, MI); USEM(1, MO); break;
            case R_NEG_I: DEFM(MI); USEM(0, MI); break;
            case R_ADD_F: case R_SUB_F: case R_MUL_F: case R_DIV_F: case R_REM_F:
                DEFM(MF); USEM(0, MF); USEM(1, MF); break;
            case R_NEG_F: DEFM(MF); USEM(0, MF); break;
            case R_EQ_I: case R_NE_I: case R_LT_I: case R_LE_I: case R_GT_I: case R_GE_I:
                DEFM(MB); USEM(0, MI); USEM(1, MI); break;
            case R_EQ_F: case R_NE_F: case R_LT_F: case R_LE_F: case R_GT_F: case R_GE_F:
                DEFM(MB); USEM(0, MF); USEM(1, MF); break;
            case R_EQ_B: case R_NE_B: DEFM(MB); USEM(0, MB); USEM(1, MB); break;
            case R_NOT: DEFM(MB); USEM(0, MB); break;
            case R_EQ_S: case R_NE_S: DEFM(MB); USEM(0, MO); USEM(1, MO); break;
            case R_CONCAT: DEFM(MO); USEM(0, MO); USEM(1, MO); break;
            case R_STR_I: DEFM(MO); USEM(0, MI); break;
            case R_STR_F: DEFM(MO); USEM(0, MF); break;
            case R_STR_B: DEFM(MO); USEM(0, MB); break;
            case R_STR_OBJ: DEFM(MO); USEM(0, MO); break;
            case R_JT: case R_JF: USEM(0, MB); break;
            case R_MOV: case R_MOVS: case R_STORE: case R_STORE_L: cuni(D, UW(x, 0)); break;
            case R_ALEN: case R_ALEN_L: DEFM(MI); USEM(0, MO); break;
            case R_VTAG: DEFM(MI); USEM(0, MO); break;
            case R_NEWSTRUCT: DEFM(MS); if (x->ndef) ctid(D, w[2]); break;
            case R_NEWARR: case R_NEWMAP: case R_NEWVAR: DEFM(MO); break;
            case R_COPY: if (w[2] != 0xFFFF) { DEFM(MS); ctid(D, w[2]); } else DEFM(MO); break;
            case R_AGET: case R_AGET_L: case R_AGET_LU: USEM(0, MO); USEM(1, MI); DEFM(MA); break;
            case R_ASET: case R_ASET_L: case R_ASET_LU: USEM(0, MO); USEM(1, MI); break;
            case R_APUSH: case R_APUSH_L: USEM(0, MO); break;
            case R_FGET: case R_FGET_L: USEM(0, MS); DEFM(MA); break;
            case R_FSET: case R_FSET_L: {
                USEM(0, MS);
                for (int r = 0; r < a->f->arity; r++)
                    if (ROOT(UW(x, 0)) == ROOT(a->dbase + r)) { USEM(0, MP); break; }
                break;
            }
            case R_VGET: USEM(0, MO); DEFM(MA); break;
            case R_SLICE: DEFM(MO); USEM(0, MO); USEM(1, MI); USEM(2, MI); break;
            case R_MGET: USEM(0, MO); DEFM(MA); break;
            case R_MSET: USEM(0, MO); break;
            case R_RET: cuni(a->retnode, UW(x, 0)); break;
            case R_CALL: {
                int gid = w[0];
                afn *g = &afns[gid];
                for (int k = 0; k < g->f->arity; k++) cuni(UW(x, k), ROOT(g->dbase + k));
                if (g->returns) cuni(D, g->retnode);
                break;
            }
            case R_NATIVE: if (x->ndef) DEFM(MA); break;
            default:
                if (op >= R_JC_EQ_I && op <= R_JC_GE_I) { USEM(0, MI); USEM(1, MI); }
                else if ((op >= R_JC_EQ_F && op <= R_JN_GE_F)) { USEM(0, MF); USEM(1, MF); }
                else if (op >= R_JCI_EQ && op <= R_JCI_GE) USEM(0, MI);
        }
        #undef DEFM
        #undef USEM
    }
    (void)vm;
}

// Once classes know their struct type, field accesses tell us the field types.
static void aot_field_kinds(afn *a) {
    for (int i = 0; i < a->n; i++) {
        ains *x = &a->ins[i];
        const uint16_t *w = a->f->rcode + x->at + 1;
        switch (x->op) {
            case R_NEWSTRUCT: {
                int tid = w[2];
                if (tid >= aot_vm->nstructs) break;
                for (int k = 0; k < x->nuse && k < aot_vm->struct_desc[tid][0]; k++) {
                    int fk = fkind(tid, k);
                    if (fk) cmask(UW(x, k), fk == 1 ? MI : fk == 2 ? MF : MB);
                }
                break;
            }
            case R_FGET: case R_FGET_L: {
                int tid = ctidof(UW(x, 0));
                if (tid >= 0 && tid < aot_vm->nstructs && w[2] < aot_vm->struct_desc[tid][0]) {
                    int fk = fkind(tid, w[2]);
                    if (fk) cmask(x->defweb, fk == 1 ? MI : fk == 2 ? MF : MB);
                }
                break;
            }
            case R_FSET: case R_FSET_L: {
                int tid = ctidof(UW(x, 0));
                if (tid >= 0 && tid < aot_vm->nstructs && w[1] < aot_vm->struct_desc[tid][0]) {
                    int fk = fkind(tid, w[1]);
                    if (fk) cmask(UW(x, 1), fk == 1 ? MI : fk == 2 ? MF : MB);
                }
                break;
            }
        }
    }
}

// ---- C text ---------------------------------------------------------------

static char aot_buf[16][200];
static int aot_bi;
static char *bufn(void) { aot_bi = (aot_bi + 1) & 15; return aot_buf[aot_bi]; }

static const char *cmp_c[6] = { "==", "!=", "<", "<=", ">", ">=" };

static char *vname(int web) { char *b = bufn(); snprintf(b, 200, "w%d", ROOT(web)); return b; }

static char *ev_i(int web) {
    char *b = bufn();
    switch (kind_of(web)) {
        case K_INT: snprintf(b, 200, "%s", vname(web)); break;
        case K_GEN: snprintf(b, 200, "%s.as.i", vname(web)); break;
        default: snprintf(b, 200, "(int64_t)%s", vname(web)); break;
    }
    return b;
}
static char *ev_f(int web) {
    char *b = bufn();
    if (kind_of(web) == K_GEN) snprintf(b, 200, "%s.as.f", vname(web)); else snprintf(b, 200, "%s", vname(web));
    return b;
}
static char *ev_b(int web) {
    char *b = bufn();
    if (kind_of(web) == K_GEN) snprintf(b, 200, "(%s.as.i != 0)", vname(web)); else snprintf(b, 200, "%s", vname(web));
    return b;
}
// an owned sl_value taken out of the web
static char *ev_take(int web) {
    char *b = bufn();
    switch (kind_of(web)) {
        case K_INT:  snprintf(b, 200, "sl_int(%s)", vname(web)); break;
        case K_FLT:  snprintf(b, 200, "sl_float(%s)", vname(web)); break;
        case K_BOOL: snprintf(b, 200, "sl_bool(%s)", vname(web)); break;
        case K_STRUCT: snprintf(b, 200, "S%d_box(vm, %s)", tid_of(web), vname(web)); break;
        default:     snprintf(b, 200, "aot_take(&%s)", vname(web)); break;
    }
    return b;
}
// the value without giving it up
static char *ev_peek(int web) {
    char *b = bufn();
    switch (kind_of(web)) {
        case K_INT:  snprintf(b, 200, "sl_int(%s)", vname(web)); break;
        case K_FLT:  snprintf(b, 200, "sl_float(%s)", vname(web)); break;
        case K_BOOL: snprintf(b, 200, "sl_bool(%s)", vname(web)); break;
        case K_STRUCT: snprintf(b, 200, "S%d_box(vm, %s)", tid_of(web), vname(web)); break;
        default:     snprintf(b, 200, "vload(&%s)", vname(web)); break;
    }
    return b;
}

static void set_i(FILE *o, int web, const char *e) {
    switch (kind_of(web)) {
        case K_INT: fprintf(o, "%s = %s;\n", vname(web), e); break;
        case K_GEN: fprintf(o, "set_int(&%s, %s);\n", vname(web), e); break;
        default: fprintf(o, "%s = (%s);\n", vname(web), e);
    }
}
static void set_f(FILE *o, int web, const char *e) {
    if (kind_of(web) == K_GEN) fprintf(o, "set_flt(&%s, %s);\n", vname(web), e); else fprintf(o, "%s = %s;\n", vname(web), e);
}
static void set_b(FILE *o, int web, const char *e) {
    if (kind_of(web) == K_GEN) fprintf(o, "set_bool(&%s, %s);\n", vname(web), e); else fprintf(o, "%s = %s;\n", vname(web), e);
}
// assign an sl_value expression to a web
static void set_v(FILE *o, int web, const char *e) {
    switch (kind_of(web)) {
        case K_INT:  fprintf(o, "%s = (%s).as.i;\n", vname(web), e); break;
        case K_FLT:  fprintf(o, "%s = (%s).as.f;\n", vname(web), e); break;
        case K_BOOL: fprintf(o, "%s = (%s).as.i != 0;\n", vname(web), e); break;
        default:     fprintf(o, "vstore(&%s, %s);\n", vname(web), e);
    }
}
static const int *aot_from;
static char *base_val(const uint8_t *alias, int i, int web) {
    char *b = bufn();
    if (alias[i] == 1) snprintf(b, 200, "ali%d", aot_from[i]); else snprintf(b, 200, "vload(&%s)", vname(web));
    return b;
}
static void drop_web(FILE *o, int web) { if (kind_of(web) == K_GEN) fprintf(o, "drop(vm, &%s);\n", vname(web)); }

static const char *ctype(akind k) {
    switch (k) { case K_INT: return "int64_t"; case K_FLT: return "double"; case K_BOOL: return "bool"; default: return "sl_value"; }
}
static char *ctype_w(int web) {
    char *b = bufn();
    akind k = kind_of(web);
    if (k == K_STRUCT) snprintf(b, 200, "S%d", tid_of(web)); else snprintf(b, 200, "%s", ctype(k));
    return b;
}

typedef struct { int web; } seenweb;

static void aot_emit_fn(sl_vm *vm, afn *afns, int fid, FILE *o) {
    afn *a = &afns[fid];
    sl_func *f = a->f;
    // webs used by this function
    int *seen = (int *)malloc((size_t)(a->nreg + a->n + 1) * sizeof(int));
    int nseen = 0;
    #define NOTE(wb) do { int r_ = ROOT(wb); bool f_ = false; for (int q = 0; q < nseen; q++) if (seen[q] == r_) { f_ = true; break; } if (!f_) seen[nseen++] = r_; } while (0)
    for (int r = 0; r < a->nreg; r++) if (r < f->arity) NOTE(a->dbase + r);
    for (int i = 0; i < a->n; i++) {
        ains *x = &a->ins[i];
        for (int k = 0; k < x->nuse; k++) NOTE(x->useweb[k]);
        if (x->ndef) NOTE(x->defweb);
    }
    akind rk = a->returns ? kind_of(a->retnode) : K_GEN;
    fprintf(o, "static %s f%d(sl_vm *vm", a->returns ? ctype_w(a->retnode) : "void", fid);
    for (int k = 0; k < f->arity; k++) fprintf(o, ", %s a%d", ctype_w(a->dbase + k), k);
    fprintf(o, ") {\n");
    for (int q = 0; q < nseen; q++) {
        akind k = kind_of(seen[q]);
        if (k == K_GEN) fprintf(o, "    sl_value w%d = { SL_VOID, { 0 } };\n", seen[q]);
        else if (k == K_STRUCT) fprintf(o, "    %s w%d; memset(&w%d, 0, sizeof w%d);\n", ctype_w(seen[q]), seen[q], seen[q], seen[q]);
        else fprintf(o, "    %s w%d = 0;\n", ctype(k), seen[q]);
    }
    if (a->recursive) fprintf(o, "    if (AOT_UNLIKELY(++vm->nframes > SL_FRAMES_MAX)) AOT_FAIL(\"call depth exceeded %%d (infinite recursion?)\", SL_FRAMES_MAX);\n");
    for (int k = 0; k < f->arity; k++) fprintf(o, "    w%d = a%d;\n", ROOT(a->dbase + k), k);
    // label every jump target
    uint8_t *lab = (uint8_t *)calloc((size_t)a->n + 1, 1);
    for (int i = 0; i < a->n; i++) if (a->ins[i].target >= 0) { int t = a->w2i[a->ins[i].target]; if (t >= 0) lab[t] = 1; }

    // A borrowed element read (arr[i], s.f) that feeds straight into the next
    // instruction does not need a reference of its own: the container is alive
    // and nothing runs in between. alias[i] == 1: instruction i reads its base
    // from instruction i-1's alias variable; == 2: it produces that alias.
    uint8_t *alias = (uint8_t *)calloc((size_t)a->n + 1, 1);
    int *aliasfrom = (int *)calloc((size_t)a->n + 1, sizeof(int));
    {
        int *nuses = (int *)calloc((size_t)(a->nreg + a->n + 1), sizeof(int));
        for (int i = 0; i < a->n; i++)
            for (int k = 0; k < a->ins[i].nuse; k++) nuses[ROOT(a->ins[i].useweb[k]) - a->dbase < 0 ? 0 : ROOT(a->ins[i].useweb[k]) - a->dbase]++;
        for (int i = 0; i + 1 < a->n; i++) {
            ains *p = &a->ins[i];
            if (!(p->op == R_AGET_L || p->op == R_AGET_LU || p->op == R_FGET_L || p->op == R_MOV) || kind_of(p->defweb) != K_GEN) continue;
            if (p->op == R_MOV && kind_of(p->useweb[0]) != K_GEN) continue;
            // the consumer: the next instruction, or a few scalar-only ones later
            int j = i + 1;
            for (; j < a->n && j <= i + 4; j++) {
                ains *m = &a->ins[j];
                if (lab[j]) { j = a->n; break; }
                bool scalar_only = (m->op == R_LDI || m->op == R_LDC_I || m->op == R_LDC_F || m->op == R_LDB ||
                                    m->op == R_ADDI || (m->op >= R_ADD_I && m->op <= R_NEG_F) ||
                                    ((m->op == R_MOV || m->op == R_MOVS) && kind_of(m->defweb) != K_GEN));
                bool eligible = p->op == R_MOV ? (m->op == R_MGET || m->op == R_MSET)
                                               : (m->op == R_AGET || m->op == R_FGET || m->op == R_ALEN || m->op == R_COPY);
                if (eligible && m->nuse > 0 && ROOT(m->useweb[0]) == ROOT(p->defweb)) break;
                if (!scalar_only) { j = a->n; break; }
                // an intervening instruction must not touch the producer's variable
                bool touches = false;
                for (int k = 0; k < m->nuse; k++) if (ROOT(m->useweb[k]) == ROOT(p->defweb)) touches = true;
                if (m->ndef && ROOT(m->defweb) == ROOT(p->defweb)) touches = true;
                if (touches) { j = a->n; break; }
            }
            if (j >= a->n || j > i + 4) continue;
            ains *c = &a->ins[j];
            if (nuses[ROOT(p->defweb) - a->dbase] != 1) continue;
            if (c->op == R_COPY && kind_of(c->defweb) != K_STRUCT) continue;
            if (alias[i] || alias[j]) continue;
            alias[i] = 2; alias[j] = 1; aliasfrom[j] = i;
        }
        free(nuses);
    }
    aot_from = aliasfrom;
    for (int i = 0; i < a->n; i++) if (alias[i] == 2) fprintf(o, "    sl_value ali%d = { SL_VOID, { 0 } };\n", i);

    // text of "leave the function"
    char exitbuf[8192]; snprintf(exitbuf, sizeof exitbuf, "%s", a->recursive ? "vm->nframes--; " : "");
    for (int q = 0; q < nseen; q++)
        if (kind_of(seen[q]) == K_GEN) { char t[64]; snprintf(t, sizeof t, "drop(vm, &w%d); ", seen[q]); strcat(exitbuf, t); }

    for (int i = 0; i < a->n; i++) {
        ains *x = &a->ins[i];
        const uint16_t *w = f->rcode + x->at + 1;
        int D = x->defweb;
        if (lab[i]) fprintf(o, "L%d:;\n", i);
        fprintf(o, "    ");
        switch (x->op) {
            case R_LDI: { char e[40]; snprintf(e, sizeof e, "%d", (int)(int16_t)w[1]); set_i(o, D, e); break; }
            case R_LDC_I: {
                uint64_t u = (uint64_t)w[1] | ((uint64_t)w[2] << 16) | ((uint64_t)w[3] << 32) | ((uint64_t)w[4] << 48);
                char e[64]; snprintf(e, sizeof e, "(int64_t)%" PRIu64 "ULL", u); set_i(o, D, e); break;
            }
            case R_LDC_F: {
                uint64_t u = (uint64_t)w[1] | ((uint64_t)w[2] << 16) | ((uint64_t)w[3] << 32) | ((uint64_t)w[4] << 48);
                char e[64]; snprintf(e, sizeof e, "aot_f64(0x%" PRIx64 "ULL)", u); set_f(o, D, e); break;
            }
            case R_LDS: fprintf(o, "vstore(&%s, const_str(vm, %u));\n", vname(D), w[1]); break;
            case R_LDB: set_b(o, D, w[1] ? "1" : "0"); break;
            case R_LDNULL: fprintf(o, "{ sl_value v; v.type = SL_OBJ; v.as.o = NULL; vstore(&%s, v); }\n", vname(D)); break;
            case R_MOV: {
                if (alias[i] == 2) { fprintf(o, "ali%d = vload(&%s);\n", i, vname(UW(x, 0))); break; }
                if (kind_of(D) == K_GEN) fprintf(o, "{ sl_value v = vload(&%s); sl_retain(v); vstore(&%s, v); }\n", vname(UW(x, 0)), vname(D));
                else fprintf(o, "%s = %s;\n", vname(D), vname(UW(x, 0)));
                break;
            }
            case R_MOVS:
                if (kind_of(D) == K_GEN) fprintf(o, "vstore(&%s, vload(&%s));\n", vname(D), vname(UW(x, 0)));
                else fprintf(o, "%s = %s;\n", vname(D), vname(UW(x, 0)));
                break;
            case R_STORE:
                if (kind_of(D) == K_GEN) fprintf(o, "{ sl_value old = vload(&%s); vstore(&%s, vload(&%s)); moved(&%s); rel(vm, old); }\n", vname(D), vname(D), vname(UW(x, 0)), vname(UW(x, 0)));
                else fprintf(o, "%s = %s;\n", vname(D), vname(UW(x, 0)));
                break;
            case R_STORE_L:
                if (kind_of(D) == K_GEN) fprintf(o, "{ sl_value v = vload(&%s); sl_retain(v); sl_value old = vload(&%s); vstore(&%s, v); rel(vm, old); }\n", vname(UW(x, 0)), vname(D), vname(D));
                else fprintf(o, "%s = %s;\n", vname(D), vname(UW(x, 0)));
                break;
            case R_DROP: drop_web(o, UW(x, 0)); if (kind_of(UW(x, 0)) != K_GEN) fprintf(o, ";\n"); break;
            case R_ADD_I: { char e[420]; snprintf(e, sizeof e, "%s + %s", ev_i(UW(x, 0)), ev_i(UW(x, 1))); set_i(o, D, e); break; }
            case R_SUB_I: { char e[420]; snprintf(e, sizeof e, "%s - %s", ev_i(UW(x, 0)), ev_i(UW(x, 1))); set_i(o, D, e); break; }
            case R_MUL_I: { char e[420]; snprintf(e, sizeof e, "%s * %s", ev_i(UW(x, 0)), ev_i(UW(x, 1))); set_i(o, D, e); break; }
            case R_DIV_I: case R_REM_I: {
                char e[420]; const char *ds = x->op == R_DIV_I ? "/" : "%";
                fprintf(o, "{ int64_t b_ = %s; if (AOT_UNLIKELY(b_ == 0)) AOT_FAIL(\"%s\"); ", ev_i(UW(x, 1)), x->op == R_DIV_I ? "division by zero" : "remainder by zero");
                snprintf(e, sizeof e, "%s %s b_", ev_i(UW(x, 0)), ds); set_i(o, D, e); fprintf(o, "    }\n");
                break;
            }
            case R_ADDI: { char e[420]; snprintf(e, sizeof e, "%s + %d", ev_i(UW(x, 0)), (int)(int16_t)w[2]); set_i(o, D, e); break; }
            case R_NEG_I: { char e[420]; snprintf(e, sizeof e, "-%s", ev_i(UW(x, 0))); set_i(o, D, e); break; }
            case R_JLTLEN: fprintf(o, "if (%s < aot_len(vload(&%s))) goto L%d;\n", ev_i(UW(x, 0)), vname(UW(x, 1)), a->w2i[x->target]); break;
            case R_AJLT: case R_AJLTI: {
                char e[420]; snprintf(e, sizeof e, "%s + %d", ev_i(UW(x, 0)), (int)(int16_t)w[2]); set_i(o, D, e);
                if (x->op == R_AJLT) fprintf(o, "    if (%s < %s) goto L%d;\n", ev_i(D), ev_i(UW(x, 1)), a->w2i[x->target]);
                else fprintf(o, "    if (%s < (int64_t)%d) goto L%d;\n", ev_i(D), (int)(int16_t)w[1], a->w2i[x->target]);
                break;
            }
            case R_ADD_F: case R_SUB_F: case R_MUL_F: case R_DIV_F: {
                const char *os = x->op == R_ADD_F ? "+" : x->op == R_SUB_F ? "-" : x->op == R_MUL_F ? "*" : "/";
                char e[420]; snprintf(e, sizeof e, "%s %s %s", ev_f(UW(x, 0)), os, ev_f(UW(x, 1))); set_f(o, D, e); break;
            }
            case R_REM_F: { char e[420]; snprintf(e, sizeof e, "fmod(%s, %s)", ev_f(UW(x, 0)), ev_f(UW(x, 1))); set_f(o, D, e); break; }
            case R_NEG_F: { char e[420]; snprintf(e, sizeof e, "-%s", ev_f(UW(x, 0))); set_f(o, D, e); break; }
            case R_EQ_B: case R_NE_B: { char e[420]; snprintf(e, sizeof e, "%s %s %s", ev_b(UW(x, 0)), x->op == R_EQ_B ? "==" : "!=", ev_b(UW(x, 1))); set_b(o, D, e); break; }
            case R_NOT: { char e[420]; snprintf(e, sizeof e, "!%s", ev_b(UW(x, 0))); set_b(o, D, e); break; }
            case R_EQ_S: case R_NE_S: {
                char e[420]; snprintf(e, sizeof e, "%saot_streq(vm, %s, %s)", x->op == R_NE_S ? "!" : "", ev_take(UW(x, 0)), ev_take(UW(x, 1))); set_b(o, D, e); break;
            }
            case R_CONCAT: fprintf(o, "vstore(&%s, aot_concat(vm, %s, %s));\n", vname(D), ev_take(UW(x, 0)), ev_take(UW(x, 1))); break;
            case R_STR_I: fprintf(o, "vstore(&%s, aot_str_i(vm, %s));\n", vname(D), ev_i(UW(x, 0))); break;
            case R_STR_F: fprintf(o, "vstore(&%s, aot_str_f(vm, %s));\n", vname(D), ev_f(UW(x, 0))); break;
            case R_STR_B: fprintf(o, "vstore(&%s, aot_str_b(vm, %s));\n", vname(D), ev_b(UW(x, 0))); break;
            case R_STR_OBJ: fprintf(o, "{ sl_value v_ = %s; sl_value s = sl_to_string(vm, v_); rel(vm, v_); vstore(&%s, s); }\n", ev_take(UW(x, 0)), vname(D)); break;
            case R_JMP: fprintf(o, "goto L%d;\n", a->w2i[x->target]); break;
            case R_JT: fprintf(o, "if (%s) goto L%d;\n", ev_b(UW(x, 0)), a->w2i[x->target]); break;
            case R_JF: fprintf(o, "if (!%s) goto L%d;\n", ev_b(UW(x, 0)), a->w2i[x->target]); break;
            case R_CALL: {
                int gid = w[0];
                afn *g = &afns[gid];
                char args[2048]; args[0] = 0;
                for (int k = 0; k < g->f->arity; k++) {
                    char t[240];
                    akind pk = kind_of(g->dbase + k);
                    if (pk == K_GEN) snprintf(t, sizeof t, ", aot_take(&%s)", vname(UW(x, k)));
                    else snprintf(t, sizeof t, ", %s", pk == K_INT ? ev_i(UW(x, k)) : pk == K_FLT ? ev_f(UW(x, k)) : pk == K_BOOL ? ev_b(UW(x, k)) : vname(UW(x, k)));
                    strcat(args, t);
                }
                if (g->returns) {
                    if (kind_of(D) == K_GEN) fprintf(o, "vstore(&%s, f%d(vm%s));\n", vname(D), gid, args);
                    else fprintf(o, "%s = f%d(vm%s);\n", vname(D), gid, args);
                } else fprintf(o, "f%d(vm%s);\n", gid, args);
                break;
            }
            case R_NATIVE: {
                int argc = w[2];
                fprintf(o, "{ sl_value argv[%d];", argc ? argc : 1);
                for (int k = 0; k < argc; k++) fprintf(o, " argv[%d] = %s;", k, ev_take(UW(x, k)));
                fprintf(o, " sl_value res = aot_native(vm, %u, %d, argv); ", w[0], argc);
                if (w[3]) set_v(o, D, "res"); else fprintf(o, "rel(vm, res); ");
                fprintf(o, "}\n");
                break;
            }
            case R_RET: {
                akind k = kind_of(UW(x, 0));
                if (k == K_GEN) fprintf(o, "{ sl_value ret_ = aot_take(&%s); %s return ret_; }\n", vname(UW(x, 0)), exitbuf);
                else fprintf(o, "{ %s ret_ = %s; %s return ret_; }\n", ctype_w(UW(x, 0)), vname(UW(x, 0)), exitbuf);
                break;
            }
            case R_RETV: case R_HALT: {
                // A function that returns a value still ends in RET_VOID; that path never runs.
                char tail[200];
                if (!a->returns) snprintf(tail, sizeof tail, "return;");
                else if (rk == K_GEN) snprintf(tail, sizeof tail, "return (sl_value){ SL_VOID, { 0 } };");
                else if (rk == K_STRUCT) snprintf(tail, sizeof tail, "{ %s z_; memset(&z_, 0, sizeof z_); return z_; }", ctype_w(a->retnode));
                else snprintf(tail, sizeof tail, "return 0;");
                fprintf(o, "{ %s %s }\n", exitbuf, tail);
                break;
            }
            case R_NEWARR: {
                int n = w[1];
                fprintf(o, "{ sl_value av = sl_array(vm, %d); sl_arr *A = (sl_arr *)av.as.o;", n);
                for (int k = 0; k < n; k++) fprintf(o, " array_push(vm, A, %s);", ev_take(UW(x, k)));
                fprintf(o, " vstore(&%s, av); }\n", vname(D));
                break;
            }
            case R_AGET: case R_AGET_L: case R_AGET_LU: {
                bool owned = x->op == R_AGET && alias[i] != 1;
                bool unchecked = x->op == R_AGET_LU;
                akind dk = kind_of(D);
                if (alias[i] == 2) {
                    if (unchecked) fprintf(o, "{ sl_arr *A = (sl_arr *)%s.as.o; int64_t I = %s; ali%d = arr_get(A, I); }\n", base_val(alias, i, UW(x, 0)), ev_i(UW(x, 1)), i);
                    else fprintf(o, "{ sl_arr *A = aot_arr(vm, %s); int64_t I = aot_index(vm, A, %s); ali%d = arr_get(A, I); }\n", base_val(alias, i, UW(x, 0)), ev_i(UW(x, 1)), i);
                    break;
                }
                if (unchecked) fprintf(o, "{ sl_arr *A = (sl_arr *)%s.as.o; int64_t I = %s; ", base_val(alias, i, UW(x, 0)), ev_i(UW(x, 1)));
                else fprintf(o, "{ sl_arr *A = aot_arr(vm, %s); int64_t I = aot_index(vm, A, %s); ", base_val(alias, i, UW(x, 0)), ev_i(UW(x, 1)));
                if (dk == K_INT) fprintf(o, "int64_t e = ((int64_t *)A->data)[I]; ");
                else if (dk == K_FLT) fprintf(o, "double e = ((double *)A->data)[I]; ");
                else if (dk == K_BOOL) fprintf(o, "bool e = ((uint8_t *)A->data)[I]; ");
                else fprintf(o, "sl_value e = arr_get(A, I); sl_retain(e); ");
                if (owned) fprintf(o, "drop(vm, &%s); ", vname(UW(x, 0)));
                if (dk == K_GEN) set_v(o, D, "e"); else fprintf(o, "%s = e;\n", vname(D));
                fprintf(o, "    }\n");
                break;
            }
            case R_ASET: case R_ASET_L: case R_ASET_LU: {
                bool owned = x->op == R_ASET;
                akind vk = kind_of(UW(x, 2));
                if (x->op == R_ASET_LU) fprintf(o, "{ sl_arr *A = (sl_arr *)%s.as.o; int64_t I = %s; ", vname(UW(x, 0)), ev_i(UW(x, 1)));
                else fprintf(o, "{ sl_arr *A = aot_arr(vm, vload(&%s)); int64_t I = aot_index(vm, A, %s); ", vname(UW(x, 0)), ev_i(UW(x, 1)));
                if (vk == K_STRUCT) fprintf(o, "{ sl_value old = arr_get(A, I); if (old.type == SL_OBJ && old.as.o && old.as.o->rc == 1) S%d_store((sl_struct *)old.as.o, %s); else { vstore(&((sl_value *)A->data)[I], S%d_box(vm, %s)); rel(vm, old); } } ", tid_of(UW(x, 2)), vname(UW(x, 2)), tid_of(UW(x, 2)), vname(UW(x, 2)));
                else if (vk == K_INT) fprintf(o, "((int64_t *)A->data)[I] = %s; ", vname(UW(x, 2)));
                else if (vk == K_FLT) fprintf(o, "((double *)A->data)[I] = %s; ", vname(UW(x, 2)));
                else if (vk == K_BOOL) fprintf(o, "((uint8_t *)A->data)[I] = %s; ", vname(UW(x, 2)));
                else fprintf(o, "sl_value nv = aot_take(&%s); if (A->ek != AK_GEN && A->ek != ak_of(nv)) arr_generify(vm, A); sl_value old = arr_get(A, I); arr_put(A, I, nv); rel(vm, old); ", vname(UW(x, 2)));
                if (owned) fprintf(o, "drop(vm, &%s); ", vname(UW(x, 0)));
                fprintf(o, "}\n");
                break;
            }
            case R_ALEN: case R_ALEN_L: {
                bool owned = x->op == R_ALEN && alias[i] != 1;
                fprintf(o, "{ int64_t n_ = aot_len(%s); ", base_val(alias, i, UW(x, 0)));
                if (owned) fprintf(o, "drop(vm, &%s); ", vname(UW(x, 0)));
                set_i(o, D, "n_"); fprintf(o, "    }\n");
                break;
            }
            case R_APUSH: case R_APUSH_L: {
                bool owned = x->op == R_APUSH;
                {
                    akind vk = kind_of(UW(x, 1));
                    fprintf(o, "{ sl_arr *A = aot_arr(vm, vload(&%s)); ", vname(UW(x, 0)));
                    if (vk == K_INT) fprintf(o, "if (AOT_LIKELY(A->ek == AK_INT && A->len < A->cap)) ((int64_t *)A->data)[A->len++] = %s; else if (!array_push(vm, A, sl_int(%s))) longjmp(aot_jb, 1); ", vname(UW(x, 1)), vname(UW(x, 1)));
                    else if (vk == K_FLT) fprintf(o, "if (AOT_LIKELY(A->ek == AK_FLT && A->len < A->cap)) ((double *)A->data)[A->len++] = %s; else if (!array_push(vm, A, sl_float(%s))) longjmp(aot_jb, 1); ", vname(UW(x, 1)), vname(UW(x, 1)));
                    else if (vk == K_BOOL) fprintf(o, "if (AOT_LIKELY(A->ek == AK_BOOL && A->len < A->cap)) ((uint8_t *)A->data)[A->len++] = %s; else if (!array_push(vm, A, sl_bool(%s))) longjmp(aot_jb, 1); ", vname(UW(x, 1)), vname(UW(x, 1)));
                    else fprintf(o, "if (!array_push(vm, A, %s)) longjmp(aot_jb, 1); ", ev_take(UW(x, 1)));
                }
                if (owned) fprintf(o, "drop(vm, &%s); ", vname(UW(x, 0)));
                fprintf(o, "}\n");
                break;
            }
            case R_SLICE: fprintf(o, "vstore(&%s, aot_slice(vm, aot_take(&%s), %s, %s));\n", vname(D), vname(UW(x, 0)), ev_i(UW(x, 1)), ev_i(UW(x, 2))); break;
            case R_NEWSTRUCT: if (kind_of(D) == K_STRUCT) {
                int tid = w[2], n = w[1];
                fprintf(o, "%s = (S%d){", vname(D), tid);
                for (int k = 0; k < n; k++) {
                    int fk = fkind(tid, k);
                    fprintf(o, "%s%s", k ? ", " : "", fk == 1 ? ev_i(UW(x, k)) : fk == 2 ? ev_f(UW(x, k)) : ev_b(UW(x, k)));
                }
                fprintf(o, "};\n");
                break;
            }
            /* fall through */
            case R_NEWVAR: case R_NEWMAP: {
                int n = x->op == R_NEWVAR ? w[2] : w[1];
                int cnt = x->op == R_NEWMAP ? 2 * n : n;
                fprintf(o, "{ sl_value it[%d];", cnt ? cnt : 1);
                for (int k = 0; k < cnt; k++) fprintf(o, " it[%d] = %s;", k, ev_take(UW(x, k)));
                if (x->op == R_NEWSTRUCT) fprintf(o, " vstore(&%s, aot_newstruct(vm, %d, it)); }\n", vname(D), n);
                else if (x->op == R_NEWVAR) fprintf(o, " vstore(&%s, aot_newvar(vm, %u, %d, it)); }\n", vname(D), w[1], n);
                else fprintf(o, " vstore(&%s, aot_newmap(vm, %d, it)); }\n", vname(D), n);
                break;
            }
            case R_FGET: case R_FGET_L: {
                bool owned = x->op == R_FGET && alias[i] != 1;
                if (alias[i] == 2) {
                    fprintf(o, "{ sl_struct *S = aot_st(vm, %s); ali%d = vload(&S->fields[%u]); }\n", base_val(alias, i, UW(x, 0)), i, w[2]);
                    break;
                }
                if (kind_of(UW(x, 0)) == K_STRUCT) {
                    char e[200]; int fk = fkind(tid_of(UW(x, 0)), w[2]);
                    snprintf(e, sizeof e, "%s.f%u", vname(UW(x, 0)), w[2]);
                    if (fk == 1) set_i(o, D, e); else if (fk == 2) set_f(o, D, e); else set_b(o, D, e);
                    break;
                }
                fprintf(o, "{ sl_struct *S = aot_st(vm, %s); sl_value f_ = vload(&S->fields[%u]); ", base_val(alias, i, UW(x, 0)), w[2]);
                if (kind_of(D) == K_GEN) fprintf(o, "sl_retain(f_); ");
                if (owned) fprintf(o, "drop(vm, &%s); ", vname(UW(x, 0)));
                set_v(o, D, "f_"); fprintf(o, "    }\n");
                break;
            }
            case R_FSET: case R_FSET_L: {
                bool owned = x->op == R_FSET;
                akind vk = kind_of(UW(x, 1));
                if (kind_of(UW(x, 0)) == K_STRUCT) {
                    int fk = fkind(tid_of(UW(x, 0)), w[1]);
                    fprintf(o, "%s.f%u = %s;\n", vname(UW(x, 0)), w[1], fk == 1 ? ev_i(UW(x, 1)) : fk == 2 ? ev_f(UW(x, 1)) : ev_b(UW(x, 1)));
                    break;
                }
                fprintf(o, "{ sl_struct *S = aot_st(vm, vload(&%s)); ", vname(UW(x, 0)));
                if (vk == K_GEN) fprintf(o, "sl_value old = vload(&S->fields[%u]); vstore(&S->fields[%u], aot_take(&%s)); rel(vm, old); ", w[1], w[1], vname(UW(x, 1)));
                else fprintf(o, "vstore(&S->fields[%u], %s); ", w[1], ev_peek(UW(x, 1)));
                if (owned) fprintf(o, "drop(vm, &%s); ", vname(UW(x, 0)));
                fprintf(o, "}\n");
                break;
            }
            case R_VTAG: { char e[300]; snprintf(e, sizeof e, "aot_vtag(vm, aot_take(&%s))", vname(UW(x, 0))); set_i(o, D, e); break; }
            case R_VGET: { char e[300]; snprintf(e, sizeof e, "aot_vget(vm, aot_take(&%s), %u)", vname(UW(x, 0)), w[2]); set_v(o, D, e); break; }
            case R_LOADG: { char e[200]; snprintf(e, sizeof e, "aot_global(vm, %u)", w[1]); set_v(o, D, e); break; }
            case R_STOREG: fprintf(o, "{ sl_value old = vload(&vm->globals[%u]); vstore(&vm->globals[%u], %s); rel(vm, old); }\n", w[0], w[0], ev_take(UW(x, 0))); break;
            case R_COPY: {
                akind dk = kind_of(D), sk = kind_of(UW(x, 0));
                if (dk == K_STRUCT && sk == K_STRUCT) fprintf(o, "%s = %s;\n", vname(D), vname(UW(x, 0)));
                else if (dk == K_STRUCT && alias[i] == 1) fprintf(o, "%s = S%d_load(aot_st(vm, ali%d));\n", vname(D), tid_of(D), aliasfrom[i]);
                else if (dk == K_STRUCT) fprintf(o, "{ sl_value v_ = vload(&%s); %s = S%d_load(aot_st(vm, v_)); drop(vm, &%s); }\n", vname(UW(x, 0)), vname(D), tid_of(D), vname(UW(x, 0)));
                else if (sk == K_STRUCT) fprintf(o, "vstore(&%s, S%d_box(vm, %s));\n", vname(D), tid_of(UW(x, 0)), vname(UW(x, 0)));
                else fprintf(o, "{ sl_value c = struct_copy(vm, vload(&%s)); drop(vm, &%s); vstore(&%s, c); }\n", vname(UW(x, 0)), vname(UW(x, 0)), vname(D));
                break;
            }
            case R_MGET: {
                char e[300];
                if (alias[i] == 1) snprintf(e, sizeof e, "aot_mget_b(vm, ali%d, %s)", aliasfrom[i], ev_take(UW(x, 1)));
                else snprintf(e, sizeof e, "aot_mget(vm, aot_take(&%s), %s)", vname(UW(x, 0)), ev_take(UW(x, 1)));
                set_v(o, D, e); break;
            }
            case R_MSET:
                if (alias[i] == 1) fprintf(o, "aot_mset_b(vm, ali%d, %s, %s);\n", aliasfrom[i], ev_take(UW(x, 1)), ev_take(UW(x, 2)));
                else fprintf(o, "aot_mset(vm, aot_take(&%s), %s, %s);\n", vname(UW(x, 0)), ev_take(UW(x, 1)), ev_take(UW(x, 2)));
                break;
            default:
                if (x->op >= R_EQ_I && x->op <= R_GE_I) {
                    char e[420]; snprintf(e, sizeof e, "%s %s %s", ev_i(UW(x, 0)), cmp_c[x->op - R_EQ_I], ev_i(UW(x, 1))); set_b(o, D, e);
                } else if (x->op >= R_EQ_F && x->op <= R_GE_F) {
                    char e[420]; snprintf(e, sizeof e, "%s %s %s", ev_f(UW(x, 0)), cmp_c[x->op - R_EQ_F], ev_f(UW(x, 1))); set_b(o, D, e);
                } else if (x->op >= R_JC_EQ_I && x->op <= R_JC_GE_I) {
                    fprintf(o, "if (%s %s %s) goto L%d;\n", ev_i(UW(x, 0)), cmp_c[x->op - R_JC_EQ_I], ev_i(UW(x, 1)), a->w2i[x->target]);
                } else if (x->op >= R_JC_EQ_F && x->op <= R_JC_GE_F) {
                    fprintf(o, "if (%s %s %s) goto L%d;\n", ev_f(UW(x, 0)), cmp_c[x->op - R_JC_EQ_F], ev_f(UW(x, 1)), a->w2i[x->target]);
                } else if (x->op >= R_JN_EQ_F && x->op <= R_JN_GE_F) {
                    fprintf(o, "if (!(%s %s %s)) goto L%d;\n", ev_f(UW(x, 0)), cmp_c[x->op - R_JN_EQ_F], ev_f(UW(x, 1)), a->w2i[x->target]);
                } else if (x->op >= R_JCI_EQ && x->op <= R_JCI_GE) {
                    fprintf(o, "if (%s %s (int64_t)%d) goto L%d;\n", ev_i(UW(x, 0)), cmp_c[x->op - R_JCI_EQ], (int)(int16_t)w[1], a->w2i[x->target]);
                } else {
                    fprintf(o, "AOT_FAIL(\"aot: unhandled op %d\");\n", x->op);
                }
        }
    }
    // falling off the end (should not happen: every function ends in RET)
    {
        char tail[200];
        if (!a->returns) snprintf(tail, sizeof tail, "return;");
        else if (rk == K_GEN) snprintf(tail, sizeof tail, "return (sl_value){ SL_VOID, { 0 } };");
        else if (rk == K_STRUCT) snprintf(tail, sizeof tail, "{ %s z_; memset(&z_, 0, sizeof z_); return z_; }", ctype_w(a->retnode));
        else snprintf(tail, sizeof tail, "return 0;");
        fprintf(o, "    %s%s\n}\n\n", a->recursive ? "vm->nframes--; " : "", tail);
    }
    free(seen); free(lab);
}

int sl_emit_c(sl_vm *vm, const uint8_t *img, size_t len, FILE *o) {
    int nf = vm->nfuncs;
    afn *afns = (afn *)calloc((size_t)nf, sizeof(afn));
    int next = 0;
    for (int i = 0; i < nf; i++) {
        afns[i].f = &vm->funcs[i];
        aot_decode(vm, &afns[i]);
        aot_webs(&afns[i], &next);
        for (int k = 0; k < afns[i].n; k++) if (afns[i].ins[k].op == R_RET) afns[i].returns = true;
    }
    for (int i = 0; i < nf; i++) { afns[i].retnode = next++; }
    aot_par = (int *)malloc((size_t)next * sizeof(int));
    aot_cpar = (int *)malloc((size_t)next * sizeof(int));
    aot_mask = (uint8_t *)calloc((size_t)next, 1);
    aot_stid = (int *)malloc((size_t)next * sizeof(int));
    aot_vm = vm;
    for (int i = 0; i < next; i++) { aot_par[i] = aot_cpar[i] = i; aot_stid[i] = -1; }
    // a function is recursive if it can reach itself through calls
    {
        bool *reach = (bool *)calloc((size_t)nf * nf, sizeof(bool));
        for (int i = 0; i < nf; i++)
            for (int k = 0; k < afns[i].n; k++)
                if (afns[i].ins[k].op == R_CALL) reach[(size_t)i * nf + (afns[i].f->rcode[afns[i].ins[k].at + 1])] = true;
        for (int m = 0; m < nf; m++)
            for (int i = 0; i < nf; i++)
                if (reach[(size_t)i * nf + m])
                    for (int j = 0; j < nf; j++) if (reach[(size_t)m * nf + j]) reach[(size_t)i * nf + j] = true;
        for (int i = 0; i < nf; i++) afns[i].recursive = reach[(size_t)i * nf + i];
        free(reach);
    }
    for (int i = 0; i < nf; i++) aot_reach(&afns[i]);
    for (int i = 0; i < nf; i++) aot_constrain(vm, afns, &afns[i]);
    for (int pass = 0; pass < 4; pass++)
        for (int i = 0; i < nf; i++) aot_field_kinds(&afns[i]);

    fprintf(o, "// Generated by solis --emit-c. Build with:\n//   cc -O2 -fwrapv -I<solis>/runtime this.c <solis>/runtime/solis_std.c <solis>/runtime/solis_os.c -lm\n#define _GNU_SOURCE\n#include \"solis.c\"\n#include \"aot_rt.h\"\n\n");
    fprintf(o, "static inline double aot_f64(uint64_t b) { double d; memcpy(&d, &b, sizeof d); return d; }\n");
    fprintf(o, "static inline sl_value aot_global(sl_vm *vm, unsigned g) { sl_value v = vload(&vm->globals[g]); sl_retain(v); return v; }\n\n");
    for (int t = 0; t < vm->nstructs; t++) {
        if (!struct_ok(t)) continue;
        int n = vm->struct_desc[t][0];
        fprintf(o, "typedef struct {");
        for (int k = 0; k < n; k++) {
            int fk = fkind(t, k);
            fprintf(o, " %s f%d;", fk == 1 ? "int64_t" : fk == 2 ? "double" : "bool", k);
        }
        fprintf(o, " } S%d;\n", t);
        fprintf(o, "static inline S%d S%d_load(const sl_struct *s) { S%d v;", t, t, t);
        for (int k = 0; k < n; k++) {
            int fk = fkind(t, k);
            fprintf(o, " v.f%d = s->fields[%d].as.%s;", k, k, fk == 1 ? "i" : fk == 2 ? "f" : "i != 0 ? 1 : 0; (void)0");
        }
        fprintf(o, " return v; }\n");
        fprintf(o, "static inline void S%d_store(sl_struct *s, S%d v) {", t, t);
        for (int k = 0; k < n; k++) {
            int fk = fkind(t, k);
            if (fk == 1) fprintf(o, " s->fields[%d].as.i = v.f%d;", k, k);
            else if (fk == 2) fprintf(o, " s->fields[%d].as.f = v.f%d;", k, k);
            else fprintf(o, " s->fields[%d].as.i = v.f%d;", k, k);
        }
        fprintf(o, " }\n");
        fprintf(o, "static sl_value S%d_box(sl_vm *vm, S%d v) { sl_struct *s = struct_alloc(vm, %d); if (!s) AOT_FAIL(\"out of memory\"); s->head.rc = 1; s->head.kind = SL_STRUCT; s->nfields = %d;", t, t, n, n);
        for (int k = 0; k < n; k++) {
            int fk = fkind(t, k);
            fprintf(o, " s->fields[%d].type = %s; s->fields[%d].as.i = 0; s->fields[%d].as.%s = v.f%d;", k, fk == 1 ? "SL_INT" : fk == 2 ? "SL_FLOAT" : "SL_BOOL", k, k, fk == 2 ? "f" : "i", k);
        }
        fprintf(o, " vm->live++; return obj_value(&s->head); }\n");
    }
    fprintf(o, "\n");
    for (int i = 0; i < nf; i++) {
        fprintf(o, "static %s f%d(sl_vm *vm", afns[i].returns ? ctype_w(afns[i].retnode) : "void", i);
        for (int k = 0; k < afns[i].f->arity; k++) fprintf(o, ", %s a%d", ctype_w(afns[i].dbase + k), k);
        fprintf(o, ");\n");
    }
    fprintf(o, "\n");
    for (int i = 0; i < nf; i++) aot_emit_fn(vm, afns, i, o);
    fprintf(o, "static const uint8_t aot_image[] = {");
    for (size_t i = 0; i < len; i++) fprintf(o, "%s%u,", i % 24 ? "" : "\n", img[i]);
    fprintf(o, "\n};\n\n");
    fprintf(o, "int main(int argc, char **argv) {\n    (void)argc; (void)argv;\n    sl_vm *vm = sl_new();\n    sl_open_std(vm);\n    sl_open_os(vm, 0, NULL);\n    sl_register(vm, \"print\", aot_native_print);\n");
    fprintf(o, "    if (sl_load(vm, aot_image, sizeof aot_image) != SL_OK) { fprintf(stderr, \"load error: %%s\\n\", sl_error(vm)); return 1; }\n");
    fprintf(o, "    if (setjmp(aot_jb)) { fflush(stdout); fprintf(stderr, \"runtime error: %%s\\n\", sl_error(vm)); return 1; }\n");
    fprintf(o, "    f%d(vm);\n    fflush(stdout);\n    return 0;\n}\n", vm->entry);
    return 0;
}
