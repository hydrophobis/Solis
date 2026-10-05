// Recursive-descent parser.
//
// On a syntax error it records a diagnostic and resynchronises at the next
// statement or item boundary rather than giving up, so a missing semicolon
// doesn't swallow the rest of the file.

#ifndef SOLIS_PARSE_H
#define SOLIS_PARSE_H

#include "ast.h"
#include "lex.h"

// Lex and parse `src`. Diagnostics are appended to `diags`; the returned
// program is whatever could be recovered, which may be partial.
program *parse(arena *a, sl_interner *in, const char *src, size_t len, diag_vec *diags);

#endif // SOLIS_PARSE_H
