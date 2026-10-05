// AST to bytecode. Runs after the checker, so the program is known good and
// the instructions can be typed: ADD_I for ints, ADD_F for floats.
//
// Field access compiles to an index and `for`/`switch` are desugared here,
// which is most of why the VM stays small.

#ifndef SOLIS_GEN_H
#define SOLIS_GEN_H

#include "check.h"
#include "emit.h"

// Compile every module to one bytecode image. Returns NULL and appends
// diagnostics if anything cannot be lowered.
bc_program *generate(arena *a, sl_interner *in, loaded *l, decls *d, diag_vec *diags);

#endif // SOLIS_GEN_H
