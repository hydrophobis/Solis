// The `os` module: the process and its surroundings.
//
// The most ambient of the batteries: `env` reads the environment, `run`
// starts other programs. Only the standard interpreter opens this.

// Everything the interpreter was given after its own options, starting with
// the script path itself, so `args()[0]` is the script and the rest are its
// arguments, as in most scripting languages.
extern func args(): [str];

// The variable's value, or the empty string. `hasEnv` tells an unset
// variable from one set to nothing.
extern func env(name: str): str;
extern func hasEnv(name: str): bool;

// Stops immediately with this status. Nothing after it runs, and no
// destructor does either.
extern func exit(code: int);

// One of "windows", "macos", "linux" or "unix".
extern func platform(): str;

extern func cwd(): str;

// Runs a command through the system shell and returns its exit status, or
// -1 if it could not be started. The command's output goes wherever this
// process's output goes.
extern func run(command: str): int;

// The script's own arguments, without the script path. Index 0 is the first
// real argument, which is what a script usually wants.
func argv(): [str] {
    var out: [str] = [];
    let all = args();
    var i = 1;
    while i < len(all) {
        push(out, all[i]);
        i += 1;
    }
    return out;
}

// The i'th script argument, or `fallback` if it was not given.
func arg(i: int, fallback: str): str {
    let all = args();
    if i + 1 >= len(all) {
        return fallback;
    }
    return all[i + 1];
}

func isWindows(): bool {
    return platform() == "windows";
}

// The value of an environment variable, or `fallback` when it is unset.
func envOr(name: str, fallback: str): str {
    if !hasEnv(name) {
        return fallback;
    }
    return env(name);
}
