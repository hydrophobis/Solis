// Shared runner used by index.html, learn.html and playground.html. Loads
// the wasm module once per page and gives every runnable code block its own
// fresh VM, with a small fixed set of host natives any example may call.
import { Solis } from "./playground/solis.mjs";
import createSolisCore from "./playground/solis_core.mjs";

const corePromise = createSolisCore();
let nextFile = 0;

// A toy "host" so examples that reach past the language into the embedding
// application (hostName, roll, nowMs) have something to call. Mirrors
// examples/plugins in the main repo: a real embedder registers its own set.
function registerDemoHost(vm, print) {
    vm.register("print", print);
    vm.register("hostName", () => "playground");
    vm.register("nowMs", () => Date.now());
    vm.register("roll", (sides) => 1 + Math.floor(Math.random() * sides));
}

// Runs `source` to completion, returning { ok, lines, error }. Never throws.
export async function runSolis(source) {
    const core = await corePromise;
    const vm = new Solis(core);
    const lines = [];
    registerDemoHost(vm, (s) => lines.push(String(s)));
    const path = `snippet_${nextFile++}.sl`;
    try {
        vm.writeFile(path, source);
        vm.run(path);
        return { ok: true, lines };
    } catch (e) {
        return { ok: false, lines, error: e.message || String(e) };
    } finally {
        vm.free();
    }
}

// Wires every `.example` block on the page: a Run button, a <pre><code> (or
// an editable textarea if the block has [data-editable]), and an .output
// slot that gets filled in after running.
export function wireExamples(root = document) {
    root.querySelectorAll(".example").forEach((block) => {
        const btn = block.querySelector(".run-btn");
        const out = block.querySelector(".output");
        const codeEl = block.querySelector("textarea.code-edit, code");
        if (!btn || !out || !codeEl) return;

        const getSource = () =>
            codeEl.tagName === "TEXTAREA" ? codeEl.value : codeEl.textContent;

        btn.addEventListener("click", async () => {
            btn.disabled = true;
            out.className = "output";
            out.textContent = "running…";
            const { ok, lines, error } = await runSolis(getSource());
            out.innerHTML = "";
            for (const line of lines) {
                const d = document.createElement("span");
                d.className = "line";
                d.textContent = line;
                out.appendChild(d);
                out.appendChild(document.createElement("br"));
            }
            if (!ok) {
                const d = document.createElement("div");
                d.textContent = error;
                out.appendChild(d);
            }
            out.className = "output " + (ok ? "ok" : "err");
            btn.disabled = false;
        });
    });
}
