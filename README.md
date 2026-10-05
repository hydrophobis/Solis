# Solis

A small, statically typed, embeddable scripting language. C-family syntax,
no GC pauses, and great error handling.

```solis
struct Vec2 {
    x: float;
    y: float;

    func length(): float {
        return sqrt(this.x * this.x + this.y * this.y);
    }
}

func main() {
    let v = Vec2 { x: 3.0, y: 4.0 };
    print("length = ${v.length()}");
}
```
<small>Below content is partly AI generated</small>
## Why

Lua owns the embedded-scripting niche, and deservedly. Solis isn't trying to
dethrone it on vibes. It's betting on a few things Lua is genuinely weak at:
sandboxing and determinism (fuel metering, no ambient authority, hard memory
caps, so running untrusted mods is actually safe), no GC pauses (deterministic
memory behavior for realtime loops), good diagnostics (rendered, Rust/Elm-grade
error messages instead of a bare line number), and static types from the
start. Untyped tables make "what fields does this have?" an unsolvable
question for tooling; static types are what make a debugger and an LSP
possible later instead of permanently out of reach.

The design rationale, including what got cut and why, is in
[`docs/design.md`](docs/design.md).

## Building

You must install [Bloomery](bloomery.toml)():
```bash
pip install bloomery-build
bloomery --list
```

## Using it

```bash
solis script.sl                # interpret directly
solis -o script.slb script.sl  # compile to a bytecode image
solis script.slb                # run a compiled image
solis --check script.sl         # type check only, don't run
```

Run `solis --help` for the full option list. A host embeds `runtime/` as a
static library (`make lib`) and registers native functions for scripts to call
through `extern func` declarations. See [`runtime/example.cpp`](runtime/example.cpp)
and [`examples/host.sl`](examples/host.sl).

## Layout

- [`compiler/`](compiler): lexer, parser, type checker, bytecode emitter.
- [`runtime/`](runtime): the VM and standard library a host embeds.
- [`cli/`](cli): the `solis` command-line binary.
- [`std/`](std): the standard library, written in Solis itself.
- [`examples/`](examples): a language tour, a host-embedding example, a
  benchmark, a plugin system built on the prelude.
- [`tests/`](tests): output bytecode and expected output, checked by
  [`tools/conform.sh`](tools/conform.sh).

## "Vibe Coding" Disclosure

The following parts of this project were vibe coded (developed with substantial assistance from AI):

[**/docs**]: I'm lazy

[**/editors**]: I do not know vscode extensions well

[**/web**]: I do not know HTML well

[**various examples and tests**]: It is a large time saver

[**many comments and some documentation**]: It is a large time saver

All other parts of the project were developed without substantial AI assistance, unless otherwise noted.