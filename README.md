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
## Why

Lua is an extremely popular pick for environments where you need a scripting language but do not want to do a lot of work to embed something. However, while Lua is a powerful and well designed language, as any language, it is not without its flaws. Most notably, the syntax. Lua chose a very weird and clunky syntax and memory model, which can often constrain how it is used and complicate both setup and usage of a project. Solis aims to fix those problems as well as multiple others. Solis uses a more familiar C-like syntax that should feel much more natural to programmers coming from any C-family language. Solis does not use a garbage collector, but rather opts for semi-automatic memory management, where memory is freed as it is able to be and allocated when it is needed. Another gripe many have with Lua is its typing system (or rather lack thereof), which Solis improves via static typing and a more object-oriented approach. And finally, Solis aims to have very descriptive errors which show and tell the problem, taking a page out of Rust's ideology.

## Building

You must install [Bloomery](bloomery.toml):
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

Run `solis --help` for the full option list.

## Embedding

A host links the VM from `runtime/` and registers native functions, with signatures, that scripts reach through `import host;`. The compiler checks every call against the registered types. A host can run precompiled `.slb` images, or link the compiler too and accept `.sl` source.

- [Embedding guide](docs/embedding.md): what to link, how to run a script, writing natives, ownership rules, sandboxing and errors.
- [`examples/embed/`](examples/embed): `build.cpp` compiles to `.slb`, `interpret.cpp` and `minimal.c` run from source (C++ and C), and `run_image.cpp` runs a precompiled image. Build them with `make -C examples/embed` or `bloomery embed`.
- [`runtime/example.cpp`](runtime/example.cpp) and [`examples/host.sl`](examples/host.sl) are the original precompiled-script example.

## Layout

- [`compiler/`](compiler): lexer, parser, type checker, bytecode emitter.
- [`runtime/`](runtime): the VM and standard library a host embeds.
- [`cli/`](cli): the `solis` command-line binary.
- [`std/`](std): the standard library, written in Solis itself.
- [`examples/`](examples): a language tour, a host-embedding example, a
  benchmark, a plugin system built on the prelude.
- [`benchmarks/`](benchmarks): Solis, C and Lua versions of the same workloads, `run.sh`, and [results](benchmarks/RESULTS.md).
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
