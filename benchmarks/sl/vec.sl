struct V2 {
    x: float;
    y: float;
    func add(o: V2): V2 { return V2 { x: this.x + o.x, y: this.y + o.y }; }
    func scale(k: float): V2 { return V2 { x: this.x * k, y: this.y * k }; }
    func dot(o: V2): float { return this.x * o.x + this.y * o.y; }
}
func main() {
    var p = V2 { x: 0.0, y: 0.0 };
    let v = V2 { x: 1.0, y: 0.5 };
    var i = 0;
    var acc = 0.0;
    while i < 3000000 {
        p = p.add(v.scale(0.001));
        acc += p.dot(v);
        i += 1;
    }
    print("${p.x} ${p.y} ${acc}");
}
