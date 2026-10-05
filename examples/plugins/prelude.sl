// The host's declarations, written once.
//
// A file named prelude.sl is picked up automatically by every script in the
// directory beside it, and what it declares resolves unqualified. So the
// embedding application writes this file, and no plugin in here has to
// declare anything to call out to the host. `solis --prelude <file>` names one
// explicitly; `solis --no-prelude` ignores it.
//
// These three are what the standard interpreter registers for
// examples/host.sl, which declares them the long way round; compare the two.

import strings;

extern func nowMs(): int;
extern func hostName(): str;
extern func repeat(s: str, n: int): str;

// A prelude is an ordinary module, so it can carry more than declarations.
// Constants and functions resolve unqualified like the externs do; types are
// named `prelude.Thing`, because a bare type name has to keep meaning the
// local one.

let rule = "------------------------";

struct Limits {
    frameMs: int;

    static func default(): Limits {
        return Limits { frameMs: 16 };
    }
}

func shout(s: str): str {
    return strings.upper(s) + "!";
}
