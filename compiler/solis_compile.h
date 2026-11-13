#ifndef SOLIS_COMPILE_H
#define SOLIS_COMPILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *image;
    size_t   image_len;
    char    *errors;     // rendered diagnostics, or NULL
    int      error_count;
} sl_compiled;

// prelude: NULL uses a prelude.sl beside the script, "" disables it, anything else is a path.
// Returns 0 on success. On failure `image` is NULL and `errors` holds the diagnostics.
// Always call sl_compiled_free().
int  sl_compile_file(const char *path, const char *prelude, sl_compiled *out);

// host: source of the module `import host;` resolves to (see sl_declarations), or NULL for none.
int  sl_compile_file_with_host(const char *path, const char *prelude, const char *host, sl_compiled *out);

int  sl_check_file(const char *path, const char *prelude, sl_compiled *out);
int  sl_check_file_with_host(const char *path, const char *prelude, const char *host, sl_compiled *out);

void sl_compiled_free(sl_compiled *c);

#ifdef __cplusplus
}
#endif

#endif // SOLIS_COMPILE_H
