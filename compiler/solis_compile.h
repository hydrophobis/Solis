#ifndef SOLIS_COMPILE_H
#define SOLIS_COMPILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One error, with the position an editor needs. Lines and columns are 1-based
// and columns count characters, not bytes. The strings belong to the
// sl_compiled that holds this diagnostic.
typedef struct {
    char    *file;        // path as given, as resolved for an import, or "<host>"
    uint32_t line, col;
    uint32_t end_line, end_col;
    char    *message;
    char    *help;        // NULL when there is nothing to add
} sl_diagnostic;

typedef struct {
    uint8_t *image;
    size_t   image_len;
    char    *errors;     // rendered diagnostics, or NULL
    int      error_count;
    // The same errors as data, in report order. NULL when there are none.
    sl_diagnostic *diagnostics;
    int            diagnostic_count;
} sl_compiled;

// prelude: NULL uses a prelude.sl beside the script, "" disables it, anything else is a path.
// Returns 0 on success. On failure `image` is NULL and `errors` holds the diagnostics.
// Always call sl_compiled_free().
int  sl_compile_file(const char *path, const char *prelude, sl_compiled *out);

// host: source of the module `import host;` resolves to (see sl_declarations), or NULL for none.
int  sl_compile_file_with_host(const char *path, const char *prelude, const char *host, sl_compiled *out);

int  sl_check_file(const char *path, const char *prelude, sl_compiled *out);
int  sl_check_file_with_host(const char *path, const char *prelude, const char *host, sl_compiled *out);

// Everything above, and more, in one call. Zero the struct and set what you need.
typedef struct {
    const char *prelude;        // as for sl_compile_file
    const char *host;           // as for sl_compile_file_with_host
    int         check_only;     // type check without producing an image

    // The text of the entry file. When set, `path` need not exist: it names the
    // module and anchors its imports. Use this for an editor buffer or a string.
    const char *entry_source;

    // Hooks, all optional. Anything they return is copied; the compiler frees
    // what it is given with free(), so allocate with malloc.
    //
    // read:    the text of `path`, or NULL if it cannot be read.
    // exists:  whether `path` exists. A file named like a built-in module
    //          overrides it, so this decides which one `import` finds.
    // resolve: the path `import name;` means when written in the file `from`,
    //          or NULL to use the default (name.sl beside the entry file).
    char *(*read)(void *user, const char *path, size_t *len);
    int   (*exists)(void *user, const char *path);
    char *(*resolve)(void *user, const char *from, const char *name);
    void *user;
} sl_compile_options;

int  sl_compile_with(const char *path, const sl_compile_options *options, sl_compiled *out);

void sl_compiled_free(sl_compiled *c);

#ifdef __cplusplus
}
#endif

#endif // SOLIS_COMPILE_H
