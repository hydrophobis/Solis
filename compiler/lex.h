// Tokens and the tokenizer.
//
// Bad input never stops the scan. It becomes a diagnostic and scanning
// continues, so one typo doesn't hide the rest of the file.

#ifndef SOLIS_LEX_H
#define SOLIS_LEX_H

#include "common.h"

typedef enum {
    // literals
    T_INT, T_FLOAT, T_STR, T_IDENT,

    // `default` is not a keyword; it's matched as an identifier followed
    // by `:`, so it stays usable as a name.
    T_FUNC, T_LET, T_VAR, T_IF, T_ELSE, T_WHILE, T_FOR, T_IN,
    T_SWITCH, T_CASE, T_RETURN, T_BREAK, T_CONTINUE,
    T_STRUCT, T_ENUM, T_INTERFACE, T_STATIC, T_EXTERN, T_IMPORT,
    T_ARENA, T_WEAK, T_MUT, T_THIS, T_TRUE, T_FALSE, T_NULL,

    // punctuation
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
    T_COMMA, T_SEMI, T_COLON, T_DOT, T_ARROW, T_FAT_ARROW,
    T_QUESTION, T_UNDERSCORE,

    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_BANG,

    T_EQ, T_PLUS_EQ, T_MINUS_EQ, T_STAR_EQ, T_SLASH_EQ, T_PERCENT_EQ,

    T_EQ_EQ, T_BANG_EQ, T_LT, T_LT_EQ, T_GT, T_GT_EQ, T_AND_AND, T_OR_OR,

    T_EOF
} token_kind;

// One piece of a string literal. Interpolations keep their *raw source*, which
// the parser re-lexes in context, the same split the Rust implementation
// makes, and the reason `"${f("}")}"` works.
typedef struct {
    bool        is_expr;
    const char *text;
} str_part;

typedef VEC(str_part) str_part_vec;

typedef struct {
    token_kind kind;
    span       at;
    union {
        int64_t       i;
        double        f;
        const char   *ident;        // interned
        str_part_vec  parts;        // T_STR
    } as;
} token;

typedef VEC(token) token_vec;

// How a token should be named in a diagnostic.
const char *token_describe(arena *a, const token *t);
// The literal spelling of a fixed token, or "" for literals.
const char *token_lexeme(token_kind k);

// Tokenize `src`. The result always ends with T_EOF.
token_vec lex(arena *a, sl_interner *in, const char *src, size_t len, diag_vec *diags);

#endif // SOLIS_LEX_H
