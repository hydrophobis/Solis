// The `hex` module: bytes as lowercase hexadecimal text.
//
// Strings carry bytes, so `encode("A")` is "41" and non-ASCII text gives one
// pair per UTF-8 byte.

// "hello" -> "68656c6c6f".
extern func encode(s: str): str;

// The reverse. Upper and lower case digits are both read. Empty when the
// text is not valid hex (odd length or a bad digit), so check with
// `isValid` first if the empty string is a possible answer.
extern func decode(s: str): str;

extern func isValid(s: str): bool;
