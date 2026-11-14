// Shared by the compile and analysis entry points. Not part of the public API.

#ifndef SOLIS_COMPILE_INTERNAL_H
#define SOLIS_COMPILE_INTERNAL_H

#include "module.h"
#include "solis_compile.h"

// Load the entry file and its imports through the options' hooks. Returns NULL
// and sets *prelude_missing when an explicitly named prelude cannot be opened.
loaded *sl_load_modules(arena *a, sl_interner *in, const char *path, const sl_compile_options *opt,
                        diag_vec *diags, int *prelude_missing);

// The diagnostics as data, with positions taken from each module's own source.
// Allocates *out with calloc; the caller frees it and the strings in it.
void sl_fill_diagnostics(const loaded *l, const diag_vec *diags, const char *path,
                         sl_diagnostic **out, int *count);

#endif
