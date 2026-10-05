import { Solis } from "./solis.mjs";

const vm = await Solis.create();

let out = [];
vm.register("print", (...parts) => out.push(parts.join(" ")));
vm.register("hostName", () => "web");
vm.register("nowMs", () => Date.now());
vm.register("repeat", (s, n) => s.repeat(n));

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
console.log("SMOKE TEST OK");
