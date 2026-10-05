# Solis for the web

Compiles and runs Solis scripts in the browser or Node, with host functions
exposed as plain JS functions — the same "plugin" story as
[`examples/plugins`](../examples/plugins), just with a JS host instead of a C
one.

What ships is the compiler plus the pure half of the runtime (`solis.c` +
`solis_std.c`, i.e. `math`/`strings`). `solis_os.c` (`io`/`fs`/`os`/`time`/
`rand` — real files, the environment, subprocesses) is deliberately left out:
a script in a browser tab should only be able to do what the host hands it
through `register()`.

## Build

Needs the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
(`emcc`) on `PATH` (`emsdk install latest && emsdk activate latest`).

```bash
web/build.sh
```

Verified working against emsdk 6.0.11: builds clean and
[smoke_test.mjs](smoke_test.mjs) (compile + run a script with JS-registered
natives via Node) passes.

Produces `web/solis_core.mjs` + `web/solis_core.wasm`. `web/solis.mjs` is the
hand-written wrapper apps actually import; it never needs rebuilding.

## Use

```js
import { Solis } from "./solis.mjs";

const vm = await Solis.create();

vm.register("print", (...parts) => console.log(parts.join(" ")));
vm.register("hostName", () => "web");
vm.register("nowMs", () => Date.now());

vm.writeFile("plugin.sl", `
    func main() {
        print("plugin running on ${hostName()}");
    }
`);
vm.run("plugin.sl");
```

A script that imports other modules, or expects a `prelude.sl` of ambient
declarations (see `examples/plugins/prelude.sl`), just needs those written to
the same virtual path with `writeFile()` before `run()`/`compile()`.

### Native functions

`register(name, fn)` binds a JS function as a Solis `extern`. Arguments
arrive as plain `number | boolean | string | undefined`; return one of those
(or throw, to fail the script's call with your message as the error).

Numbers cross as doubles: exact integers up to 2^53, same as any other JS
number. Arrays, structs and variants aren't marshaled yet — round-trip those
through a string (e.g. JSON) until that's added. The native pool is 32 slots
per VM (`SOLIS_NATIVE_POOL` in `solis_wasm.c`); bump it there and in
`solis.mjs` if a host needs more.

### Errors

`compile()`/`run()` throw with Solis's own rendered diagnostic (same text the
CLI prints) on a compile error, or the runtime's error message on a script
fault.
