// Module loading. A module is a file: `import math;` looks for math.sl next
// to the importer, and that bare name identifies it everywhere.
//
// Loading is depth-first with the import stack carried along, so a cycle can
// be reported as the actual chain. Modules are kept sorted by name: the
// visit order decides function numbering in the output, so it has to be
// stable.

#ifndef SOLIS_MODULE_H
#define SOLIS_MODULE_H

#include "ast.h"
#include "lex.h"

typedef struct {
    const char  *name;
    const char  *path;
    const char  *src;
    size_t       src_len;
    program     *prog;
    // Module names this one imports, in source order.
    VEC(const char *) imports;
} module;

typedef VEC(module *) module_vec;

typedef struct {
    module_vec  modules;        // sorted by name
    const char *root;           // the entry module
    // The prelude's module name, or NULL. Every module can see it without
    // importing it, and its functions resolve unqualified, which is what
    // saves a host's plugins from re-declaring every extern they call.
    const char *prelude;
} loaded;

// Where the loader gets files from. Every member is optional; a NULL member
// means the default, which reads from disk and looks for `import name;` in the
// entry file's directory. Strings handed back are allocated from `a` or are
// static, and the loader never frees them.
typedef struct {
    // The text of the file at `path`, or NULL if it cannot be read.
    const char *(*read)(void *user, arena *a, const char *path, size_t *len);
    // Whether a file exists. Decides if a file overrides a built-in module.
    bool (*exists)(void *user, const char *path);
    // The path `import name;` refers to when written in the file `from`, or NULL
    // for the default (`name.sl` beside the entry file).
    const char *(*resolve)(void *user, arena *a, const char *from, const char *name);
    void *user;
} module_io;

// Load `entry` and everything it imports, transitively.
//
// `prelude_path` picks the ambient declarations: NULL looks for prelude.sl
// beside the entry file, "" turns the mechanism off, anything else is used
// as given.
//
// `host_src` is the source `import host;` resolves to, or NULL. It takes
// precedence over a host.sl on disk.
loaded *module_load(arena *a, sl_interner *in, const char *entry_path,
                    const char *prelude_path, const char *host_src, diag_vec *diags);

// As module_load, reading files through `io` (NULL for the disk). When
// `entry_src` is not NULL it is the text of `entry_path`, which then need not
// exist: the path only names the module and anchors its imports.
loaded *module_load_io(arena *a, sl_interner *in, const char *entry_path,
                       const char *entry_src, const char *prelude_path,
                       const char *host_src, const module_io *io, diag_vec *diags);

module *module_find(const loaded *l, const char *name);

// Source for a built-in module, or NULL. The stdlib is compiled into the
// binary, so `import math;` needs no file on disk.
const char *stdlib_source(const char *name);

#endif // SOLIS_MODULE_H
