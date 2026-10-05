// Ergonomic JS wrapper around solis_core.mjs (built by build.sh).
//
//   import { Solis } from "./solis.mjs";
//   const vm = await Solis.create();
//   vm.register("print", (...parts) => console.log(parts.join(" ")));
//   vm.register("hostName", () => "web");
//   vm.writeFile("plugin.sl", source);
//   vm.run("plugin.sl");
//
// Numbers cross the JS/Solis boundary as doubles, so integers are exact up
// to 2^53 (same ceiling as any other JS number) and lose precision beyond
// it. Strings are copied both ways. Arrays, structs and variants are not
// marshaled yet -- a native that needs them has to encode/decode through a
// string (e.g. JSON) for now.
import createSolisCore from "./solis_core.mjs";

const NATIVE_POOL = 32; // must match SOLIS_NATIVE_POOL in web/solis_wasm.c

function readArg(core, argv, i) {
    const type = core.ccall("solis_arg_type", "number", ["number", "number"], [argv, i]);
    switch (type) {
        case 0: return undefined; // SL_VOID
        case 1: return core.ccall("solis_arg_int", "number", ["number", "number"], [argv, i]);
        case 2: return core.ccall("solis_arg_float", "number", ["number", "number"], [argv, i]);
        case 3: return !!core.ccall("solis_arg_bool", "number", ["number", "number"], [argv, i]);
        case 4: { // SL_OBJ -- strings only
            const lenPtr = core._malloc(4);
            const ptr = core.ccall(
                "solis_arg_cstr", "number", ["number", "number", "number"], [argv, i, lenPtr]);
            const len = core.getValue(lenPtr, "i32");
            core._free(lenPtr);
            if (!ptr) throw new Error("native argument is a non-string object (unsupported)");
            return core.UTF8ToString(ptr, len);
        }
        default: throw new Error(`unknown Solis value type ${type}`);
    }
}

function writeResult(core, vm, result) {
    if (result === undefined || result === null) {
        core.ccall("solis_ret_void", null, [], []);
    } else if (typeof result === "boolean") {
        core.ccall("solis_ret_bool", null, ["number"], [result ? 1 : 0]);
    } else if (typeof result === "number") {
        if (Number.isInteger(result)) core.ccall("solis_ret_int", null, ["number"], [result]);
        else core.ccall("solis_ret_float", null, ["number"], [result]);
    } else if (typeof result === "string") {
        core.ccall("solis_ret_str", null, ["number", "string", "number"], [vm, result, -1]);
    } else {
        throw new Error(`native returned an unsupported value: ${typeof result}`);
    }
}

export class Solis {
    // Call instead of `new Solis()` -- loading the wasm module is async.
    static async create() {
        const core = await createSolisCore();
        return new Solis(core);
    }

    constructor(core) {
        this.core = core;
        this.vm = core.ccall("solis_vm_new", "number", [], []);
        core.ccall("solis_open_std", null, ["number"], [this.vm]); // math, strings

        this._slots = new Array(NATIVE_POOL).fill(null);
        core.__solisDispatch = (id, vmPtr, argc, argv) => {
            const fn = this._slots[id];
            try {
                const args = [];
                for (let i = 0; i < argc; i++) args.push(readArg(core, argv, i));
                writeResult(core, vmPtr, fn(...args));
            } catch (e) {
                core.ccall("solis_ret_fail", null, ["number", "string"],
                    [vmPtr, e && e.message ? e.message : String(e)]);
            }
        };
    }

    // Registers a JS function as a Solis extern. `fn` receives plain JS
    // values (number/boolean/string/undefined) and returns one of those;
    // throwing fails the script's native call with the thrown message.
    register(name, fn) {
        const id = this._slots.indexOf(null);
        if (id < 0) throw new Error(`Solis: native pool exhausted (max ${NATIVE_POOL})`);
        this._slots[id] = fn;
        if (!this.core.ccall("solis_register", "number",
                ["number", "string", "number"], [this.vm, name, id])) {
            throw new Error(`Solis: could not register '${name}'`);
        }
    }

    // Puts source text at `path` in the module's virtual filesystem, so
    // compile()/run() (and anything the script imports) can see it.
    writeFile(path, source) {
        this.core.FS.writeFile(path, source);
    }

    // Compiles `path` (and its imports/prelude, resolved via writeFile'd
    // paths) to bytecode and loads it into this VM. `preludePath` follows
    // solis_compile: omit for the automatic prelude.sl lookup, "" to
    // disable it, or a path to use explicitly.
    compile(path, preludePath = null) {
        const rc = this.core.ccall("solis_compile", "number",
            ["string", "string"], [path, preludePath]);
        if (rc !== 0) throw new Error(this.core.ccall("solis_last_error", "string", [], []));

        const ptr = this.core.ccall("solis_image_ptr", "number", [], []);
        const len = this.core.ccall("solis_image_len", "number", [], []);
        if (this.core.ccall("solis_load", "number", ["number", "number", "number"],
                [this.vm, ptr, len])) {
            throw new Error(this.core.ccall("solis_last_error", "string", [], []));
        }
    }

    // Compiles (if not already loaded) and runs `path` to completion.
    run(path, preludePath = null) {
        this.compile(path, preludePath);
        if (this.core.ccall("solis_run", "number", ["number"], [this.vm])) {
            throw new Error(this.core.ccall("solis_last_error", "string", [], []));
        }
    }

    free() {
        this.core.ccall("solis_vm_free", null, ["number"], [this.vm]);
    }
}
