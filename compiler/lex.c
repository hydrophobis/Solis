#include "lex.h"

#include <errno.h>
#include <stdio.h>

const char *token_lexeme(token_kind k) {
    switch (k) {
        case T_FUNC: return "func";
        case T_LET: return "let";
        case T_VAR: return "var";
        case T_IF: return "if";
        case T_ELSE: return "else";
        case T_WHILE: return "while";
        case T_FOR: return "for";
        case T_IN: return "in";
        case T_SWITCH: return "switch";
        case T_CASE: return "case";
        case T_RETURN: return "return";
        case T_BREAK: return "break";
        case T_CONTINUE: return "continue";
        case T_STRUCT: return "struct";
        case T_ENUM: return "enum";
        case T_INTERFACE: return "interface";
        case T_STATIC: return "static";
        case T_EXTERN: return "extern";
        case T_IMPORT: return "import";
        case T_ARENA: return "arena";
        case T_WEAK: return "weak";
        case T_MUT: return "mut";
        case T_THIS: return "this";
        case T_TRUE: return "true";
        case T_FALSE: return "false";
        case T_NULL: return "null";
        case T_LPAREN: return "(";
        case T_RPAREN: return ")";
        case T_LBRACE: return "{";
        case T_RBRACE: return "}";
        case T_LBRACKET: return "[";
        case T_RBRACKET: return "]";
        case T_COMMA: return ",";
        case T_SEMI: return ";";
        case T_COLON: return ":";
        case T_DOT: return ".";
        case T_ARROW: return "->";
        case T_FAT_ARROW: return "=>";
        case T_QUESTION: return "?";
        case T_UNDERSCORE: return "_";
        case T_PLUS: return "+";
        case T_MINUS: return "-";
        case T_STAR: return "*";
        case T_SLASH: return "/";
        case T_PERCENT: return "%";
        case T_BANG: return "!";
        case T_EQ: return "=";
        case T_PLUS_EQ: return "+=";
        case T_MINUS_EQ: return "-=";
        case T_STAR_EQ: return "*=";
        case T_SLASH_EQ: return "/=";
        case T_PERCENT_EQ: return "%=";
        case T_EQ_EQ: return "==";
        case T_BANG_EQ: return "!=";
        case T_LT: return "<";
        case T_LT_EQ: return "<=";
        case T_GT: return ">";
        case T_GT_EQ: return ">=";
        case T_AND_AND: return "&&";
        case T_OR_OR: return "||";
        default: return "";
    }
}

const char *token_describe(arena *a, const token *t) {
    switch (t->kind) {
        case T_INT: return "integer literal";
        case T_FLOAT: return "float literal";
        case T_STR: return "string literal";
        case T_IDENT: return arena_printf(a, "identifier `%s`", t->as.ident);
        case T_EOF: return "end of file";
        default: return arena_printf(a, "`%s`", token_lexeme(t->kind));
    }
}

// Maps a keyword spelling to its token, or T_IDENT for an ordinary name.
static token_kind keyword_of(const char *p, size_t n) {
    struct { const char *word; token_kind kind; } table[] = {
        { "func", T_FUNC }, { "let", T_LET }, { "var", T_VAR },
        { "if", T_IF }, { "else", T_ELSE }, { "while", T_WHILE },
        { "for", T_FOR }, { "in", T_IN }, { "switch", T_SWITCH },
        { "case", T_CASE }, { "return", T_RETURN }, { "break", T_BREAK },
        { "continue", T_CONTINUE }, { "struct", T_STRUCT }, { "enum", T_ENUM },
        { "interface", T_INTERFACE }, { "static", T_STATIC },
        { "extern", T_EXTERN }, { "import", T_IMPORT }, { "arena", T_ARENA },
        { "weak", T_WEAK }, { "mut", T_MUT }, { "this", T_THIS },
        { "true", T_TRUE }, { "false", T_FALSE }, { "null", T_NULL },
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strlen(table[i].word) == n && memcmp(table[i].word, p, n) == 0)
            return table[i].kind;
    return T_IDENT;
}

typedef struct {
    const char  *src;
    size_t       len;
    size_t       pos;
    arena       *a;
    sl_interner *in;
    diag_vec    *diags;
} lexer;

// Bytes in the UTF-8 sequence starting with `b`. A continuation or invalid
// byte advances one, which guarantees progress on malformed input.
static size_t utf8_len(unsigned char b) {
    if (b < 0x80) return 1;
    if (b >= 0xC0 && b <= 0xDF) return 2;
    if (b >= 0xE0 && b <= 0xEF) return 3;
    if (b >= 0xF0 && b <= 0xF7) return 4;
    return 1;
}

static bool at_end(lexer *L) { return L->pos >= L->len; }

static unsigned char peek(lexer *L) {
    return at_end(L) ? 0 : (unsigned char)L->src[L->pos];
}

static unsigned char peek_at(lexer *L, size_t n) {
    size_t i = L->pos + n;
    return i >= L->len ? 0 : (unsigned char)L->src[i];
}

static bool eat(lexer *L, unsigned char c) {
    if (peek(L) == c) { L->pos++; return true; }
    return false;
}

static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool is_alnum(unsigned char c) { return is_alpha(c) || is_digit(c); }
static bool is_ident_start(unsigned char c) { return is_alpha(c) || c == '_'; }
static bool is_ident_continue(unsigned char c) { return is_alnum(c) || c == '_'; }
static bool is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Whitespace plus line and nesting block comments.
static void skip_trivia(lexer *L) {
    for (;;) {
        while (!at_end(L) && is_ws(peek(L))) L->pos++;

        if (peek(L) == '/' && peek_at(L, 1) == '/') {
            while (!at_end(L) && peek(L) != '\n') L->pos++;
            continue;
        }
        if (peek(L) == '/' && peek_at(L, 1) == '*') {
            size_t start = L->pos;
            L->pos += 2;
            // Nesting means a commented-out block containing a comment still
            // terminates where you expect.
            size_t depth = 1;
            while (!at_end(L) && depth > 0) {
                if (peek(L) == '/' && peek_at(L, 1) == '*') { depth++; L->pos += 2; }
                else if (peek(L) == '*' && peek_at(L, 1) == '/') { depth--; L->pos += 2; }
                else L->pos += utf8_len(peek(L));
            }
            if (depth > 0) {
                diag_help(L->a, L->diags, span_make(start, L->len),
                          "add `*/` to close it", "unterminated block comment");
            }
            continue;
        }
        break;
    }
}

// A number. Returns false if a diagnostic was recorded instead.
static bool scan_radix(lexer *L, size_t start, int radix, const char *name, token *out) {
    size_t digits_start = L->pos;
    while (is_alnum(peek(L)) || peek(L) == '_') L->pos++;

    span at = span_make(start, L->pos);
    // Strip the digit separators before parsing.
    char buf[80];
    size_t n = 0;
    for (size_t i = digits_start; i < L->pos && n + 1 < sizeof buf; i++)
        if (L->src[i] != '_') buf[n++] = L->src[i];
    buf[n] = 0;

    if (n == 0) {
        diag_addf(L->a, L->diags, at, "%s literal has no digits", name);
        return false;
    }
    errno = 0;
    char *end = NULL;
    long long v = strtoll(buf, &end, radix);
    if (errno == ERANGE || (end && *end)) {
        diag_addf(L->a, L->diags, at, "`%.*s` is not a valid %s literal",
                  (int)(L->pos - start), L->src + start, name);
        return false;
    }
    out->kind = T_INT;
    out->as.i = (int64_t)v;
    return true;
}

static bool scan_number(lexer *L, token *out) {
    size_t start = L->pos;

    // Hex and binary are integers only.
    if (peek(L) == '0' && (peek_at(L, 1) | 0x20) == 'x') {
        L->pos += 2;
        return scan_radix(L, start, 16, "hexadecimal", out);
    }
    if (peek(L) == '0' && (peek_at(L, 1) | 0x20) == 'b') {
        L->pos += 2;
        return scan_radix(L, start, 2, "binary", out);
    }

    while (is_digit(peek(L)) || peek(L) == '_') L->pos++;

    // A '.' is only a decimal point if a digit follows; otherwise it is field
    // access, so `1.max(2)` lexes as Int Dot Ident.
    bool is_float = false;
    if (peek(L) == '.' && is_digit(peek_at(L, 1))) {
        is_float = true;
        L->pos++;
        while (is_digit(peek(L)) || peek(L) == '_') L->pos++;
    }
    if ((peek(L) | 0x20) == 'e' &&
        (is_digit(peek_at(L, 1)) ||
         ((peek_at(L, 1) == '+' || peek_at(L, 1) == '-') && is_digit(peek_at(L, 2))))) {
        is_float = true;
        L->pos++;
        if (peek(L) == '+' || peek(L) == '-') L->pos++;
        while (is_digit(peek(L))) L->pos++;
    }

    span at = span_make(start, L->pos);
    char buf[512];
    size_t n = 0;
    for (size_t i = start; i < L->pos && n + 1 < sizeof buf; i++)
        if (L->src[i] != '_') buf[n++] = L->src[i];
    buf[n] = 0;

    if (is_float) {
        errno = 0;
        char *end = NULL;
        double v = strtod(buf, &end);
        if (end == buf || (end && *end)) {
            diag_addf(L->a, L->diags, at, "`%s` is not a valid float", buf);
            return false;
        }
        out->kind = T_FLOAT;
        out->as.f = v;
        return true;
    }

    errno = 0;
    char *end = NULL;
    long long v = strtoll(buf, &end, 10);
    if (errno == ERANGE || (end && *end)) {
        diag_help(L->a, L->diags, at, "the largest int is 9223372036854775807",
                  "integer `%s` does not fit in 64 bits", buf);
        return false;
    }
    out->kind = T_INT;
    out->as.i = (int64_t)v;
    return true;
}

// A growable text buffer, for assembling a string literal's bytes.
typedef struct { char *at; int len; int cap; } charbuf;

static void cb_push(arena *a, charbuf *b, const char *p, size_t n) {
    while (b->len + (int)n > b->cap) {
        int cap = b->cap < 32 ? 32 : b->cap * 2;
        char *out = (char *)arena_alloc(a, (size_t)cap);
        if (b->len) memcpy(out, b->at, (size_t)b->len);
        b->at = out;
        b->cap = cap;
    }
    memcpy(b->at + b->len, p, n);
    b->len += (int)n;
}

static void cb_putc(arena *a, charbuf *b, char c) { cb_push(a, b, &c, 1); }

// A string literal, splitting `${...}` interpolations out as raw source.
static bool scan_string(lexer *L, token *out) {
    size_t open = L->pos;
    L->pos++;   // the opening quote

    str_part_vec parts;
    memset(&parts, 0, sizeof parts);
    charbuf text;
    memset(&text, 0, sizeof text);

    for (;;) {
        if (at_end(L)) {
            diag_help(L->a, L->diags, span_make(open, L->pos),
                      "add a closing `\"`", "unterminated string literal");
            return false;
        }
        unsigned char c = peek(L);

        if (c == '"') { L->pos++; break; }

        // A newline inside a string is nearly always a missing quote.
        if (c == '\n') {
            diag_help(L->a, L->diags, span_make(open, L->pos),
                      "strings cannot span lines; add a closing `\"`",
                      "unterminated string literal");
            return false;
        }

        if (c == '\\') {
            size_t esc_start = L->pos;
            L->pos++;
            unsigned char e = peek(L);
            L->pos++;
            switch (e) {
                case 'n': cb_putc(L->a, &text, '\n'); break;
                case 't': cb_putc(L->a, &text, '\t'); break;
                case 'r': cb_putc(L->a, &text, '\r'); break;
                case '0': cb_putc(L->a, &text, '\0'); break;
                case '\\': cb_putc(L->a, &text, '\\'); break;
                case '"': cb_putc(L->a, &text, '"'); break;
                case '$': cb_putc(L->a, &text, '$'); break;
                default:
                    diag_help(L->a, L->diags, span_make(esc_start, L->pos),
                              "valid escapes are \\n \\t \\r \\0 \\\\ \\\" \\$",
                              "unknown escape `%.*s`",
                              (int)(L->pos - esc_start), L->src + esc_start);
                    break;
            }
            continue;
        }

        // Interpolation: capture the raw source between the braces so the
        // parser can re-lex it in context.
        if (c == '$' && peek_at(L, 1) == '{') {
            if (text.len) {
                str_part p;
                p.is_expr = false;
                p.text = arena_strndup(L->a, text.at, (size_t)text.len);
                vec_push(L->a, &parts, p);
                memset(&text, 0, sizeof text);
            }
            size_t brace_start = L->pos;
            L->pos += 2;
            size_t expr_start = L->pos;
            size_t depth = 1;
            while (!at_end(L) && depth > 0) {
                unsigned char d = peek(L);
                if (d == '{') { depth++; L->pos++; }
                else if (d == '}') { depth--; L->pos++; }
                else if (d == '"') {
                    // Skip a nested string so its braces do not count.
                    L->pos++;
                    while (!at_end(L) && peek(L) != '"') {
                        if (peek(L) == '\\') L->pos++;
                        L->pos += utf8_len(peek(L));
                    }
                    L->pos++;
                } else {
                    L->pos += utf8_len(d);
                }
            }
            if (depth > 0) {
                diag_help(L->a, L->diags, span_make(brace_start, L->pos),
                          "add a closing `}`", "unterminated interpolation");
                return false;
            }
            // Trim, then keep the raw text.
            size_t s0 = expr_start, s1 = L->pos - 1;
            while (s0 < s1 && is_ws((unsigned char)L->src[s0])) s0++;
            while (s1 > s0 && is_ws((unsigned char)L->src[s1 - 1])) s1--;
            if (s1 == s0) {
                diag_help(L->a, L->diags, span_make(brace_start, L->pos),
                          "put an expression between `${` and `}`",
                          "empty interpolation");
            } else {
                str_part p;
                p.is_expr = true;
                p.text = arena_strndup(L->a, L->src + s0, s1 - s0);
                vec_push(L->a, &parts, p);
            }
            continue;
        }

        size_t n = utf8_len(c);
        if (L->pos + n > L->len) n = L->len - L->pos;
        cb_push(L->a, &text, L->src + L->pos, n);
        L->pos += n;
    }

    // An empty literal still gets one empty text part, so `""` is not the
    // same shape as an interpolation-only literal.
    if (text.len || parts.len == 0) {
        str_part p;
        p.is_expr = false;
        p.text = arena_strndup(L->a, text.at ? text.at : "", (size_t)text.len);
        vec_push(L->a, &parts, p);
    }

    out->kind = T_STR;
    out->as.parts = parts;
    return true;
}

// Scan one token. Returns false when a diagnostic was recorded instead.
static bool scan_token(lexer *L, token *out) {
    size_t start = L->pos;
    unsigned char c = peek(L);

    if (c == '_' && !is_ident_continue(peek_at(L, 1))) {
        L->pos++;
        out->kind = T_UNDERSCORE;
        return true;
    }
    if (is_ident_start(c)) {
        while (!at_end(L) && is_ident_continue(peek(L))) L->pos++;
        size_t n = L->pos - start;
        token_kind k = keyword_of(L->src + start, n);
        out->kind = k;
        if (k == T_IDENT) out->as.ident = intern(L->in, L->src + start, n);
        return true;
    }
    if (is_digit(c)) return scan_number(L, out);
    if (c == '"') return scan_string(L, out);

    L->pos++;
    switch (c) {
        case '(': out->kind = T_LPAREN; return true;
        case ')': out->kind = T_RPAREN; return true;
        case '{': out->kind = T_LBRACE; return true;
        case '}': out->kind = T_RBRACE; return true;
        case '[': out->kind = T_LBRACKET; return true;
        case ']': out->kind = T_RBRACKET; return true;
        case ',': out->kind = T_COMMA; return true;
        case ';': out->kind = T_SEMI; return true;
        case ':': out->kind = T_COLON; return true;
        case '.': out->kind = T_DOT; return true;
        case '?': out->kind = T_QUESTION; return true;
        case '+': out->kind = eat(L, '=') ? T_PLUS_EQ : T_PLUS; return true;
        case '-':
            out->kind = eat(L, '=') ? T_MINUS_EQ : eat(L, '>') ? T_ARROW : T_MINUS;
            return true;
        case '*': out->kind = eat(L, '=') ? T_STAR_EQ : T_STAR; return true;
        case '/': out->kind = eat(L, '=') ? T_SLASH_EQ : T_SLASH; return true;
        case '%': out->kind = eat(L, '=') ? T_PERCENT_EQ : T_PERCENT; return true;
        case '!': out->kind = eat(L, '=') ? T_BANG_EQ : T_BANG; return true;
        case '=':
            out->kind = eat(L, '=') ? T_EQ_EQ : eat(L, '>') ? T_FAT_ARROW : T_EQ;
            return true;
        case '<': out->kind = eat(L, '=') ? T_LT_EQ : T_LT; return true;
        case '>': out->kind = eat(L, '=') ? T_GT_EQ : T_GT; return true;

        // The Lua-isms get a targeted hint, because the people most likely to
        // type them are exactly the audience for this language.
        case '&':
            if (eat(L, '&')) { out->kind = T_AND_AND; return true; }
            diag_help(L->a, L->diags, span_make(start, L->pos),
                      "Solis has no bitwise `&`; did you mean `&&`?", "unexpected `&`");
            return false;
        case '|':
            if (eat(L, '|')) { out->kind = T_OR_OR; return true; }
            diag_help(L->a, L->diags, span_make(start, L->pos),
                      "Solis has no bitwise `|`; did you mean `||`?", "unexpected `|`");
            return false;
        case '~':
            diag_help(L->a, L->diags, span_make(start, L->pos),
                      "inequality is written `!=` in Solis, not `~=`", "unexpected `~`");
            return false;
        case '#':
            diag_help(L->a, L->diags, span_make(start, L->pos),
                      "comments start with `//`", "unexpected `#`");
            return false;
        default:
            // Report the whole character, not the first byte of it.
            L->pos = start + utf8_len(c);
            if (L->pos > L->len) L->pos = L->len;
            diag_addf(L->a, L->diags, span_make(start, L->pos),
                      "unexpected character `%.*s`",
                      (int)(L->pos - start), L->src + start);
            return false;
    }
}

token_vec lex(arena *a, sl_interner *in, const char *src, size_t len, diag_vec *diags) {
    lexer L;
    L.src = src;
    L.len = len;
    L.pos = 0;
    L.a = a;
    L.in = in;
    L.diags = diags;

    token_vec out;
    memset(&out, 0, sizeof out);

    for (;;) {
        skip_trivia(&L);
        size_t start = L.pos;
        if (at_end(&L)) {
            token t;
            memset(&t, 0, sizeof t);
            t.kind = T_EOF;
            t.at = span_make(start, start);
            vec_push(a, &out, t);
            break;
        }
        token t;
        memset(&t, 0, sizeof t);
        if (scan_token(&L, &t)) {
            t.at = span_make(start, L.pos);
            vec_push(a, &out, t);
        }
        // A failed scan already recorded a diagnostic; keep going.
    }
    return out;
}
