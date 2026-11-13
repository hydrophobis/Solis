# Embedding Solis

This guide is for developers putting Solis inside an application such as a game, a tool or a server. It covers what to link, how to run a script, how scripts reach your application, and what happens when something fails. Runnable versions of the code are in [`examples/embed/`](../examples/embed).

## 1. What you are embedding

Solis has two independent pieces.

| Piece | Files | What it does | Needed when |
|---|---|---|---|
| Runtime (the VM) | `runtime/solis.c`, `solis_std.c`, `solis_os.c`, header `solis.h` (C++ wrapper `solis.hpp`) | Loads and runs a bytecode image. `sl_load` translates each function to register code once, so load time is linear in image size. | Always. |
| Compiler | `compiler/*.c` (not `cli/solis.c`), header `solis_compile.h` (C++ wrapper `solis_compile.hpp`) | Turns `.sl` source into a bytecode image, with readable errors. | Only if your application accepts source. |

That gives you two ways to embed, and you can choose per project:

- Precompiled. Compile scripts to `.slb` images at build time with the `solis` CLI or [`examples/embed/build.cpp`](../examples/embed/build.cpp), and ship only the VM. Your binary doesn't contain the compiler. See [`run_image.cpp`](../examples/embed/run_image.cpp).
- From source. Link the compiler too and compile at runtime, so users can drop in `.sl` files. See [`interpret.cpp`](../examples/embed/interpret.cpp) (C++) and [`minimal.c`](../examples/embed/minimal.c) (C).

| Example | Links | Does |
|---|---|---|
| `build.cpp` | compiler + VM | source to `.slb` file (`--host file` for `import host;`) |
| `interpret.cpp` | compiler + VM | source to running script (C++) |
| `minimal.c` | compiler + VM | source to running script (C) |
| `run_image.cpp` | VM only | `.slb` to running script |

Build them all with `make -C examples/embed`.

## 2. Quick start

### C

```c
#include "solis.h"
#include "solis_compile.h"

sl_vm *vm = sl_new();
sl_open_std(vm);                          // math and strings; optional
sl_register(vm, "print", my_print);       // see section 4
sl_register_sig(vm, "nowMs", "()->int", my_now_ms);   // with a signature: checked, and importable

char *host = sl_declarations(vm);         // the `host` module, written from what you registered
sl_compiled c;
int bad = sl_compile_file_with_host("script.sl", NULL, host, &c);
free(host);
if (bad) {
    fputs(c.errors, stderr);              // file, line, source excerpt, caret
    sl_compiled_free(&c);
    sl_free(vm);
    return 1;
}

if (sl_load(vm, c.image, c.image_len) != SL_OK) { /* sl_error(vm) */ }
else if (sl_run(vm) != SL_OK)                   { /* sl_error(vm) */ }

sl_compiled_free(&c);
sl_free(vm);
```

The script:

```solis
import host;

func main() {
    print("t = ${host.nowMs()}");
}
```

### C++17

```cpp
#include "solis.hpp"
#include "solis_compile.hpp"

try {
    solis::Vm vm;
    vm.openStd();
    vm.define("print", [](solis::Vm &, solis::Args args) {
        for (int i = 0; i < args.size(); i++) std::cout << args[i].toString();
        std::cout << '\n';
        return solis::Value();
    });
    vm.define("nowMs", "()->int", [](solis::Vm &, solis::Args) { return solis::Value::integer(0); });

    auto image = solis::compile("script.sl", nullptr, vm.declarations());
    vm.load(image);                       // throws if the script and the host disagree
    vm.run();
} catch (const solis::CompileError &e) {  // e.what() is the rendered diagnostics
} catch (const solis::Error &e) {         // bad image, or the script faulted
}
```

### Linking

Compile the C files with a C compiler (the compiler and runtime are C23) and link them into your program. A C++ program can link the objects directly. `examples/embed/Makefile` does exactly this.

For the VM alone there is a single-file option: `python tools/amalgamate.py` writes `build/runtime.c`, which holds the VM and the standard library. It leaves out `solis_os.c`, so `sl_open_os` doesn't exist and scripts have no library access to the filesystem, the clock or io.

## 3. How a script sees your application

A script can only call what you registered. There are three ways for it to name those functions, and the first is the one to use.

### `import host;`

Register your natives with signatures, then ask the VM for the `host` module and give it to the compiler:

```c
char *host = sl_declarations(vm);
sl_compile_file_with_host("script.sl", NULL, host, &c);
free(host);
```

```cpp
auto image = solis::compile("script.sl", nullptr, vm.declarations());
```

A script then imports it and calls what you registered, with no declarations of its own:

```solis
import host;

func main() {
    print("running on ${host.hostName()}");
    print(host.repeat("-", 10));
}
```

The module is written from your signatures, so the compiler checks every call against the types you registered. `host.repeat(5, "x")` does not compile: `expected str, found int`. There is no second copy of the signature in the script to get out of date. `solis --show-host` prints the module the interpreter offers:

```
extern func nowMs(): int;
extern func hostName(): str;
extern func repeat(p0: str, p1: int): str;
```

Things to know about it:

- Only natives registered with a signature (`sl_register_sig`, `define(name, sig, fn)`) appear. Natives without one are left out, since there is nothing to declare them with.
- Parameters are named `p0`, `p1` and so on, because a signature has types but no names.
- A signature that names a user struct or enum is left out, and a comment in the module says so. Those types belong to the script that defines them, and a host module cannot define them.
- Register your natives before you call `sl_declarations`: it describes what is registered at that moment.
- `import host;` only resolves when you pass a host module. Otherwise the compiler looks for a `host.sl` file.
- The module applies to imports only. A script file that is itself called `host.sl` is still just a script.
- To compile without a VM, for example at build time, write `vm.declarations()` to a file from your application and pass it to `examples/embed/build --host file`.

### `extern func` in the script

A script can still declare each function it calls itself:

```solis
extern func nowMs(): int;
extern func hostName(): str;

func main() {
    print("running on ${hostName()}");
}
```

This works, and `sl_load` checks the declaration against your registered signature (section 4). The script and the host each state the type, though, and nothing makes them agree until load.

### The prelude

Declaring the same externs in every script gets repetitive, so a file named `prelude.sl` beside a script is picked up automatically, and everything it declares resolves unqualified in that script and every module it imports. See [`examples/plugins/`](../examples/plugins): `mod.sl` calls `hostName()` and `repeat()` without declaring either.

`sl_compile_file(path, prelude, ...)` controls this:

| `prelude` | Meaning |
|---|---|
| `NULL` | use a `prelude.sl` next to the script, if there is one |
| `""` | no prelude |
| a path | use that file (it must exist) |

A prelude is an ordinary module, so it can also hold constants and helper functions, not only declarations. It works through an implicit import and has no type connection to your registered natives, so prefer `import host;` for anything that talks to the host.

## 4. Writing natives

A native is a C function with this signature:

```c
sl_value my_native(sl_vm *vm, int argc, sl_value *argv);
```

Register it with `sl_register(vm, "name", my_native)`. The name must match the script's `extern func name(...)`.

Arguments are borrowed: read them freely, and `sl_retain` any you keep past the call. The return value is owned by the VM, so you hand it over.

```c
static sl_value host_name(sl_vm *vm, int argc, sl_value *argv) {
    (void)argc; (void)argv;
    return sl_cstr(vm, "my-app");            // a new string, ownership goes to the VM
}

static sl_value add(sl_vm *vm, int argc, sl_value *argv) {
    if (argc != 2 || argv[0].type != SL_INT || argv[1].type != SL_INT) {
        sl_fail(vm, "add(int, int) expects two ints");   // becomes a script runtime error
        return sl_void();
    }
    return sl_int(argv[0].as.i + argv[1].as.i);
}
```

Scalars (`sl_int`, `sl_float`, `sl_bool`, `sl_void`) are plain values with nothing to free. Strings, arrays, maps, structs and variants are reference counted heap objects (`SL_OBJ`).

### Signatures

Register a native with the signature the script declares for it, and `sl_load` checks the two against each other:

```c
sl_register_sig(vm, "repeat", "(str,int)->str", host_repeat);
```

```cpp
vm.define("repeat", "(str,int)->str", [](solis::Vm &v, solis::Args a) { /* ... */ });
```

This is the script's `extern func repeat(s: str, n: int): str;` written as `(parameter types)->result type`. A function with no result is `->()`, so `extern func log(msg: str);` is `(str)->()`. Types are `int`, `float`, `bool`, `str`, arrays as `[T]`, maps as `{K: V}`, and your own structs and enums as `module.Name`. Spaces are ignored.

If they disagree, or the script calls something you did not register, the program does not load. `sl_load` returns `SL_ERR_LINK` and `sl_error` lists every problem at once:

```
cannot link the program to this host:
  `hostName`: the script declares ()->int but the host registered ()->str
  `ghost` (int)->int is declared extern but the host did not register it
```

A script's types are trusted. If a native registered without a signature returns a string while the script declared an `int`, nothing stops the script from reading the string's pointer as a number, and it will print one. The signature is the only guard, so give every native one.

What is not checked:

- A native registered with plain `sl_register` or `define(name, fn)`, with no signature. It links without a type check.
- The runtime's own natives: the standard library and builtins such as `print`.
- What your function actually does. A native registered as `(str)->int` that returns a float will still break the script; the signature is a promise you have to keep.

Two scripts that declare the same host function with different types are a compile error, because they would share one native.

### Ownership rules

| Call | You get | Then |
|---|---|---|
| `sl_str`, `sl_cstr`, `sl_array`, `sl_map` | a new object | you own it; return it, or `sl_release` it |
| `sl_to_string` | a new string | you own it; `sl_release` it or it leaks |
| `sl_array_push/set/insert`, `sl_map_set` | | they take ownership of what you pass in; do not release it afterwards |
| `sl_array_get`, `sl_array_pop`, `sl_array_remove` | the element | handed to you; release it when done |
| `sl_map_get` | the stored value, borrowed | do not release; retain to keep |
| `sl_map_remove` | key and value | both owned by you |

Two details about strings and maps:

- `sl_as_str(v, &len)` returns a pointer and a length. Use the length; do not assume the data is NUL-terminated.
- Map lookup is hashed (a plain scan while a map holds eight pairs or fewer). Pairs iterate in insertion order, and removing a key rebuilds the index, so heavy removal from a large map costs O(n) each.

### Errors from natives

- In C, call `sl_fail(vm, "message")` and return `sl_void()`. Execution stops, `sl_run` returns `SL_ERR_RUNTIME`, and `sl_error(vm)` holds your message.
- In C++, throw any `std::exception` (the examples use `solis::Error`). The wrapper catches it and converts it to `sl_fail`, so no exception crosses into the VM. A native that throws `Error("host says no")` makes `vm.run()` throw `host says no`.

### The C++ wrapper

`solis::Vm::define(name, callable)` accepts any callable with the signature `Value(Vm &, Args)`, including lambdas that capture your application's state, which the raw C function pointer cannot do:

```cpp
int frame = 0;
vm.define("frame", [&frame](solis::Vm &, solis::Args) { return solis::Value::integer(frame); });
```

`Value` handles reference counting for you (copy retains, destruction releases). `Value::integer/number/boolean`, `vm.string()`, `vm.array()` build values; `isInt()`, `asInt()`, `asString()`, `toString()`, `size()`, `at(i)` read them.

A `Vm` accepts at most 128 natives, and `vm.nativesRemaining()` tells you how many are left. The runtime uses some itself: a fresh VM had 115 free after `sl_new`, and `openStd()` took 37 of them.

## 5. What a script can reach

By default, almost nothing. Scripts get only:

- what you register with `sl_register` / `define`;
- the libraries you open: `sl_open_std` gives `math` and `strings`; `sl_open_os(vm, argc, argv)` gives `io`, `fs`, `os`, `time` and `rand`.

`print` is not built in. The host registers it, as every example does. If you forget, the program does not load, and the link error names it.

A missing native fails at load, not at the call. Every native the program calls has to be registered, including ones on a path that never runs. `sl_load` returns `SL_ERR_LINK` with the full list (see Signatures above), so you find out before any script code has run.

To run untrusted scripts, leave out `sl_open_os`, register only natives you are comfortable exposing, and consider the amalgamated runtime, which does not contain `solis_os.c` at all.

## 6. Errors, in the order they happen

| Stage | Where | You see |
|---|---|---|
| Compile | `sl_compile_file` returns non-zero | `c.errors`: rendered diagnostics with file, line, a source excerpt and a caret, plus `c.error_count`. C++ throws `solis::CompileError`. |
| Load | `sl_load` returns `SL_ERR_BADFILE` or `SL_ERR_LINK` | `sl_error(vm)`: `not a Solis bytecode image`, a version mismatch, or the link problems listed under Signatures. |
| Run | `sl_run` returns `SL_ERR_RUNTIME` | `sl_error(vm)`: for example `index 10 is out of bounds for an array of length 3`, or the message from your `sl_fail`. |

Compile errors include a missing file (`cannot read module`) and import problems. A type error looks like:

```
error: expected `int`, found `str`
 --> script.sl:2:18
  |
2 |     let x: int = "oops";
  |                  ^^^^^^
```

Images carry a format version (currently 6), and a runtime refuses an image with a different one. Rebuild your `.slb` files when you update Solis.

## 7. Memory and leak checking

Solis uses reference counting. After a program finishes, `sl_live_objects(vm)` (`vm.liveObjects()` in C++) reports how many heap objects are still alive. In tests it should be 0 unless the script made a reference cycle.

Globals a script holds stay alive until the VM is freed, so a script whose prelude defines a global string reports 1 at exit even though nothing leaks. `examples/plugins/mod.sl` shows this, and the CLI's `--check-leaks` reports it too. For scripts with globals, compare counts between runs instead of expecting 0.

Your natives can leak too: one that calls `sl_to_string` and forgets `sl_release` shows up in the count.

## 8. Other notes

- Keep each VM on one thread. Nothing in `solis.h` is documented as thread-safe.
- `sl_load` copies the image, so you can free the compiler output right after loading. Under AddressSanitizer, freeing the image before `sl_run` caused no errors.
- Calling `sl_load` again on the same VM loaded and ran the second program in testing. Use a fresh `sl_new` when scripts shouldn't share state, since it also resets your registered natives and globals.
- `sl_compile_file` takes a path because imports resolve relative to the script. There is no compile-from-string call yet.
