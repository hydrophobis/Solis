#ifndef SOLIS_ANALYSIS_H
#define SOLIS_ANALYSIS_H

// What the type checker knows, for tools: what a name refers to, where it is
// declared, its type, every use of it, and what can be typed at a position.
// This is the layer a language server sits on. It is read-only; analysing
// again after an edit gives a fresh result.
//
// Offsets are byte offsets into the file's text, as in solis_syntax.h. Convert
// with sl_offset_to_line_col and sl_line_col_to_offset. (A client that counts
// UTF-16 units, as the language server protocol does, converts at the edge.)
//
// The checker runs only when the files parse. After a syntax error the result
// still holds the diagnostics, but entity and completion queries find
// nothing; keep the last good analysis for answering while the user types, or
// use sl_completion_source below.

#include "solis_compile.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sl_analysis sl_analysis;

typedef struct {
    const char *file;           // as it was given or resolved; "<host>" for the host module
    uint32_t    start, end;     // byte offsets
    uint32_t    line, col;      // 1-based, of start
    uint32_t    end_line, end_col;
} sl_location;

typedef enum {
    SL_ENT_LOCAL, SL_ENT_PARAM, SL_ENT_FUNC, SL_ENT_CONST, SL_ENT_TYPE,
    SL_ENT_FIELD, SL_ENT_METHOD, SL_ENT_VARIANT, SL_ENT_MODULE, SL_ENT_BUILTIN,
    SL_ENT_KEYWORD
} sl_entity_kind;

// Everything known about a name. Strings belong to the analysis.
typedef struct {
    sl_entity_kind kind;
    const char    *name;
    const char    *type;        // "int", "[str]", "func(int): bool"; "" when there is none
    const char    *detail;      // one line, e.g. "func area(r: float): float"
    int            has_definition;
    sl_location    use;         // the name where it was found
    sl_location    definition;  // valid when has_definition
} sl_entity;

typedef struct {
    const char    *label;
    sl_entity_kind kind;
    const char    *detail;
} sl_completion;

// Analyse the program rooted at `path`. Takes the same options as sl_compile_with
// (an editor buffer as entry_source, hooks for reading files, a host module,
// a prelude); check_only is ignored. Returns NULL only if memory runs out.
sl_analysis *sl_analyze(const char *path, const sl_compile_options *options);
void         sl_analysis_free(sl_analysis *a);

// Errors, in the same form as sl_compiled. Valid until the analysis is freed.
const sl_diagnostic *sl_analysis_diagnostics(const sl_analysis *a, int *count);
// Whether the checker ran. False when a file did not parse.
int sl_analysis_checked(const sl_analysis *a);

// The name at `offset` in `file` (a path as given to sl_analyze or produced by
// the import hook). The offset may be at the end of the name. Returns 1 and
// fills *out, or 0 if there is no name there.
int sl_analysis_entity_at(const sl_analysis *a, const char *file, uint32_t offset, sl_entity *out);

// Every place `entity` is used, including where it is declared, in file and
// position order. Writes at most `max` locations and returns how many there are.
int sl_analysis_references(const sl_analysis *a, const sl_entity *entity, sl_location *out, int max);

// What can be typed at `offset`: names in scope, or the members of the thing
// before a '.'. Entries starting with the text already typed come back. The
// array is valid until the next call on this analysis or until it is freed.
const sl_completion *sl_analysis_complete(sl_analysis *a, const char *file, uint32_t offset, int *count);

// Completion usually happens in code that does not parse, such as `player.`.
// This returns a malloc'd copy of the source with a placeholder after the dot
// (and a semicolon when the line ends there) so it can be analysed; analyse the
// copy and complete at the same offset. Returns NULL when the source needs no
// change. Free with free().
char *sl_completion_source(const char *source, size_t len, uint32_t offset, size_t *out_len);

// 1-based line and column to byte offset; the inverse of sl_offset_to_line_col.
uint32_t sl_line_col_to_offset(const char *source, size_t len, uint32_t line, uint32_t col);

#ifdef __cplusplus
}
#endif

#endif // SOLIS_ANALYSIS_H
