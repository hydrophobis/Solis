// The `hash` module: checksums and digests of a string's bytes.
//
// The digests come back as lowercase hex, the form every tool prints.
// `md5` and `sha1` are here for checksums and compatibility with older
// formats; use `sha256` for anything security-related.

extern func md5(s: str): str;
extern func sha1(s: str): str;
extern func sha256(s: str): str;

// HMAC-SHA256 of `message` under `key`, as hex.
extern func hmacSha256(key: str, message: str): str;

// CRC-32 (the zip/PNG polynomial), 0 to 4294967295.
extern func crc32(s: str): int;

// 64-bit FNV-1a. The unsigned result is shown as a signed int, so about
// half of the values are negative. Handy for hash-table style bucketing.
extern func fnv1a(s: str): int;
