// Native half of the text modules: bytes, hash, base64, hex and regex.
//
// Included from solis_std.c, which supplies the argument helpers and the
// UTF-8 routines used here. Everything in this file is deterministic and
// touches nothing outside its arguments, so a sandboxed host can open it.

// --- bytes ------------------------------------------------------------------

static sl_value s_bytes(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "bytes expects a string", &n);
    if (!p) return sl_void();
    sl_value arr = sl_array(vm, (int64_t)n + 1);
    if (arr.type != SL_OBJ) return sl_void();
    for (size_t i = 0; i < n; i++) {
        if (!sl_array_push(vm, arr, sl_int((unsigned char)p[i]))) { sl_release(vm, arr); return sl_void(); }
    }
    return arr;
}

static sl_value s_from_bytes(sl_vm *vm, int argc, sl_value *argv) {
    if (argc < 1 || argv[0].type != SL_OBJ || !argv[0].as.o) { sl_fail(vm, "fromBytes expects an array"); return sl_void(); }
    int64_t count = sl_array_len(argv[0]);
    char *buf = (char *)malloc((size_t)count + 1);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }
    for (int64_t i = 0; i < count; i++) {
        sl_value v = sl_array_get(argv[0], i);
        if (v.type != SL_INT || v.as.i < 0 || v.as.i > 255) {
            free(buf);
            sl_fail(vm, "fromBytes expects ints from 0 to 255");
            return sl_void();
        }
        buf[i] = (char)v.as.i;
    }
    sl_value out = sl_str(vm, buf, (size_t)count);
    free(buf);
    return out;
}

// --- hex and base64 -----------------------------------------------------------

static sl_value hex_value(sl_vm *vm, const unsigned char *p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    char *buf = (char *)malloc(n * 2 + 1);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }
    for (size_t i = 0; i < n; i++) {
        buf[2 * i] = digits[p[i] >> 4];
        buf[2 * i + 1] = digits[p[i] & 15];
    }
    sl_value out = sl_str(vm, buf, n * 2);
    free(buf);
    return out;
}

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static sl_value hex_encode(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "hex.encode expects a string", &n);
    if (!p) return sl_void();
    return hex_value(vm, (const unsigned char *)p, n);
}

// Decodes into `out` (n/2 bytes); false when the length is odd or a digit is bad.
static bool hex_decode_into(const char *p, size_t n, unsigned char *out) {
    if (n % 2) return false;
    for (size_t i = 0; i < n; i += 2) {
        int a = hex_digit_value(p[i]), b = hex_digit_value(p[i + 1]);
        if (a < 0 || b < 0) return false;
        out[i / 2] = (unsigned char)(a * 16 + b);
    }
    return true;
}

static sl_value hex_decode(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "hex.decode expects a string", &n);
    if (!p) return sl_void();
    unsigned char *buf = (unsigned char *)malloc(n / 2 + 1);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }
    sl_value out = hex_decode_into(p, n, buf) ? sl_str(vm, (const char *)buf, n / 2) : sl_cstr(vm, "");
    free(buf);
    return out;
}

static sl_value hex_is_valid(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "hex.isValid expects a string", &n);
    if (!p) return sl_void();
    unsigned char *buf = (unsigned char *)malloc(n / 2 + 1);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }
    bool ok = hex_decode_into(p, n, buf);
    free(buf);
    return sl_bool(ok);
}

static sl_value base64_encode_with(sl_vm *vm, int argc, sl_value *argv, const char *alphabet, bool pad) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "base64.encode expects a string", &n);
    if (!p) return sl_void();
    const unsigned char *u = (const unsigned char *)p;
    char *buf = (char *)malloc((n + 2) / 3 * 4 + 1);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }
    size_t o = 0, i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)u[i] << 16) | ((uint32_t)u[i + 1] << 8) | u[i + 2];
        buf[o++] = alphabet[(v >> 18) & 63]; buf[o++] = alphabet[(v >> 12) & 63];
        buf[o++] = alphabet[(v >> 6) & 63];  buf[o++] = alphabet[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)u[i] << 16;
        buf[o++] = alphabet[(v >> 18) & 63]; buf[o++] = alphabet[(v >> 12) & 63];
        if (pad) { buf[o++] = '='; buf[o++] = '='; }
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)u[i] << 16) | ((uint32_t)u[i + 1] << 8);
        buf[o++] = alphabet[(v >> 18) & 63]; buf[o++] = alphabet[(v >> 12) & 63];
        buf[o++] = alphabet[(v >> 6) & 63];
        if (pad) buf[o++] = '=';
    }
    sl_value out = sl_str(vm, buf, o);
    free(buf);
    return out;
}

static sl_value b64_encode(sl_vm *vm, int argc, sl_value *argv) {
    return base64_encode_with(vm, argc, argv, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", true);
}

static sl_value b64_encode_url(sl_vm *vm, int argc, sl_value *argv) {
    return base64_encode_with(vm, argc, argv, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", false);
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

// Both alphabets are accepted; padding is optional; whitespace is skipped.
static bool b64_decode_into(const char *p, size_t n, unsigned char *out, size_t *out_len) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0, symbols = 0;
    bool padding = false;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '=') { padding = true; continue; }
        if (padding) return false;
        int v = b64_value(c);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        symbols++;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    if (symbols % 4 == 1) return false;
    *out_len = o;
    return true;
}

static sl_value b64_decode(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "base64.decode expects a string", &n);
    if (!p) return sl_void();
    unsigned char *buf = (unsigned char *)malloc(n / 4 * 3 + 3);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }
    size_t len = 0;
    sl_value out = b64_decode_into(p, n, buf, &len) ? sl_str(vm, (const char *)buf, len) : sl_cstr(vm, "");
    free(buf);
    return out;
}

static sl_value b64_is_valid(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "base64.isValid expects a string", &n);
    if (!p) return sl_void();
    unsigned char *buf = (unsigned char *)malloc(n / 4 * 3 + 3);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }
    size_t len;
    bool ok = b64_decode_into(p, n, buf, &len);
    free(buf);
    return sl_bool(ok);
}

// --- hashes ---------------------------------------------------------------------

#define ROTL32(x, c) (((x) << (c)) | ((x) >> (32 - (c))))
#define ROTR32(x, c) (((x) >> (c)) | ((x) << (32 - (c))))

static void md5_digest(const unsigned char *msg, size_t n, unsigned char out[16]) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const int S[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    size_t total = ((n + 8) / 64 + 1) * 64;
    unsigned char *m = (unsigned char *)calloc(total, 1);
    if (!m) { memset(out, 0, 16); return; }
    memcpy(m, msg, n);
    m[n] = 0x80;
    uint64_t bitlen = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) m[total - 8 + (size_t)i] = (unsigned char)(bitlen >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[off + 4 * i] | ((uint32_t)m[off + 4 * i + 1] << 8) |
                   ((uint32_t)m[off + 4 * i + 2] << 16) | ((uint32_t)m[off + 4 * i + 3] << 24);
        uint32_t a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t f; int g;
            if (i < 16)      { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) % 16; }
            else             { f = c ^ (b | ~d);       g = (7 * i) % 16; }
            f = f + a + K[i] + w[g];
            a = d; d = c; c = b;
            b = b + ROTL32(f, S[i]);
        }
        a0 += a; b0 += b; c0 += c; d0 += d;
    }
    free(m);
    uint32_t r[4] = { a0, b0, c0, d0 };
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (unsigned char)(r[i] >> (8 * j));
}

static void sha1_digest(const unsigned char *msg, size_t n, unsigned char out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    size_t total = ((n + 8) / 64 + 1) * 64;
    unsigned char *m = (unsigned char *)calloc(total, 1);
    if (!m) { memset(out, 0, 20); return; }
    memcpy(m, msg, n);
    m[n] = 0x80;
    uint64_t bitlen = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) m[total - 1 - (size_t)i] = (unsigned char)(bitlen >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)m[off + 4 * i] << 24) | ((uint32_t)m[off + 4 * i + 1] << 16) |
                   ((uint32_t)m[off + 4 * i + 2] << 8) | m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = ROTL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t t = ROTL32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = ROTL32(b, 30); b = a; a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    free(m);
    uint32_t r[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (unsigned char)(r[i] >> (24 - 8 * j));
}

static void sha256_digest(const unsigned char *msg, size_t n, unsigned char out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    size_t total = ((n + 8) / 64 + 1) * 64;
    unsigned char *m = (unsigned char *)calloc(total, 1);
    if (!m) { memset(out, 0, 32); return; }
    memcpy(m, msg, n);
    m[n] = 0x80;
    uint64_t bitlen = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) m[total - 1 - (size_t)i] = (unsigned char)(bitlen >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)m[off + 4 * i] << 24) | ((uint32_t)m[off + 4 * i + 1] << 16) |
                   ((uint32_t)m[off + 4 * i + 2] << 8) | m[off + 4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ROTR32(w[i - 15], 7) ^ ROTR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ROTR32(w[i - 2], 17) ^ ROTR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    free(m);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (unsigned char)(h[i] >> (24 - 8 * j));
}

static sl_value h_md5(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "md5 expects a string", &n);
    if (!p) return sl_void();
    unsigned char d[16];
    md5_digest((const unsigned char *)p, n, d);
    return hex_value(vm, d, sizeof d);
}

static sl_value h_sha1(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "sha1 expects a string", &n);
    if (!p) return sl_void();
    unsigned char d[20];
    sha1_digest((const unsigned char *)p, n, d);
    return hex_value(vm, d, sizeof d);
}

static sl_value h_sha256(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "sha256 expects a string", &n);
    if (!p) return sl_void();
    unsigned char d[32];
    sha256_digest((const unsigned char *)p, n, d);
    return hex_value(vm, d, sizeof d);
}

static sl_value h_hmac_sha256(sl_vm *vm, int argc, sl_value *argv) {
    size_t kn, mn;
    const char *key = want_s(vm, argc, argv, 0, "hmacSha256 expects a key and a message", &kn);
    const char *msg = want_s(vm, argc, argv, 1, "hmacSha256 expects a key and a message", &mn);
    if (!key || !msg) return sl_void();
    unsigned char k[64];
    memset(k, 0, sizeof k);
    if (kn > 64) sha256_digest((const unsigned char *)key, kn, k);
    else memcpy(k, key, kn);
    unsigned char *inner = (unsigned char *)malloc(64 + mn);
    unsigned char outer[64 + 32];
    if (!inner) { sl_fail(vm, "out of memory"); return sl_void(); }
    for (int i = 0; i < 64; i++) { inner[i] = k[i] ^ 0x36; outer[i] = k[i] ^ 0x5c; }
    memcpy(inner + 64, msg, mn);
    sha256_digest(inner, 64 + mn, outer + 64);
    free(inner);
    unsigned char d[32];
    sha256_digest(outer, sizeof outer, d);
    return hex_value(vm, d, sizeof d);
}

static sl_value h_crc32(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "crc32 expects a string", &n);
    if (!p) return sl_void();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= (unsigned char)p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return sl_int((int64_t)(crc ^ 0xFFFFFFFFu));
}

// 64-bit FNV-1a, reinterpreted as a signed int.
static sl_value h_fnv1a(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *p = want_s(vm, argc, argv, 0, "fnv1a expects a string", &n);
    if (!p) return sl_void();
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 0x100000001b3ULL; }
    return sl_int((int64_t)h);
}

// --- regex ----------------------------------------------------------------------------
//
// A backtracking matcher over UTF-8 text. Syntax: literals, `.`, classes
// (`[a-z0-9_]`, `[^...]`), `\d \w \s` and their capitals, `\b \B`, `^ $`,
// groups `( )` and `(?: )`, alternation `|`, and the quantifiers `* + ? {n}
// {n,} {n,m}` with a trailing `?` for the lazy form. A leading `(?i)`, `(?s)`
// or `(?m)` sets a flag. Leftmost match, first alternative wins, like Perl and
// Python. A step budget turns pathological patterns into an error.

typedef struct { uint32_t lo, hi; } rx_range;
typedef struct { rx_range *r; int n, cap; bool neg; } rx_class;

enum { RX_CHAR, RX_ANY, RX_CLASS, RX_SPLIT, RX_JMP, RX_SAVE, RX_BOL, RX_EOL, RX_WORDB, RX_NWORDB,
       RX_MARK, RX_PROGRESS, RX_MATCH };

typedef struct { int op; uint32_t a; int x, y; } rx_inst;

typedef struct rx_node rx_node;
struct rx_node {
    int       kind;
    uint32_t  ch;
    int       cls, min, max, group;
    bool      lazy;
    rx_node  *a, *b;
};
enum { N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_CAT, N_ALT, N_REP, N_GROUP, N_BOL, N_EOL, N_WORDB, N_NWORDB };

typedef struct {
    rx_inst  *code;
    int       ncode, capcode;
    rx_class *cls;
    int       ncls, capcls;
    int       ngroups, nslots;
    bool      icase, dotall, multiline;
    char      err[96];
} rx_prog;

typedef struct {
    const char *p;
    size_t      n, i;
    rx_prog    *prog;
    rx_node   **nodes;
    int         nnodes, capnodes;
    bool        failed;
} rx_parser;

#define RX_MAX_CODE 20000
#define RX_MAX_REPEAT 1000

static void rx_fail(rx_parser *ps, const char *msg) {
    if (ps->failed) return;
    ps->failed = true;
    snprintf(ps->prog->err, sizeof ps->prog->err, "%s", msg);
}

static rx_node *rx_new(rx_parser *ps, int kind) {
    rx_node *nd = (rx_node *)calloc(1, sizeof *nd);
    if (!nd) { rx_fail(ps, "out of memory"); return NULL; }
    nd->kind = kind;
    nd->group = -1;
    if (ps->nnodes == ps->capnodes) {
        int cap = ps->capnodes ? ps->capnodes * 2 : 32;
        rx_node **bigger = (rx_node **)realloc(ps->nodes, (size_t)cap * sizeof *bigger);
        if (!bigger) { free(nd); rx_fail(ps, "out of memory"); return NULL; }
        ps->nodes = bigger;
        ps->capnodes = cap;
    }
    ps->nodes[ps->nnodes++] = nd;
    return nd;
}

static int rx_class_new(rx_parser *ps) {
    rx_prog *pg = ps->prog;
    if (pg->ncls == pg->capcls) {
        int cap = pg->capcls ? pg->capcls * 2 : 8;
        rx_class *bigger = (rx_class *)realloc(pg->cls, (size_t)cap * sizeof *bigger);
        if (!bigger) { rx_fail(ps, "out of memory"); return 0; }
        pg->cls = bigger;
        pg->capcls = cap;
    }
    memset(&pg->cls[pg->ncls], 0, sizeof pg->cls[0]);
    return pg->ncls++;
}

static void rx_class_add(rx_parser *ps, int ci, uint32_t lo, uint32_t hi) {
    rx_class *c = &ps->prog->cls[ci];
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 4;
        rx_range *bigger = (rx_range *)realloc(c->r, (size_t)cap * sizeof *bigger);
        if (!bigger) { rx_fail(ps, "out of memory"); return; }
        c->r = bigger;
        c->cap = cap;
    }
    c->r[c->n].lo = lo;
    c->r[c->n].hi = hi;
    c->n++;
}

// \d \w \s (or the complement) added to a class.
static void rx_class_shorthand(rx_parser *ps, int ci, char which) {
    static const rx_range digit[] = { { '0', '9' } };
    static const rx_range word[] = { { '0', '9' }, { 'A', 'Z' }, { '_', '_' }, { 'a', 'z' }, { 128, 0x10FFFF } };
    static const rx_range space[] = { { 9, 13 }, { ' ', ' ' } };
    const rx_range *set; int n;
    char lower = (char)(which | 0x20);
    if (lower == 'd') { set = digit; n = 1; }
    else if (lower == 'w') { set = word; n = 5; }
    else { set = space; n = 2; }
    if (which == lower) {
        for (int i = 0; i < n; i++) rx_class_add(ps, ci, set[i].lo, set[i].hi);
        return;
    }
    uint32_t next = 0;
    for (int i = 0; i < n; i++) {
        if (set[i].lo > next) rx_class_add(ps, ci, next, set[i].lo - 1);
        next = set[i].hi + 1;
    }
    rx_class_add(ps, ci, next, 0x10FFFF);
}

static bool rx_more(const rx_parser *ps) { return ps->i < ps->n; }

static uint32_t rx_next_cp(rx_parser *ps) {
    size_t w;
    uint32_t c = utf8_next(ps->p, ps->n, ps->i, &w);
    ps->i += w;
    return c;
}

// The code point after a backslash, for the escapes that name one character.
// Returns false when the escape is not a single character.
static bool rx_escape_char(rx_parser *ps, uint32_t c, uint32_t *out) {
    switch (c) {
        case 'n': *out = '\n'; return true;
        case 't': *out = '\t'; return true;
        case 'r': *out = '\r'; return true;
        case 'f': *out = '\f'; return true;
        case 'v': *out = '\v'; return true;
        case '0': *out = 0; return true;
        case 'x': {
            if (ps->i + 2 > ps->n) { rx_fail(ps, "bad \\x escape"); return true; }
            int a = hex_digit_value(ps->p[ps->i]), b = hex_digit_value(ps->p[ps->i + 1]);
            if (a < 0 || b < 0) { rx_fail(ps, "bad \\x escape"); return true; }
            ps->i += 2;
            *out = (uint32_t)(a * 16 + b);
            return true;
        }
        default:
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return false;
            *out = c;
            return true;
    }
}

static rx_node *rx_parse_alt(rx_parser *ps, int depth);

static rx_node *rx_parse_class(rx_parser *ps) {
    int ci = rx_class_new(ps);
    if (ps->failed) return NULL;
    if (rx_more(ps) && ps->p[ps->i] == '^') { ps->prog->cls[ci].neg = true; ps->i++; }
    bool first = true;
    for (;;) {
        if (!rx_more(ps)) { rx_fail(ps, "missing `]`"); return NULL; }
        if (ps->p[ps->i] == ']' && !first) { ps->i++; break; }
        first = false;
        uint32_t lo = rx_next_cp(ps);
        if (lo == '\\') {
            if (!rx_more(ps)) { rx_fail(ps, "dangling `\\`"); return NULL; }
            uint32_t e = rx_next_cp(ps);
            if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
                rx_class_shorthand(ps, ci, (char)e);
                continue;
            }
            if (e == 'b') lo = 8;
            else if (!rx_escape_char(ps, e, &lo)) { rx_fail(ps, "unknown escape in class"); return NULL; }
            if (ps->failed) return NULL;
        }
        uint32_t hi = lo;
        if (ps->i + 1 < ps->n && ps->p[ps->i] == '-' && ps->p[ps->i + 1] != ']') {
            ps->i++;
            hi = rx_next_cp(ps);
            if (hi == '\\') {
                if (!rx_more(ps)) { rx_fail(ps, "dangling `\\`"); return NULL; }
                uint32_t e = rx_next_cp(ps);
                if (!rx_escape_char(ps, e, &hi)) { rx_fail(ps, "bad range end"); return NULL; }
                if (ps->failed) return NULL;
            }
            if (hi < lo) { rx_fail(ps, "reversed range in class"); return NULL; }
        }
        rx_class_add(ps, ci, lo, hi);
        if (ps->failed) return NULL;
    }
    rx_node *nd = rx_new(ps, N_CLASS);
    if (nd) nd->cls = ci;
    return nd;
}

static rx_node *rx_parse_atom(rx_parser *ps, int depth) {
    uint32_t c = rx_next_cp(ps);
    rx_node *nd;
    switch (c) {
        case '(': {
            int group = -1;
            if (ps->i + 1 < ps->n && ps->p[ps->i] == '?' && ps->p[ps->i + 1] == ':') ps->i += 2;
            else group = ++ps->prog->ngroups;
            rx_node *inner = rx_parse_alt(ps, depth + 1);
            if (ps->failed) return NULL;
            if (!rx_more(ps) || ps->p[ps->i] != ')') { rx_fail(ps, "missing `)`"); return NULL; }
            ps->i++;
            nd = rx_new(ps, N_GROUP);
            if (!nd) return NULL;
            nd->a = inner;
            nd->group = group;
            return nd;
        }
        case '[': return rx_parse_class(ps);
        case '.': return rx_new(ps, N_ANY);
        case '^': return rx_new(ps, N_BOL);
        case '$': return rx_new(ps, N_EOL);
        case '*': case '+': case '?': rx_fail(ps, "nothing to repeat"); return NULL;
        case '\\': {
            if (!rx_more(ps)) { rx_fail(ps, "dangling `\\`"); return NULL; }
            uint32_t e = rx_next_cp(ps);
            if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
                int ci = rx_class_new(ps);
                if (ps->failed) return NULL;
                rx_class_shorthand(ps, ci, (char)e);
                nd = rx_new(ps, N_CLASS);
                if (nd) nd->cls = ci;
                return nd;
            }
            if (e == 'b') return rx_new(ps, N_WORDB);
            if (e == 'B') return rx_new(ps, N_NWORDB);
            uint32_t lit;
            if (!rx_escape_char(ps, e, &lit)) { rx_fail(ps, "unknown escape"); return NULL; }
            if (ps->failed) return NULL;
            nd = rx_new(ps, N_CHAR);
            if (nd) nd->ch = lit;
            return nd;
        }
        default:
            nd = rx_new(ps, N_CHAR);
            if (nd) nd->ch = c;
            return nd;
    }
}

static bool rx_read_int(rx_parser *ps, int *out) {
    size_t start = ps->i;
    long v = 0;
    while (rx_more(ps) && ps->p[ps->i] >= '0' && ps->p[ps->i] <= '9') {
        v = v * 10 + (ps->p[ps->i] - '0');
        if (v > 100000) v = 100000;
        ps->i++;
    }
    *out = (int)v;
    return ps->i > start;
}

static rx_node *rx_parse_repeat(rx_parser *ps, int depth) {
    rx_node *atom = rx_parse_atom(ps, depth);
    if (!atom || ps->failed) return NULL;
    for (;;) {
        if (!rx_more(ps)) return atom;
        char c = ps->p[ps->i];
        int min, max;
        if (c == '*') { min = 0; max = -1; ps->i++; }
        else if (c == '+') { min = 1; max = -1; ps->i++; }
        else if (c == '?') { min = 0; max = 1; ps->i++; }
        else if (c == '{') {
            size_t save = ps->i;
            ps->i++;
            int lo, hi;
            if (!rx_read_int(ps, &lo)) { ps->i = save; return atom; }   // a literal `{`
            hi = lo;
            if (rx_more(ps) && ps->p[ps->i] == ',') {
                ps->i++;
                if (!rx_read_int(ps, &hi)) hi = -1;
            }
            if (!rx_more(ps) || ps->p[ps->i] != '}') { ps->i = save; return atom; }
            ps->i++;
            if (lo > RX_MAX_REPEAT || hi > RX_MAX_REPEAT) { rx_fail(ps, "repeat count too large"); return NULL; }
            if (hi != -1 && hi < lo) { rx_fail(ps, "bad repeat range"); return NULL; }
            min = lo; max = hi;
        } else {
            return atom;
        }
        if (atom->kind == N_BOL || atom->kind == N_EOL || atom->kind == N_WORDB || atom->kind == N_NWORDB) {
            rx_fail(ps, "nothing to repeat");
            return NULL;
        }
        rx_node *rep = rx_new(ps, N_REP);
        if (!rep) return NULL;
        rep->a = atom; rep->min = min; rep->max = max;
        if (rx_more(ps) && ps->p[ps->i] == '?') { rep->lazy = true; ps->i++; }
        atom = rep;
    }
}

static rx_node *rx_parse_cat(rx_parser *ps, int depth) {
    rx_node *left = NULL;
    while (rx_more(ps) && ps->p[ps->i] != '|' && !(ps->p[ps->i] == ')' && depth > 0)) {
        rx_node *next = rx_parse_repeat(ps, depth);
        if (!next || ps->failed) return NULL;
        if (!left) { left = next; continue; }
        rx_node *cat = rx_new(ps, N_CAT);
        if (!cat) return NULL;
        cat->a = left; cat->b = next;
        left = cat;
    }
    return left ? left : rx_new(ps, N_EMPTY);
}

static rx_node *rx_parse_alt(rx_parser *ps, int depth) {
    if (depth > 200) { rx_fail(ps, "pattern nested too deeply"); return NULL; }
    rx_node *left = rx_parse_cat(ps, depth);
    if (!left || ps->failed) return NULL;
    while (rx_more(ps) && ps->p[ps->i] == '|') {
        ps->i++;
        rx_node *right = rx_parse_cat(ps, depth);
        if (!right || ps->failed) return NULL;
        rx_node *alt = rx_new(ps, N_ALT);
        if (!alt) return NULL;
        alt->a = left; alt->b = right;
        left = alt;
    }
    return left;
}

static bool rx_nullable(const rx_node *nd) {
    switch (nd->kind) {
        case N_CHAR: case N_ANY: case N_CLASS: return false;
        case N_CAT: return rx_nullable(nd->a) && rx_nullable(nd->b);
        case N_ALT: return rx_nullable(nd->a) || rx_nullable(nd->b);
        case N_REP: return nd->min == 0 || rx_nullable(nd->a);
        case N_GROUP: return rx_nullable(nd->a);
        default: return true;
    }
}

static int rx_emit(rx_parser *ps, int op, uint32_t a, int x, int y) {
    rx_prog *pg = ps->prog;
    if (pg->ncode >= RX_MAX_CODE) { rx_fail(ps, "pattern too large"); return 0; }
    if (pg->ncode == pg->capcode) {
        int cap = pg->capcode ? pg->capcode * 2 : 64;
        rx_inst *bigger = (rx_inst *)realloc(pg->code, (size_t)cap * sizeof *bigger);
        if (!bigger) { rx_fail(ps, "out of memory"); return 0; }
        pg->code = bigger;
        pg->capcode = cap;
    }
    pg->code[pg->ncode].op = op;
    pg->code[pg->ncode].a = a;
    pg->code[pg->ncode].x = x;
    pg->code[pg->ncode].y = y;
    return pg->ncode++;
}

static void rx_gen(rx_parser *ps, const rx_node *nd) {
    if (ps->failed || !nd) return;
    rx_prog *pg = ps->prog;
    switch (nd->kind) {
        case N_EMPTY: return;
        case N_CHAR: rx_emit(ps, RX_CHAR, nd->ch, 0, 0); return;
        case N_ANY: rx_emit(ps, RX_ANY, 0, 0, 0); return;
        case N_CLASS: rx_emit(ps, RX_CLASS, (uint32_t)nd->cls, 0, 0); return;
        case N_BOL: rx_emit(ps, RX_BOL, 0, 0, 0); return;
        case N_EOL: rx_emit(ps, RX_EOL, 0, 0, 0); return;
        case N_WORDB: rx_emit(ps, RX_WORDB, 0, 0, 0); return;
        case N_NWORDB: rx_emit(ps, RX_NWORDB, 0, 0, 0); return;
        case N_CAT: rx_gen(ps, nd->a); rx_gen(ps, nd->b); return;
        case N_ALT: {
            int split = rx_emit(ps, RX_SPLIT, 0, 0, 0);
            if (ps->failed) return;
            pg->code[split].x = pg->ncode;
            rx_gen(ps, nd->a);
            int jmp = rx_emit(ps, RX_JMP, 0, 0, 0);
            if (ps->failed) return;
            pg->code[split].y = pg->ncode;
            rx_gen(ps, nd->b);
            pg->code[jmp].x = pg->ncode;
            return;
        }
        case N_GROUP:
            if (nd->group >= 0) rx_emit(ps, RX_SAVE, (uint32_t)(2 * nd->group), 0, 0);
            rx_gen(ps, nd->a);
            if (nd->group >= 0) rx_emit(ps, RX_SAVE, (uint32_t)(2 * nd->group + 1), 0, 0);
            return;
        case N_REP: {
            for (int i = 0; i < nd->min && !ps->failed; i++) rx_gen(ps, nd->a);
            if (nd->max == -1) {
                bool guard = rx_nullable(nd->a);
                int slot = guard ? pg->nslots++ : 0;
                int split = rx_emit(ps, RX_SPLIT, 0, 0, 0);
                if (ps->failed) return;
                int body = pg->ncode;
                if (guard) rx_emit(ps, RX_MARK, (uint32_t)slot, 0, 0);
                rx_gen(ps, nd->a);
                int progress = guard ? rx_emit(ps, RX_PROGRESS, (uint32_t)slot, 0, 0) : 0;
                rx_emit(ps, RX_JMP, 0, split, 0);
                if (ps->failed) return;
                int exit = pg->ncode;
                if (guard) pg->code[progress].x = exit;
                pg->code[split].x = nd->lazy ? exit : body;
                pg->code[split].y = nd->lazy ? body : exit;
            } else {
                int *splits = (int *)malloc((size_t)(nd->max - nd->min + 1) * sizeof *splits);
                int count = 0;
                if (!splits) { rx_fail(ps, "out of memory"); return; }
                for (int i = nd->min; i < nd->max && !ps->failed; i++) {
                    int split = rx_emit(ps, RX_SPLIT, 0, 0, 0);
                    if (ps->failed) { free(splits); return; }
                    splits[count++] = split;
                    pg->code[split].x = pg->ncode;
                    rx_gen(ps, nd->a);
                }
                for (int i = 0; i < count; i++) {
                    if (nd->lazy) { pg->code[splits[i]].x = pg->ncode; pg->code[splits[i]].y = splits[i] + 1; }
                    else pg->code[splits[i]].y = pg->ncode;
                }
                free(splits);
            }
            return;
        }
    }
}

static void rx_free(rx_prog *pg) {
    if (!pg) return;
    for (int i = 0; i < pg->ncls; i++) free(pg->cls[i].r);
    free(pg->cls);
    free(pg->code);
    free(pg);
}

// NULL on a bad pattern is never returned: a program with `err` set comes back
// instead, so the caller can report the message.
static rx_prog *rx_compile(const char *pat, size_t n) {
    rx_prog *pg = (rx_prog *)calloc(1, sizeof *pg);
    if (!pg) return NULL;
    rx_parser ps;
    memset(&ps, 0, sizeof ps);
    ps.p = pat; ps.n = n; ps.prog = pg;
    // leading flag groups: (?i) (?s) (?m)
    for (;;) {
        if (ps.i + 3 < n && pat[ps.i] == '(' && pat[ps.i + 1] == '?') {
            size_t j = ps.i + 2;
            while (j < n && (pat[j] == 'i' || pat[j] == 's' || pat[j] == 'm')) j++;
            if (j > ps.i + 2 && j < n && pat[j] == ')') {
                for (size_t k = ps.i + 2; k < j; k++) {
                    if (pat[k] == 'i') pg->icase = true;
                    else if (pat[k] == 's') pg->dotall = true;
                    else pg->multiline = true;
                }
                ps.i = j + 1;
                continue;
            }
        }
        break;
    }
    rx_node *root = rx_parse_alt(&ps, 0);
    if (!ps.failed && rx_more(&ps)) rx_fail(&ps, "unmatched `)`");
    if (!ps.failed) {
        rx_emit(&ps, RX_SAVE, 0, 0, 0);
        rx_gen(&ps, root);
        rx_emit(&ps, RX_SAVE, 1, 0, 0);
        rx_emit(&ps, RX_MATCH, 0, 0, 0);
    }
    for (int i = 0; i < ps.nnodes; i++) free(ps.nodes[i]);
    free(ps.nodes);
    return pg;
}

typedef struct { int pc; int slot; size_t val; } rx_frame;   // pc >= 0: a branch; pc < 0: undo a slot

static bool rx_is_word(uint32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c > 127;
}

static uint32_t rx_fold(uint32_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool rx_class_has(const rx_class *cl, uint32_t c) {
    for (int i = 0; i < cl->n; i++)
        if (c >= cl->r[i].lo && c <= cl->r[i].hi) return true;
    return false;
}

static uint32_t rx_prev_cp(const char *s, size_t sp) {
    if (sp == 0) return 0;
    size_t j = sp - 1;
    while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) j--;
    size_t w;
    return utf8_next(s, sp, j, &w);
}

// 1 = matched (slots hold the group offsets), 0 = no match here, -1 = out of budget.
static int rx_run(const rx_prog *pg, const char *s, size_t n, size_t start, size_t *slots, long *budget,
                  rx_frame **stack, size_t *cap, size_t forbid_empty_at) {
    size_t nslots = (size_t)(2 * (pg->ngroups + 1) + pg->nslots);
    for (size_t i = 0; i < nslots; i++) slots[i] = (size_t)-1;
    size_t top = 0;
    int pc = 0;
    size_t sp = start;
    const size_t mark_base = (size_t)(2 * (pg->ngroups + 1));
    for (;;) {
        bool fail = false;
        if (--*budget < 0) return -1;
        const rx_inst *in = &pg->code[pc];
        switch (in->op) {
            case RX_CHAR: {
                if (sp >= n) { fail = true; break; }
                size_t w;
                uint32_t c = utf8_next(s, n, sp, &w);
                if (c == in->a || (pg->icase && rx_fold(c) == rx_fold(in->a))) { sp += w; pc++; }
                else fail = true;
                break;
            }
            case RX_ANY: {
                if (sp >= n) { fail = true; break; }
                size_t w;
                uint32_t c = utf8_next(s, n, sp, &w);
                if (c == '\n' && !pg->dotall) { fail = true; break; }
                sp += w; pc++;
                break;
            }
            case RX_CLASS: {
                if (sp >= n) { fail = true; break; }
                size_t w;
                uint32_t c = utf8_next(s, n, sp, &w);
                const rx_class *cl = &pg->cls[in->a];
                bool hit = rx_class_has(cl, c);
                if (!hit && pg->icase) {
                    uint32_t lo = rx_fold(c), up = (c >= 'a' && c <= 'z') ? c - 32 : c;
                    hit = rx_class_has(cl, lo) || rx_class_has(cl, up);
                }
                if (hit != cl->neg) { sp += w; pc++; }
                else fail = true;
                break;
            }
            case RX_SPLIT: {
                if (top + 1 >= *cap) {
                    if (*cap >= (size_t)4 << 20) return -1;
                    size_t ncap = *cap ? *cap * 2 : 256;
                    rx_frame *bigger = (rx_frame *)realloc(*stack, ncap * sizeof **stack);
                    if (!bigger) return -1;
                    *stack = bigger;
                    *cap = ncap;
                }
                (*stack)[top].pc = in->y; (*stack)[top].slot = 0; (*stack)[top].val = sp;
                top++;
                pc = in->x;
                break;
            }
            case RX_JMP: pc = in->x; break;
            case RX_SAVE: case RX_MARK: {
                size_t slot = in->op == RX_SAVE ? in->a : mark_base + in->a;
                if (top + 1 >= *cap) {
                    if (*cap >= (size_t)4 << 20) return -1;
                    size_t ncap = *cap ? *cap * 2 : 256;
                    rx_frame *bigger = (rx_frame *)realloc(*stack, ncap * sizeof **stack);
                    if (!bigger) return -1;
                    *stack = bigger;
                    *cap = ncap;
                }
                (*stack)[top].pc = -1; (*stack)[top].slot = (int)slot; (*stack)[top].val = slots[slot];
                top++;
                slots[slot] = sp;
                pc++;
                break;
            }
            case RX_PROGRESS:
                // An iteration that consumed nothing ends the loop instead of repeating.
                if (slots[mark_base + in->a] == sp) pc = in->x; else pc++;
                break;
            case RX_BOL:
                if (sp == 0 || (pg->multiline && s[sp - 1] == '\n')) pc++; else fail = true;
                break;
            case RX_EOL:
                if (sp == n || (pg->multiline && s[sp] == '\n')) pc++; else fail = true;
                break;
            case RX_WORDB: case RX_NWORDB: {
                bool before = sp > 0 && rx_is_word(rx_prev_cp(s, sp));
                bool after = false;
                if (sp < n) { size_t w; after = rx_is_word(utf8_next(s, n, sp, &w)); }
                bool boundary = before != after;
                if (boundary == (in->op == RX_WORDB)) pc++; else fail = true;
                break;
            }
            case RX_MATCH:
                // After an empty match the next one at the same place has to be longer.
                if (sp == start && start == forbid_empty_at) { fail = true; break; }
                return 1;
        }
        if (!fail) continue;
        for (;;) {
            if (top == 0) return 0;
            top--;
            if ((*stack)[top].pc < 0) { slots[(*stack)[top].slot] = (*stack)[top].val; continue; }
            pc = (*stack)[top].pc;
            sp = (*stack)[top].val;
            break;
        }
    }
}

typedef struct { rx_prog *pg; size_t *slots; rx_frame *stack; size_t cap; long budget; bool over; } rx_search;

static void rx_search_free(rx_search *rs) { free(rs->slots); free(rs->stack); }

static bool rx_search_init(rx_search *rs, rx_prog *pg) {
    memset(rs, 0, sizeof *rs);
    rs->pg = pg;
    rs->slots = (size_t *)malloc((size_t)(2 * (pg->ngroups + 1) + pg->nslots) * sizeof(size_t));
    rs->budget = 20000000L;
    return rs->slots != NULL;
}

// First match starting at or after byte `from`. Match offsets end up in rs->slots.
static bool rx_find_from(rx_search *rs, const char *s, size_t n, size_t from, size_t forbid_empty_at) {
    for (size_t at = from; at <= n; ) {
        int r = rx_run(rs->pg, s, n, at, rs->slots, &rs->budget, &rs->stack, &rs->cap, forbid_empty_at);
        if (r < 0) { rs->over = true; return false; }
        if (r == 1) return true;
        if (at >= n) break;
        at++;
        while (at < n && ((unsigned char)s[at] & 0xC0) == 0x80) at++;
    }
    return false;
}

static bool rx_find(rx_search *rs, const char *s, size_t n, size_t from) {
    return rx_find_from(rs, s, n, from, (size_t)-1);
}

// Small cache so a pattern used in a loop is compiled once.
static struct { char *pat; size_t n; rx_prog *pg; } rx_cache[8];
static int rx_cache_next;

static rx_prog *rx_get(sl_vm *vm, const char *pat, size_t n) {
    for (int i = 0; i < 8; i++)
        if (rx_cache[i].pg && rx_cache[i].n == n && memcmp(rx_cache[i].pat, pat, n) == 0) return rx_cache[i].pg;
    rx_prog *pg = rx_compile(pat, n);
    if (!pg) { sl_fail(vm, "out of memory"); return NULL; }
    if (pg->err[0]) {
        char msg[160];
        snprintf(msg, sizeof msg, "regex: %s", pg->err);
        rx_free(pg);
        sl_fail(vm, msg);
        return NULL;
    }
    int slot = rx_cache_next++ % 8;
    char *copy = (char *)malloc(n + 1);
    if (!copy) { rx_free(pg); sl_fail(vm, "out of memory"); return NULL; }
    memcpy(copy, pat, n);
    rx_free(rx_cache[slot].pg);
    free(rx_cache[slot].pat);
    rx_cache[slot].pat = copy;
    rx_cache[slot].n = n;
    rx_cache[slot].pg = pg;
    return pg;
}

// Compiles argv[0] and sets up a search over argv[1]. False after reporting.
static bool rx_open(sl_vm *vm, int argc, sl_value *argv, const char *who, rx_search *rs,
                    const char **text, size_t *tn) {
    size_t pn;
    const char *pat = want_s(vm, argc, argv, 0, who, &pn);
    *text = want_s(vm, argc, argv, 1, who, tn);
    if (!pat || !*text) return false;
    rx_prog *pg = rx_get(vm, pat, pn);
    if (!pg) return false;
    if (!rx_search_init(rs, pg)) { sl_fail(vm, "out of memory"); return false; }
    return true;
}

static void rx_budget_error(sl_vm *vm) { sl_fail(vm, "regex: pattern is too expensive on this text"); }

static sl_value rx_is_match(sl_vm *vm, int argc, sl_value *argv) {
    rx_search rs; const char *t; size_t tn;
    if (!rx_open(vm, argc, argv, "regex.isMatch expects a pattern and a string", &rs, &t, &tn)) return sl_void();
    bool hit = rx_find(&rs, t, tn, 0);
    bool over = rs.over;
    rx_search_free(&rs);
    if (over) { rx_budget_error(vm); return sl_void(); }
    return sl_bool(hit);
}

static sl_value rx_find_first(sl_vm *vm, int argc, sl_value *argv) {
    rx_search rs; const char *t; size_t tn;
    if (!rx_open(vm, argc, argv, "regex.find expects a pattern and a string", &rs, &t, &tn)) return sl_void();
    bool hit = rx_find(&rs, t, tn, 0);
    sl_value out = hit ? sl_str(vm, t + rs.slots[0], rs.slots[1] - rs.slots[0]) : sl_cstr(vm, "");
    bool over = rs.over;
    rx_search_free(&rs);
    if (over) { sl_release(vm, out); rx_budget_error(vm); return sl_void(); }
    return out;
}

static sl_value rx_index(sl_vm *vm, int argc, sl_value *argv) {
    rx_search rs; const char *t; size_t tn;
    if (!rx_open(vm, argc, argv, "regex.index expects a pattern and a string", &rs, &t, &tn)) return sl_void();
    bool hit = rx_find(&rs, t, tn, 0);
    int64_t at = hit ? char_count(t, rs.slots[0]) : -1;
    bool over = rs.over;
    rx_search_free(&rs);
    if (over) { rx_budget_error(vm); return sl_void(); }
    return sl_int(at);
}

static sl_value rx_group_array(sl_vm *vm, const rx_search *rs, const char *t) {
    int groups = rs->pg->ngroups;
    sl_value arr = sl_array(vm, groups + 1);
    if (arr.type != SL_OBJ) return sl_void();
    for (int g = 0; g <= groups; g++) {
        size_t a = rs->slots[2 * g], b = rs->slots[2 * g + 1];
        sl_value piece = (a == (size_t)-1 || b == (size_t)-1 || b < a) ? sl_cstr(vm, "") : sl_str(vm, t + a, b - a);
        if (!sl_array_push(vm, arr, piece)) { sl_release(vm, arr); return sl_void(); }
    }
    return arr;
}

static sl_value rx_groups(sl_vm *vm, int argc, sl_value *argv) {
    rx_search rs; const char *t; size_t tn;
    if (!rx_open(vm, argc, argv, "regex.groups expects a pattern and a string", &rs, &t, &tn)) return sl_void();
    bool hit = rx_find(&rs, t, tn, 0);
    sl_value out;
    if (hit) out = rx_group_array(vm, &rs, t);
    else out = sl_array(vm, 0);
    bool over = rs.over;
    rx_search_free(&rs);
    if (over) { sl_release(vm, out); rx_budget_error(vm); return sl_void(); }
    return out;
}

// Calls `each` for every non-overlapping match, left to right. After an empty
// match the next one may start at the same place if it is not empty, and an
// empty match right after a non-empty one counts, as in Python 3.7 and later.
typedef bool (*rx_each)(void *ctx, const rx_search *rs, size_t prev_end);

static bool rx_walk(rx_search *rs, const char *t, size_t tn, rx_each each, void *ctx, bool first_only) {
    size_t pos = 0, prev_end = 0, forbid = (size_t)-1;
    while (pos <= tn) {
        if (!rx_find_from(rs, t, tn, pos, forbid)) break;
        size_t a = rs->slots[0], b = rs->slots[1];
        if (!each(ctx, rs, prev_end)) return false;
        prev_end = b;
        if (first_only) break;
        pos = b;
        forbid = (b == a) ? b : (size_t)-1;
    }
    return true;
}

typedef struct { sl_vm *vm; sl_value arr; const char *t; bool all_groups; bool ok; } rx_collect;

static bool rx_collect_match(void *ctx, const rx_search *rs, size_t prev_end) {
    rx_collect *c = (rx_collect *)ctx;
    (void)prev_end;
    sl_value item = c->all_groups ? rx_group_array(c->vm, rs, c->t)
                                  : sl_str(c->vm, c->t + rs->slots[0], rs->slots[1] - rs->slots[0]);
    if (!sl_array_push(c->vm, c->arr, item)) { c->ok = false; return false; }
    return true;
}

static sl_value rx_collect_all(sl_vm *vm, int argc, sl_value *argv, bool all_groups, const char *who) {
    rx_search rs; const char *t; size_t tn;
    if (!rx_open(vm, argc, argv, who, &rs, &t, &tn)) return sl_void();
    rx_collect c = { vm, sl_array(vm, 4), t, all_groups, true };
    if (c.arr.type != SL_OBJ) { rx_search_free(&rs); return sl_void(); }
    rx_walk(&rs, t, tn, rx_collect_match, &c, false);
    bool over = rs.over;
    rx_search_free(&rs);
    if (over || !c.ok) {
        sl_release(vm, c.arr);
        if (over) rx_budget_error(vm); else sl_fail(vm, "out of memory");
        return sl_void();
    }
    return c.arr;
}

static sl_value rx_find_all(sl_vm *vm, int argc, sl_value *argv) {
    return rx_collect_all(vm, argc, argv, false, "regex.findAll expects a pattern and a string");
}

static sl_value rx_all_groups(sl_vm *vm, int argc, sl_value *argv) {
    return rx_collect_all(vm, argc, argv, true, "regex.allGroups expects a pattern and a string");
}

typedef struct { char *buf; size_t len, cap; bool ok; } rx_out;

static void rx_out_put(rx_out *o, const char *p, size_t n) {
    if (!o->ok) return;
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 64;
        while (o->len + n + 1 > cap) cap *= 2;
        char *bigger = (char *)realloc(o->buf, cap);
        if (!bigger) { o->ok = false; return; }
        o->buf = bigger;
        o->cap = cap;
    }
    memcpy(o->buf + o->len, p, n);
    o->len += n;
}

typedef struct {
    rx_out *out; const char *t; const char *repl; size_t rn; size_t copied;
} rx_subst;

static bool rx_subst_match(void *ctx, const rx_search *rs, size_t prev_end) {
    rx_subst *s = (rx_subst *)ctx;
    (void)prev_end;
    rx_out_put(s->out, s->t + s->copied, rs->slots[0] - s->copied);
    for (size_t i = 0; i < s->rn; i++) {
        char c = s->repl[i];
        if (c == '$' && i + 1 < s->rn) {
            char d = s->repl[i + 1];
            if (d == '$') { rx_out_put(s->out, "$", 1); i++; continue; }
            if (d >= '0' && d <= '9') {
                int g = d - '0';
                if (g <= rs->pg->ngroups) {
                    size_t a = rs->slots[2 * g], b = rs->slots[2 * g + 1];
                    if (a != (size_t)-1 && b != (size_t)-1 && b >= a) rx_out_put(s->out, s->t + a, b - a);
                }
                i++;
                continue;
            }
        }
        rx_out_put(s->out, &c, 1);
    }
    s->copied = rs->slots[1];
    return s->out->ok;
}

static sl_value rx_replace_with(sl_vm *vm, int argc, sl_value *argv, bool first_only, const char *who) {
    rx_search rs; const char *t; size_t tn;
    if (!rx_open(vm, argc, argv, who, &rs, &t, &tn)) return sl_void();
    size_t rn; const char *repl = want_s(vm, argc, argv, 2, who, &rn);
    if (!repl) { rx_search_free(&rs); return sl_void(); }
    rx_out out = { NULL, 0, 0, true };
    rx_subst s = { &out, t, repl, rn, 0 };
    rx_walk(&rs, t, tn, rx_subst_match, &s, first_only);
    rx_out_put(&out, t + s.copied, tn - s.copied);
    bool over = rs.over;
    rx_search_free(&rs);
    if (over || !out.ok) {
        free(out.buf);
        if (over) rx_budget_error(vm); else sl_fail(vm, "out of memory");
        return sl_void();
    }
    sl_value result = sl_str(vm, out.buf ? out.buf : "", out.len);
    free(out.buf);
    return result;
}

static sl_value rx_replace(sl_vm *vm, int argc, sl_value *argv) {
    return rx_replace_with(vm, argc, argv, false, "regex.replace expects a pattern and two strings");
}

static sl_value rx_replace_first(sl_vm *vm, int argc, sl_value *argv) {
    return rx_replace_with(vm, argc, argv, true, "regex.replaceFirst expects a pattern and two strings");
}

typedef struct { sl_vm *vm; sl_value arr; const char *t; size_t from; bool ok; } rx_splitter;

static bool rx_split_match(void *ctx, const rx_search *rs, size_t prev_end) {
    rx_splitter *sp = (rx_splitter *)ctx;
    (void)prev_end;
    sl_value piece = sl_str(sp->vm, sp->t + sp->from, rs->slots[0] - sp->from);
    if (!sl_array_push(sp->vm, sp->arr, piece)) { sp->ok = false; return false; }
    sp->from = rs->slots[1];
    return true;
}

static sl_value rx_split(sl_vm *vm, int argc, sl_value *argv) {
    rx_search rs; const char *t; size_t tn;
    if (!rx_open(vm, argc, argv, "regex.split expects a pattern and a string", &rs, &t, &tn)) return sl_void();
    rx_splitter sp = { vm, sl_array(vm, 4), t, 0, true };
    if (sp.arr.type != SL_OBJ) { rx_search_free(&rs); return sl_void(); }
    rx_walk(&rs, t, tn, rx_split_match, &sp, false);
    bool over = rs.over;
    rx_search_free(&rs);
    if (!over && sp.ok && !sl_array_push(vm, sp.arr, sl_str(vm, t + sp.from, tn - sp.from))) sp.ok = false;
    if (over || !sp.ok) {
        sl_release(vm, sp.arr);
        if (over) rx_budget_error(vm); else sl_fail(vm, "out of memory");
        return sl_void();
    }
    return sp.arr;
}

static sl_value rx_is_valid(sl_vm *vm, int argc, sl_value *argv) {
    size_t pn; const char *pat = want_s(vm, argc, argv, 0, "regex.isValid expects a pattern", &pn);
    if (!pat) return sl_void();
    rx_prog *pg = rx_compile(pat, pn);
    if (!pg) { sl_fail(vm, "out of memory"); return sl_void(); }
    bool ok = pg->err[0] == 0;
    rx_free(pg);
    return sl_bool(ok);
}

// The number of capture groups in a pattern.
static sl_value rx_group_count(sl_vm *vm, int argc, sl_value *argv) {
    size_t pn; const char *pat = want_s(vm, argc, argv, 0, "regex.groupCount expects a pattern", &pn);
    if (!pat) return sl_void();
    rx_prog *pg = rx_get(vm, pat, pn);
    if (!pg) return sl_void();
    return sl_int(pg->ngroups);
}

#define TEXT_NATIVES \
        { "strings.bytes",       s_bytes }, \
        { "strings.fromBytes",   s_from_bytes }, \
        { "hex.encode",          hex_encode }, \
        { "hex.decode",          hex_decode }, \
        { "hex.isValid",         hex_is_valid }, \
        { "base64.encode",       b64_encode }, \
        { "base64.encodeUrl",    b64_encode_url }, \
        { "base64.decode",       b64_decode }, \
        { "base64.isValid",      b64_is_valid }, \
        { "hash.md5",            h_md5 }, \
        { "hash.sha1",           h_sha1 }, \
        { "hash.sha256",         h_sha256 }, \
        { "hash.hmacSha256",     h_hmac_sha256 }, \
        { "hash.crc32",          h_crc32 }, \
        { "hash.fnv1a",          h_fnv1a }, \
        { "regex.isMatch",       rx_is_match }, \
        { "regex.find",          rx_find_first }, \
        { "regex.index",         rx_index }, \
        { "regex.groups",        rx_groups }, \
        { "regex.findAll",       rx_find_all }, \
        { "regex.allGroups",     rx_all_groups }, \
        { "regex.replace",       rx_replace }, \
        { "regex.replaceFirst",  rx_replace_first }, \
        { "regex.split",         rx_split }, \
        { "regex.isValid",       rx_is_valid }, \
        { "regex.groupCount",    rx_group_count },
