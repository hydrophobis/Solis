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

static const char src_json[] = {
#embed "../std/json.sl"
    , 0
};

static const char src_path[] = {
#embed "../std/path.sl"
    , 0
};

static const char src_hex[] = {
#embed "../std/hex.sl"
    , 0
};

static const char src_base64[] = {
#embed "../std/base64.sl"
    , 0
};

static const char src_hash[] = {
#embed "../std/hash.sl"
    , 0
};

static const char src_regex[] = {
#embed "../std/regex.sl"
    , 0
};

static const char src_csv[] = {
#embed "../std/csv.sl"
    , 0
};

static const char src_url[] = {
#embed "../std/url.sl"
    , 0
};

static const char src_datetime[] = {
#embed "../std/datetime.sl"
    , 0
};

static const char src_lists[] = {
#embed "../std/lists.sl"
    , 0
};

static const char src_flags[] = {
#embed "../std/flags.sl"
    , 0
};

static const char src_log[] = {
#embed "../std/log.sl"
    , 0
};

static const char src_http[] = {
#embed "../std/http.sl"
    , 0
};

static const char src_ansi[] = {
#embed "../std/ansi.sl"
    , 0
};

static const char src_text[] = {
#embed "../std/text.sl"
    , 0
};

static const char src_toml[] = {
#embed "../std/toml.sl"
    , 0
};

static const char src_net[] = {
#embed "../std/net.sl"
    , 0
};

static const char src_sqlite[] = {
#embed "../std/sqlite.sl"
    , 0
};

static const char src_httpd[] = {
#embed "../std/httpd.sl"
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
    if (strcmp(name, "json") == 0) return src_json;
    if (strcmp(name, "path") == 0) return src_path;
    if (strcmp(name, "hex") == 0) return src_hex;
    if (strcmp(name, "base64") == 0) return src_base64;
    if (strcmp(name, "hash") == 0) return src_hash;
    if (strcmp(name, "regex") == 0) return src_regex;
    if (strcmp(name, "csv") == 0) return src_csv;
    if (strcmp(name, "url") == 0) return src_url;
    if (strcmp(name, "datetime") == 0) return src_datetime;
    if (strcmp(name, "lists") == 0) return src_lists;
    if (strcmp(name, "flags") == 0) return src_flags;
    if (strcmp(name, "log") == 0) return src_log;
    if (strcmp(name, "http") == 0) return src_http;
    if (strcmp(name, "ansi") == 0) return src_ansi;
    if (strcmp(name, "text") == 0) return src_text;
    if (strcmp(name, "toml") == 0) return src_toml;
    if (strcmp(name, "net") == 0) return src_net;
    if (strcmp(name, "sqlite") == 0) return src_sqlite;
    if (strcmp(name, "httpd") == 0) return src_httpd;
    return NULL;
}
