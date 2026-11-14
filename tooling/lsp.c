// sl lsp: a language server for Solis, speaking the language server protocol
// over standard input and output.
//
// It is an ordinary host of the compiler library. Everything it knows about the
// language comes from solis_analysis.h and solis_syntax.h; the code here is the
// protocol, the open-document store, and conversions between the library's byte
// offsets and the protocol's UTF-16 positions.
//
// Supported: diagnostics, hover, go to definition, find references, completion
// (triggered by '.'), and document symbols. Documents are synchronised in full.
// Open buffers shadow the files on disk, so an import of a file being edited sees
// the unsaved text.

#include "json.h"
#include "solis_analysis.h"
#include "solis_compile.h"
#include "solis_syntax.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

#define SERVER_VERSION "0.1.0"

// ---- strings and files ----------------------------------------------------------

static char *dup_n(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) { fprintf(stderr, "sl lsp: out of memory\n"); exit(1); }
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
static char *dup_str(const char *s) { return dup_n(s, strlen(s)); }

static char *read_disk(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (1L << 30)) { fclose(f); return NULL; }   // a directory reports a huge size
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

// file:///a/b%20c.sl <-> /a/b c.sl
static char *uri_to_path(const char *uri) {
    if (strncmp(uri, "file://", 7) != 0) return dup_str(uri);
    const char *p = uri + 7;
    size_t n = strlen(p);
    char *out = (char *)malloc(n + 1);
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '%' && i + 2 < n && isxdigit((unsigned char)p[i + 1]) && isxdigit((unsigned char)p[i + 2])) {
            char hex[3] = { p[i + 1], p[i + 2], 0 };
            out[w++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[w++] = p[i];
        }
    }
    out[w] = 0;
#ifdef _WIN32
    // file:///C:/dir -> C:/dir; the client may also send the colon escaped
    if (out[0] == '/' && isalpha((unsigned char)out[1]) && out[2] == ':') memmove(out, out + 1, w);
    for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
    if (isalpha((unsigned char)out[0]) && out[1] == ':') out[0] = (char)tolower((unsigned char)out[0]);
#endif
    return out;
}

static char *path_to_uri(const char *path) {
    sbuf b = { 0 };
    sb_puts(&b, "file://");
#ifdef _WIN32
    if (isalpha((unsigned char)path[0]) && path[1] == ':') sb_puts(&b, "/");
#endif
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (isalnum(*p) || strchr("/-_.~", *p)) sb_putn(&b, (const char *)p, 1);
#ifdef _WIN32
        else if (*p == '\\') sb_putn(&b, "/", 1);
        else if (*p == ':' && p == (const unsigned char *)path + 1) sb_putn(&b, ":", 1);
#endif
        else sb_printf(&b, "%%%02X", *p);
    }
    return b.at;
}

// ---- positions ----------------------------------------------------------------------
//
// The library speaks byte offsets and counts columns in characters. The protocol
// counts UTF-16 code units. Both directions go through the text.

static size_t utf8_seq(unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }

static uint32_t off_from_lsp(const char *t, size_t n, uint32_t line, uint32_t ch) {
    size_t i = 0;
    for (uint32_t l = 0; l < line && i < n; ) if (t[i++] == '\n') l++;
    uint32_t units = 0;
    while (i < n && t[i] != '\n' && units < ch) {
        size_t k = utf8_seq((unsigned char)t[i]);
        units += k == 4 ? 2 : 1;
        i += k;
    }
    return (uint32_t)(i < n ? i : n);
}

static void lsp_from_off(const char *t, size_t n, uint32_t off, uint32_t *line, uint32_t *ch) {
    uint32_t l = 0, units = 0;
    size_t i = 0;
    if (off > n) off = (uint32_t)n;
    while (i < off) {
        if (t[i] == '\n') { l++; units = 0; i++; continue; }
        size_t k = utf8_seq((unsigned char)t[i]);
        units += k == 4 ? 2 : 1;
        i += k;
    }
    *line = l;
    *ch = units;
}

// A diagnostic's 1-based line and character column to protocol coordinates.
static void lsp_from_cp(const char *t, size_t n, uint32_t line1, uint32_t col1, uint32_t *line, uint32_t *ch) {
    uint32_t off = sl_line_col_to_offset(t, n, line1, col1);
    lsp_from_off(t, n, off, line, ch);
}

// ---- documents --------------------------------------------------------------------------

typedef struct {
    char        *uri, *path, *text;
    size_t       len;
    int          version;
    sl_analysis *an;            // the latest analysis; may not have type-checked
    sl_analysis *good;          // the latest analysis that did, and the text it saw
    char        *good_text;
    size_t       good_len;
} doc;

static doc **docs;
static int ndocs;

static doc *find_uri(const char *uri) {
    for (int i = 0; i < ndocs; i++) if (strcmp(docs[i]->uri, uri) == 0) return docs[i];
    return NULL;
}
static doc *find_path(const char *path) {
    for (int i = 0; i < ndocs; i++) if (strcmp(docs[i]->path, path) == 0) return docs[i];
    return NULL;
}

// ---- the last good analysis ----------------------------------------------------
//
// While the buffer does not parse the checker does not run, so queries are
// answered from the last analysis that did, translated across the edit. The edit
// is taken to be the one span between the common prefix and common suffix of
// the old and new text; offsets inside it have no counterpart.

static bool is_fresh(const doc *d) { return d->an && sl_analysis_checked(d->an); }

static sl_analysis *query_analysis(const doc *d) { return is_fresh(d) ? d->an : d->good; }

static void edit_span(const doc *d, size_t *prefix, size_t *suffix) {
    size_t a = d->good_len, b = d->len, m = a < b ? a : b, p = 0, s = 0;
    while (p < m && d->good_text[p] == d->text[p]) p++;
    while (s < m - p && d->good_text[a - 1 - s] == d->text[b - 1 - s]) s++;
    *prefix = p;
    *suffix = s;
}

// Offset in the current text to the text the analysis saw, and back.
static bool to_analysed(const doc *d, uint32_t in, uint32_t *out) {
    if (is_fresh(d)) { *out = in; return true; }
    if (!d->good) return false;
    size_t p, s;
    edit_span(d, &p, &s);
    if (in <= p) { *out = in; return true; }
    if (in >= d->len - s) { *out = (uint32_t)(in + d->good_len - d->len); return true; }
    return false;
}
static bool from_analysed(const doc *d, uint32_t in, uint32_t *out) {
    if (is_fresh(d)) { *out = in; return true; }
    size_t p, s;
    edit_span(d, &p, &s);
    if (in <= p) { *out = in; return true; }
    if (in >= d->good_len - s) { *out = (uint32_t)(in + d->len - d->good_len); return true; }
    return false;
}

// Open buffers shadow disk.
static char *hook_read(void *user, const char *path, size_t *len) {
    (void)user;
    doc *d = find_path(path);
    if (d) { *len = d->len; return dup_n(d->text, d->len); }
    return read_disk(path, len);
}
static int hook_exists(void *user, const char *path) {
    (void)user;
    if (find_path(path)) return 1;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static sl_analysis *analyze_text(const doc *d, const char *text) {
    sl_compile_options o;
    memset(&o, 0, sizeof o);
    o.entry_source = text;
    o.read = hook_read;
    o.exists = hook_exists;
    return sl_analyze(d->path, &o);
}

// ---- output -----------------------------------------------------------------------------------

static void send_message(const sbuf *body) {
    printf("Content-Length: %zu\r\n\r\n", body->len);
    fwrite(body->at, 1, body->len, stdout);
    fflush(stdout);
}

static void write_id(sbuf *b, const jv *id) {
    if (id && id->kind == J_STR) sb_json_string(b, id->str, id->len);
    else if (id && id->kind == J_NUM) sb_printf(b, "%.0f", id->num);
    else sb_puts(b, "null");
}

static void reply(const jv *id, const char *result_json) {
    sbuf b = { 0 };
    sb_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(&b, id);
    sb_puts(&b, ",\"result\":");
    sb_puts(&b, result_json);
    sb_puts(&b, "}");
    send_message(&b);
    sb_free(&b);
}

static void reply_error(const jv *id, int code, const char *message) {
    sbuf b = { 0 };
    sb_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(&b, id);
    sb_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
    sb_json_string(&b, message, strlen(message));
    sb_puts(&b, "}}");
    send_message(&b);
    sb_free(&b);
}

static void notify(const char *method, const char *params_json) {
    sbuf b = { 0 };
    sb_printf(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}", method, params_json);
    send_message(&b);
    sb_free(&b);
}

static void put_range(sbuf *b, const char *text, size_t len, uint32_t start, uint32_t end) {
    uint32_t l0, c0, l1, c1;
    lsp_from_off(text, len, start, &l0, &c0);
    lsp_from_off(text, len, end, &l1, &c1);
    sb_printf(b, "{\"start\":{\"line\":%u,\"character\":%u},\"end\":{\"line\":%u,\"character\":%u}}", l0, c0, l1, c1);
}

// Writes a Location, or nothing and false if it fell inside a span edited since
// the analysis ran. `from` is the document the analysis belongs to.
static bool put_location(sbuf *b, const doc *from, const sl_location *loc) {
    size_t len = 0;
    doc *d = find_path(loc->file);
    char *owned = NULL;
    const char *text;
    uint32_t start = loc->start, end = loc->end;
    if (d) {
        text = d->text; len = d->len;
        if (d == from && (!from_analysed(d, loc->start, &start) || !from_analysed(d, loc->end, &end))) return false;
    } else {
        owned = read_disk(loc->file, &len);
        text = owned ? owned : "";
    }
    char *uri = d ? dup_str(d->uri) : path_to_uri(loc->file);
    sb_puts(b, "{\"uri\":");
    sb_json_string(b, uri, strlen(uri));
    sb_puts(b, ",\"range\":");
    put_range(b, text, len, start, end);
    sb_puts(b, "}");
    free(uri);
    free(owned);
    return true;
}

// ---- diagnostics ------------------------------------------------------------------------------------

static void publish_diagnostics(doc *d) {
    sbuf b = { 0 };
    char *uri = d->uri;
    sb_puts(&b, "{\"uri\":");
    sb_json_string(&b, uri, strlen(uri));
    sb_puts(&b, ",\"version\":");
    sb_printf(&b, "%d", d->version);
    sb_puts(&b, ",\"diagnostics\":[");
    int n = 0;
    const sl_diagnostic *ds = d->an ? sl_analysis_diagnostics(d->an, &n) : NULL;
    bool first = true;
    for (int i = 0; i < n; i++) {
        const sl_diagnostic *x = &ds[i];
        const char *text = d->text;
        size_t len = d->len;
        sbuf message = { 0 };
        uint32_t l0, c0, l1, c1;
        if (strcmp(x->file, d->path) == 0) {
            lsp_from_cp(text, len, x->line, x->col, &l0, &c0);
            lsp_from_cp(text, len, x->end_line, x->end_col, &l1, &c1);
            sb_puts(&message, x->message);
        } else {
            // An error in a file this one imports. That file reports it itself
            // when it is open; otherwise show it at the top of this one.
            if (find_path(x->file)) { sb_free(&message); continue; }
            const char *base = strrchr(x->file, '/');
            l0 = c0 = l1 = c1 = 0;
            sb_printf(&message, "%s:%u:%u: %s", base ? base + 1 : x->file, x->line, x->col, x->message);
        }
        if (x->help) sb_printf(&message, "\n%s", x->help);
        if (!first) sb_puts(&b, ",");
        first = false;
        sb_printf(&b, "{\"range\":{\"start\":{\"line\":%u,\"character\":%u},\"end\":{\"line\":%u,\"character\":%u}},"
                      "\"severity\":1,\"source\":\"sl\",\"message\":", l0, c0, l1, c1);
        sb_json_string(&b, message.at ? message.at : "", message.len);
        sb_puts(&b, "}");
        sb_free(&message);
    }
    sb_puts(&b, "]}");
    notify("textDocument/publishDiagnostics", b.at);
    sb_free(&b);
}

static void refresh(doc *d) {
    if (d->an != d->good) sl_analysis_free(d->an);
    d->an = analyze_text(d, d->text);
    if (is_fresh(d)) {          // d->good and d->an then are the same analysis
        sl_analysis_free(d->good);
        free(d->good_text);
        d->good = d->an;
        d->good_text = dup_n(d->text, d->len);
        d->good_len = d->len;
    }
    publish_diagnostics(d);
}

// A change in one file can change what another open file's errors are.
static void refresh_all(void) {
    for (int i = 0; i < ndocs; i++) refresh(docs[i]);
}

// ---- requests -------------------------------------------------------------------------------------------

static int completion_kind(const sl_completion *c) {
    switch (c->kind) {
        case SL_ENT_LOCAL: case SL_ENT_PARAM: return 6;
        case SL_ENT_FUNC: case SL_ENT_BUILTIN: return 3;
        case SL_ENT_CONST: return 21;
        case SL_ENT_TYPE: return strncmp(c->detail, "enum", 4) == 0 ? 13 : 7;
        case SL_ENT_FIELD: return 5;
        case SL_ENT_METHOD: return 2;
        case SL_ENT_VARIANT: return 20;
        case SL_ENT_MODULE: return 9;
        case SL_ENT_KEYWORD: return 14;
    }
    return 1;
}

static int symbol_kind(sl_symbol_kind k) {
    switch (k) {
        case SL_SYM_FUNC: case SL_SYM_EXTERN: return 12;
        case SL_SYM_STRUCT: return 23;
        case SL_SYM_ENUM: return 10;
        case SL_SYM_INTERFACE: return 11;
        case SL_SYM_LET: return 13;
        case SL_SYM_IMPORT: return 2;
        case SL_SYM_FIELD: return 8;
        case SL_SYM_METHOD: return 6;
        case SL_SYM_VARIANT: return 22;
    }
    return 13;
}

// The document and offset a textDocument/* request is about.
static bool position_params(const jv *params, doc **d, uint32_t *offset) {
    const jv *td = jget(params, "textDocument");
    const jv *pos = jget(params, "position");
    *d = find_uri(jstr(jget(td, "uri"), ""));
    if (!*d || !pos) return false;
    *offset = off_from_lsp((*d)->text, (*d)->len, (uint32_t)jnum(jget(pos, "line"), 0), (uint32_t)jnum(jget(pos, "character"), 0));
    return true;
}

static void on_hover(const jv *id, const jv *params) {
    doc *d; uint32_t off, aoff, ustart, uend; sl_entity e;
    if (!position_params(params, &d, &off) || !to_analysed(d, off, &aoff) || !query_analysis(d) ||
        !sl_analysis_entity_at(query_analysis(d), d->path, aoff, &e)) { reply(id, "null"); return; }
    sbuf text = { 0 };
    if (*e.detail) sb_puts(&text, e.detail);
    else sb_printf(&text, "%s%s%s", e.name, *e.type ? ": " : "", e.type);
    sbuf b = { 0 };
    sb_puts(&b, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
    sbuf md = { 0 };
    sb_printf(&md, "```\n%s\n```", text.at);
    sb_json_string(&b, md.at, md.len);
    sb_puts(&b, "}");
    if (from_analysed(d, e.use.start, &ustart) && from_analysed(d, e.use.end, &uend)) {
        sb_puts(&b, ",\"range\":");
        put_range(&b, d->text, d->len, ustart, uend);
    }
    sb_puts(&b, "}");
    reply(id, b.at);
    sb_free(&b); sb_free(&md); sb_free(&text);
}

static void on_definition(const jv *id, const jv *params) {
    doc *d; uint32_t off, aoff; sl_entity e;
    if (!position_params(params, &d, &off) || !to_analysed(d, off, &aoff) || !query_analysis(d) ||
        !sl_analysis_entity_at(query_analysis(d), d->path, aoff, &e) || !e.has_definition) {
        reply(id, "null");
        return;
    }
    sbuf b = { 0 };
    if (!put_location(&b, d, &e.definition)) { reply(id, "null"); sb_free(&b); return; }
    reply(id, b.at);
    sb_free(&b);
}

static void on_references(const jv *id, const jv *params) {
    doc *d; uint32_t off, aoff; sl_entity e;
    if (!position_params(params, &d, &off) || !to_analysed(d, off, &aoff) || !query_analysis(d) ||
        !sl_analysis_entity_at(query_analysis(d), d->path, aoff, &e)) { reply(id, "[]"); return; }
    bool include_decl = jbool(jget(jget(params, "context"), "includeDeclaration"), true);
    enum { MAX = 2000 };
    sl_location *locs = (sl_location *)malloc(MAX * sizeof *locs);
    int n = sl_analysis_references(query_analysis(d), &e, locs, MAX);
    if (n > MAX) n = MAX;
    sbuf b = { 0 };
    sb_puts(&b, "[");
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (!include_decl && e.has_definition && strcmp(locs[i].file, e.definition.file) == 0 &&
            locs[i].start == e.definition.start && locs[i].end == e.definition.end) continue;
        size_t mark = b.len;
        if (!first) sb_puts(&b, ",");
        if (!put_location(&b, d, &locs[i])) { b.len = mark; b.at[mark] = 0; continue; }
        first = false;
    }
    sb_puts(&b, "]");
    reply(id, b.at);
    sb_free(&b);
    free(locs);
}

static void on_completion(const jv *id, const jv *params) {
    doc *d; uint32_t off;
    if (!position_params(params, &d, &off)) { reply(id, "null"); return; }
    // Code being typed rarely parses; complete against a copy where it does.
    size_t plen = 0;
    char *patched = sl_completion_source(d->text, d->len, off, &plen);
    sl_analysis *an = analyze_text(d, patched ? patched : d->text);
    uint32_t qoff = off;
    if (!an || !sl_analysis_checked(an)) {
        // Something besides the placeholder is wrong; use the last good analysis.
        sl_analysis_free(an);
        an = NULL;
        if (!to_analysed(d, off, &qoff)) qoff = (uint32_t)-1;
    }
    sl_analysis *use = an ? an : (qoff == (uint32_t)-1 ? NULL : d->good);
    sbuf b = { 0 };
    sb_puts(&b, "{\"isIncomplete\":false,\"items\":[");
    if (use) {
        int n;
        const sl_completion *c = sl_analysis_complete(use, d->path, qoff, &n);
        for (int i = 0; i < n; i++) {
            if (i) sb_puts(&b, ",");
            sb_puts(&b, "{\"label\":");
            sb_json_string(&b, c[i].label, strlen(c[i].label));
            sb_printf(&b, ",\"kind\":%d,\"detail\":", completion_kind(&c[i]));
            sb_json_string(&b, c[i].detail, strlen(c[i].detail));
            sb_puts(&b, "}");
        }
    }
    sb_puts(&b, "]}");
    reply(id, b.at);
    sb_free(&b);
    sl_analysis_free(an);
    free(patched);
}

static void put_symbol(sbuf *b, const char *text, size_t len, const sl_symbol *sy, int n, int i) {
    sb_puts(b, "{\"name\":");
    sb_json_string(b, sy[i].name, strlen(sy[i].name));
    sb_printf(b, ",\"kind\":%d,\"range\":", symbol_kind(sy[i].kind));
    put_range(b, text, len, sy[i].start, sy[i].end);
    sb_puts(b, ",\"selectionRange\":");
    uint32_t ns = sy[i].name_start, ne = sy[i].name_end;
    if (ns < sy[i].start || ne > sy[i].end || ne < ns) { ns = sy[i].start; ne = sy[i].start; }
    put_range(b, text, len, ns, ne);
    bool any = false;
    for (int k = 0; k < n; k++) {
        if (sy[k].parent != i) continue;
        sb_puts(b, any ? "," : ",\"children\":[");
        any = true;
        put_symbol(b, text, len, sy, n, k);
    }
    if (any) sb_puts(b, "]");
    sb_puts(b, "}");
}

static void on_document_symbol(const jv *id, const jv *params) {
    doc *d = find_uri(jstr(jget(jget(params, "textDocument"), "uri"), ""));
    if (!d) { reply(id, "[]"); return; }
    sl_syntax *s = sl_syntax_parse(d->text, d->len);
    sbuf b = { 0 };
    sb_puts(&b, "[");
    if (s) {
        int n;
        const sl_symbol *sy = sl_syntax_symbols(s, &n);
        bool first = true;
        for (int i = 0; i < n; i++) {
            if (sy[i].parent != -1) continue;
            if (!first) sb_puts(&b, ",");
            first = false;
            put_symbol(&b, d->text, d->len, sy, n, i);
        }
    }
    sb_puts(&b, "]");
    reply(id, b.at);
    sb_free(&b);
    sl_syntax_free(s);
}

// ---- notifications --------------------------------------------------------------------------------------------

static void on_did_open(const jv *params) {
    const jv *td = jget(params, "textDocument");
    const char *uri = jstr(jget(td, "uri"), NULL);
    const jv *text = jget(td, "text");
    if (!uri || !text || text->kind != J_STR) return;
    doc *d = find_uri(uri);
    if (!d) {
        d = (doc *)calloc(1, sizeof *d);
        d->uri = dup_str(uri);
        d->path = uri_to_path(uri);
        docs = (doc **)realloc(docs, (size_t)(ndocs + 1) * sizeof *docs);
        docs[ndocs++] = d;
    } else {
        free(d->text);
    }
    d->text = dup_n(text->str, text->len);
    d->len = text->len;
    d->version = (int)jnum(jget(td, "version"), 0);
    refresh_all();
}

static void on_did_change(const jv *params) {
    const jv *td = jget(params, "textDocument");
    doc *d = find_uri(jstr(jget(td, "uri"), ""));
    const jv *changes = jget(params, "contentChanges");
    if (!d || !changes || changes->n == 0) return;
    const jv *last = jidx(changes, changes->n - 1);
    const jv *text = jget(last, "text");
    if (!text || text->kind != J_STR) return;
    free(d->text);
    d->text = dup_n(text->str, text->len);
    d->len = text->len;
    d->version = (int)jnum(jget(td, "version"), d->version + 1);
    refresh_all();
}

static void on_did_close(const jv *params) {
    doc *d = find_uri(jstr(jget(jget(params, "textDocument"), "uri"), ""));
    if (!d) return;
    // clear what was shown for it
    sbuf b = { 0 };
    sb_puts(&b, "{\"uri\":");
    sb_json_string(&b, d->uri, strlen(d->uri));
    sb_puts(&b, ",\"diagnostics\":[]}");
    notify("textDocument/publishDiagnostics", b.at);
    sb_free(&b);
    for (int i = 0; i < ndocs; i++)
        if (docs[i] == d) { memmove(&docs[i], &docs[i + 1], (size_t)(ndocs - i - 1) * sizeof *docs); ndocs--; break; }
    if (d->an != d->good) sl_analysis_free(d->an);
    sl_analysis_free(d->good);
    free(d->good_text);
    free(d->uri); free(d->path); free(d->text); free(d);
    refresh_all();
}

// ---- the loop ------------------------------------------------------------------------------------------------------

static bool shutting_down = false;

static const char *CAPABILITIES =
    "{\"capabilities\":{"
    "\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":{\"includeText\":false}},"
    "\"hoverProvider\":true,\"definitionProvider\":true,\"referencesProvider\":true,"
    "\"documentSymbolProvider\":true,"
    "\"completionProvider\":{\"triggerCharacters\":[\".\"]}},"
    "\"serverInfo\":{\"name\":\"sl\",\"version\":\"" SERVER_VERSION "\"}}";

static void dispatch(const jv *msg) {
    const char *method = jstr(jget(msg, "method"), NULL);
    const jv *id = jget(msg, "id");
    const jv *params = jget(msg, "params");
    if (!method) return;                       // a response to us; we send no requests

    if (strcmp(method, "initialize") == 0) { reply(id, CAPABILITIES); return; }
    if (strcmp(method, "initialized") == 0) return;
    if (strcmp(method, "shutdown") == 0) { shutting_down = true; reply(id, "null"); return; }
    if (strcmp(method, "exit") == 0) exit(shutting_down ? 0 : 1);

    if (strcmp(method, "textDocument/didOpen") == 0) { on_did_open(params); return; }
    if (strcmp(method, "textDocument/didChange") == 0) { on_did_change(params); return; }
    if (strcmp(method, "textDocument/didClose") == 0) { on_did_close(params); return; }
    if (strcmp(method, "textDocument/didSave") == 0) { refresh_all(); return; }

    if (strcmp(method, "textDocument/hover") == 0) { on_hover(id, params); return; }
    if (strcmp(method, "textDocument/definition") == 0) { on_definition(id, params); return; }
    if (strcmp(method, "textDocument/references") == 0) { on_references(id, params); return; }
    if (strcmp(method, "textDocument/completion") == 0) { on_completion(id, params); return; }
    if (strcmp(method, "textDocument/documentSymbol") == 0) { on_document_symbol(id, params); return; }

    if (id) reply_error(id, -32601, "method not supported");     // notifications we don't know are ignored
}

// One framed message from standard input, or NULL at end of input.
static char *read_message(size_t *len) {
    char line[512];
    long content = -1;
    for (;;) {
        if (!fgets(line, sizeof line, stdin)) return NULL;
        if (line[0] == '\r' || line[0] == '\n') break;
        if (strncasecmp(line, "Content-Length:", 15) == 0) content = atol(line + 15);
    }
    if (content < 0 || content > 64 * 1024 * 1024) return NULL;
    char *body = (char *)malloc((size_t)content + 1);
    if (!body) return NULL;
    size_t got = fread(body, 1, (size_t)content, stdin);
    if (got != (size_t)content) { free(body); return NULL; }
    body[got] = 0;
    *len = got;
    return body;
}

int lsp_main(void) {
#ifdef _WIN32
    // the framing counts bytes; text mode would turn \r\n into \n
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    for (;;) {
        size_t len;
        char *body = read_message(&len);
        if (!body) return shutting_down ? 0 : 1;
        jpool pool = { 0 };
        jv *msg = json_parse(&pool, body, len);
        if (msg) dispatch(msg);
        else reply_error(NULL, -32700, "parse error");
        jpool_free(&pool);
        free(body);
    }
}
