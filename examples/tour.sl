// ---------------------------------------------------------------------------
// A tour of Solis.
//
// Second pass: C-family syntax rather than ML/Rust-flavoured, and nulls
// instead of optionals.
// ---------------------------------------------------------------------------

// Bindings. Immutable by default; `var` opts into mutation.
let gravity = 9.81;
var score = 0;

// Types are inferred locally, always annotated at boundaries.
let name: str = "Solis";
let maxHp: int = 100;


// --- Structs are values -----------------------------------------------------
// Fields and methods live in one body, the way a C++/C#/TypeScript class does.
// No allocation, no refcount traffic, no header: a Vec2 is 8 bytes.

struct Vec2 {
    x: float;
    y: float;

    // No receiver parameter. Inside a method, `this` is the instance.
    func length(): float {
        return sqrt(this.x * this.x + this.y * this.y);
    }

    // `mut` marks a method that writes to its receiver. Without it, this is a
    // compile error rather than a silent copy.
    mut func scale(k: float) {
        this.x *= k;
        this.y *= k;
    }

    // `static` for functions that need no instance: Vec2.zero()
    static func zero(): Vec2 {
        return Vec2 { x: 0.0, y: 0.0 };
    }
}


// --- Tagged unions ----------------------------------------------------------
// Variants may be empty or carry named payload fields.

enum Event {
    Quit,
    Key(code: int),
    Click(pos: Vec2, button: int),
}

// `switch` is exhaustive: leaving out a variant is a compile error that names
// the one you missed. Cases do not fall through, so no `break` is needed.
func handle(e: Event): bool {
    switch e {
        case Event.Quit:
            return false;

        case Event.Key(code):
            score += code;
            return true;

        case Event.Click(pos, button):
            spawnAt(pos, button);
            return true;
    }
}


// --- Null -------------------------------------------------------------------
// Reference types may be null. `?.` short-circuits to null instead of faulting.

func findPlayer(id: int): Player {
    if id < 0 {
        return null;
    }
    return world.players[id];
}

func greet(id: int) {
    let p = findPlayer(id);
    if p != null {
        print("hello ${p.name}");
    } else {
        print("no such player");
    }
}


// --- Interfaces, no inheritance ---------------------------------------------
// A type lists the interfaces it satisfies after a colon. Behaviour is only
// ever composed, never inherited: no base classes and no vtable chains, so
// method lookup is always a single indirection.

interface Drawable {
    func draw(c: Canvas);

    // Interfaces may carry default implementations.
    func visible(): bool {
        return true;
    }
}

struct Sprite : Drawable {
    pos: Vec2;
    tex: Texture;

    func draw(c: Canvas) {
        c.blit(this.tex, this.pos);
    }
}

// Generic over anything Drawable, monomorphised at the call site.
func drawAll[T: Drawable](items: [T], c: Canvas) {
    for it in items {
        if it.visible() {
            it.draw(c);
        }
    }
}


// --- Memory -----------------------------------------------------------------
// Heap values are reference counted. Most retain/release pairs are removed at
// compile time, and plain-data types like Vec2 never participate at all.

struct Node {
    name: str;
    children: [Node];

    // Without `weak` this parent link closes a cycle, and Solis reports that
    // at compile time rather than leaking quietly. A weak field reads as null
    // once its target is gone.
    weak parent: Node;
}

// An explicit arena: everything allocated inside is released in one step when
// the block exits. Useful on a hot path, never mandatory.
func rebuildFrame(w: World) {
    arena {
        let scratch = collectVisible(w);
        drawAll(scratch, w.canvas);
    }
}


// --- The host boundary ------------------------------------------------------
// Scripts are embedded, so the host decides what exists. Anything it does not
// grant is simply absent: there is no ambient filesystem or network.

extern func spawnAt(pos: Vec2, button: int);
extern func print(msg: str);
extern func sqrt(x: float): float;


func main() {
    var running = true;
    while running {
        for e in pollEvents() {
            running = handle(e);
        }
    }
    print("final score: ${score}");
}
