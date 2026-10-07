// The stdlib's Solis source, compiled in so `import math;` needs no file on
// disk.
//
// #embed reads the .sl files directly. That also means -MM reports them as
// dependencies, so editing the stdlib rebuilds solis without anyone having to
// remember a regeneration step.
//
// Compilers without #embed fall back to std_fallback.h, a committed copy
// produced by embed_std.py. That keeps the build working on anything from C99
// up, at the cost of having to regenerate it when the stdlib changes.

#include "module.h"

// -DSOLIS_NO_EMBED forces the fallback, which is how both paths get tested.
#if !defined(SOLIS_NO_EMBED) && defined(__has_embed)
#  if __has_embed("../std/math.sl") == __STDC_EMBED_FOUND__
#    define SOLIS_HAVE_EMBED 1
#  endif
#endif

#ifdef SOLIS_HAVE_EMBED

// #embed is C23. GCC and Clang take it earlier as an extension, and this
// builds as C99, so the pedantic note about that is expected here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#ifdef __clang__
#  pragma GCC diagnostic ignored "-Wc23-extensions"
#endif

static const char src_math[] = {
#embed "../std/math.sl"
    , 0
};

static const char src_strings[] = {
#embed "../std/strings.sl"
    , 0
};

static const char src_io[] = {
#embed "../std/io.sl"
    , 0
};

static const char src_fs[] = {
#embed "../std/fs.sl"
    , 0
};

static const char src_os[] = {
#embed "../std/os.sl"
    , 0
};

static const char src_time[] = {
#embed "../std/time.sl"
    , 0
};

static const char src_rand[] = {
#embed "../std/rand.sl"
    , 0
};

static const char src_option[] = {
#embed "../std/option.sl"
    , 0
};

static const char src_result[] = {
#embed "../std/result.sl"
    , 0
};

static const char src_test[] = {
#embed "../std/test.sl"
    , 0
};

static const char src_map[] = {
#embed "../std/map.sl"
    , 0
};

#pragma GCC diagnostic pop

#else
#  include "std_fallback.h"
#endif

// `math` and `strings` are the language's; the rest are the batteries the
// standard interpreter opens. The compiler knows all of them either way; it
// is the runtime that decides which are reachable, so a sandboxed host
// rejects `import fs;` when the natives are not registered.
const char *stdlib_source(const char *name) {
    if (strcmp(name, "math") == 0) return src_math;
    if (strcmp(name, "strings") == 0) return src_strings;
    if (strcmp(name, "io") == 0) return src_io;
    if (strcmp(name, "fs") == 0) return src_fs;
    if (strcmp(name, "os") == 0) return src_os;
    if (strcmp(name, "time") == 0) return src_time;
    if (strcmp(name, "rand") == 0) return src_rand;
    if (strcmp(name, "option") == 0) return src_option;
    if (strcmp(name, "result") == 0) return src_result;
    if (strcmp(name, "test") == 0) return src_test;
    if (strcmp(name, "map") == 0) return src_map;
    return NULL;
}
