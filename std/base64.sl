// The `base64` module.
//
//     base64.encode("hello")      // "aGVsbG8="
//     base64.decode("aGVsbG8=")   // "hello"
//
// `encodeUrl` uses the URL-safe alphabet (`-` and `_`) and leaves off the
// padding, the form used in JWTs and URLs. `decode` accepts either alphabet,
// with or without padding, and skips whitespace.

extern func encode(s: str): str;
extern func encodeUrl(s: str): str;

// Empty when the text is not valid base64; use `isValid` to tell that from
// an encoded empty string.
extern func decode(s: str): str;

extern func isValid(s: str): bool;
