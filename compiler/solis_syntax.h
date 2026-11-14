#ifndef SOLIS_SYNTAX_H
#define SOLIS_SYNTAX_H

// Read-only access to the syntax of a Solis source file, for tools that are not
// compilers: formatters, editors, outline views, linters. Parsing never fails.
// Errors come back as diagnostics, and the tokens, comments and outline describe
// whatever could be recovered.
//
// This is the syntax only. Names are not resolved and nothing is type checked, so
// there is no "go to definition" here; that needs the checker and is a separate
// addition.

#include "solis_compile.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sl_syntax sl_syntax;

// Same order and numbering as the compiler's token kinds.
typedef enum {
    SL_T_INT, SL_T_FLOAT, SL_T_STR, SL_T_IDENT,
    SL_T_FUNC, SL_T_LET, SL_T_VAR, SL_T_IF, SL_T_ELSE, SL_T_WHILE, SL_T_FOR, SL_T_IN,
    SL_T_SWITCH, SL_T_CASE, SL_T_RETURN, SL_T_BREAK, SL_T_CONTINUE,
    SL_T_STRUCT, SL_T_ENUM, SL_T_INTERFACE, SL_T_STATIC, SL_T_EXTERN, SL_T_IMPORT,
    SL_T_ARENA, SL_T_WEAK, SL_T_MUT, SL_T_THIS, SL_T_TRUE, SL_T_FALSE, SL_T_NULL,
    SL_T_LPAREN, SL_T_RPAREN, SL_T_LBRACE, SL_T_RBRACE, SL_T_LBRACKET, SL_T_RBRACKET,
    SL_T_COMMA, SL_T_SEMI, SL_T_COLON, SL_T_DOT, SL_T_ARROW, SL_T_FAT_ARROW,
    SL_T_QUESTION, SL_T_UNDERSCORE,
    SL_T_PLUS, SL_T_MINUS, SL_T_STAR, SL_T_SLASH, SL_T_PERCENT, SL_T_BANG,
    SL_T_EQ, SL_T_PLUS_EQ, SL_T_MINUS_EQ, SL_T_STAR_EQ, SL_T_SLASH_EQ, SL_T_PERCENT_EQ,
    SL_T_EQ_EQ, SL_T_BANG_EQ, SL_T_LT, SL_T_LT_EQ, SL_T_GT, SL_T_GT_EQ, SL_T_AND_AND, SL_T_OR_OR,
    SL_T_EOF
} sl_token_kind;

typedef struct {
    sl_token_kind kind;
    uint32_t      start, end;   // byte offsets into the source
} sl_token;

typedef struct {
    uint32_t start, end;        // includes the // or /* */; a block comment starts "/*"
} sl_comment;

typedef enum {
    SL_SYM_FUNC, SL_SYM_STRUCT, SL_SYM_ENUM, SL_SYM_INTERFACE, SL_SYM_EXTERN,
    SL_SYM_LET, SL_SYM_IMPORT,
    SL_SYM_FIELD, SL_SYM_METHOD, SL_SYM_VARIANT
} sl_symbol_kind;

// One declaration. Fields, methods and variants point at their struct, enum or
// interface through `parent`; top-level declarations have -1.
typedef struct {
    sl_symbol_kind kind;
    const char    *name;        // owned by the sl_syntax
    int            parent;      // index into the symbol array, or -1
    uint32_t       start, end;  // the whole declaration
    uint32_t       name_start, name_end;
} sl_symbol;

// Parse `len` bytes of source (copied). Returns NULL only if memory runs out.
sl_syntax *sl_syntax_parse(const char *source, size_t len);
void       sl_syntax_free(sl_syntax *s);

const char       *sl_syntax_source(const sl_syntax *s, size_t *len);
// Tokens in source order, ending with an SL_T_EOF token. Comments are not tokens.
const sl_token   *sl_syntax_tokens(const sl_syntax *s, int *count);
// Comments in source order.
const sl_comment *sl_syntax_comments(const sl_syntax *s, int *count);
const sl_symbol  *sl_syntax_symbols(const sl_syntax *s, int *count);
// Syntax errors. The file name in each is "<source>".
const sl_diagnostic *sl_syntax_diagnostics(const sl_syntax *s, int *count);

// "ident", "func", "(", and so on, for debugging and tooling output.
const char *sl_token_kind_name(sl_token_kind kind);

// Byte offset to 1-based line and column; columns count characters.
void sl_offset_to_line_col(const char *source, size_t len, uint32_t offset,
                           uint32_t *line, uint32_t *col);

// 1-based line and column to byte offset; the inverse of sl_offset_to_line_col.
uint32_t sl_line_col_to_offset(const char *source, size_t len, uint32_t line, uint32_t col);

#ifdef __cplusplus
}
#endif

#endif // SOLIS_SYNTAX_H
