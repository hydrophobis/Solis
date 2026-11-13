import { Solis } from "./solis.mjs";

const vm = await Solis.create();

let out = [];
vm.register("print", (...parts) => out.push(parts.join(" ")));
vm.register("hostName", () => "web", "()->str");
vm.register("nowMs", () => Date.now(), "()->int");
vm.register("repeat", (s, n) => s.repeat(n), "(str,int)->str");

vm.writeFile("prelude.sl", `
    extern func print(s: str);
    extern func hostName(): str;
    extern func nowMs(): int;
    extern func repeat(s: str, n: int): str;
`);

vm.writeFile("plugin.sl", `
    func main() {
        print("plugin running on \${hostName()}");
        print("repeat: \${repeat("ab", 3)}");
        print("the clock is reachable: \${nowMs() >= 0}");
        print("math: \${2 + 2}");
    }
`);

vm.run("plugin.sl");
console.log(out.join("\n"));

if (out.length !== 4 || !out[0].includes("web") || out[1] !== "repeat: ababab") {
    console.error("SMOKE TEST FAILED");
    process.exit(1);
}
out.length = 0;
vm.writeFile("viahost.sl", `
    import host;
    func main() {
        print("host says " + host.hostName() + ", " + host.repeat("ab", 2));
    }
`);
vm.run("viahost.sl");
if (out.length !== 1 || out[0] !== "host says web, abab") {
    console.error("SMOKE TEST FAILED: import host gave", JSON.stringify(out));
    process.exit(1);
}

vm.writeFile("badhost.sl", `
    import host;
    func main() { print(host.repeat(5, "x")); }
`);
let rejected = "";
try { vm.run("badhost.sl"); } catch (e) { rejected = e.message; }
if (!rejected.includes("expected `str`, found `int`")) {
    console.error("SMOKE TEST FAILED: a wrong host call was not rejected:", rejected);
    process.exit(1);
}

console.log("SMOKE TEST OK");
