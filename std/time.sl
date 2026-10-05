// The `time` module: the clock. Ambient authority, so only the standard
// interpreter opens it; a sandboxed host registers its own clock if needed.

import strings;

// Milliseconds since the Unix epoch, UTC.
extern func nowMs(): int;

// Milliseconds since the interpreter started. Use this for measuring
// durations; it does not jump when the system clock is corrected.
extern func monoMs(): int;

extern func sleepMs(ms: int);

// The current time as ISO 8601 in UTC, "2026-10-04T18:33:07Z".
extern func iso(): str;

func nowSec(): int {
    return nowMs() / 1000;
}

// Today as "2026-10-04".
func today(): str {
    return strings.sub(iso(), 0, 10);
}
