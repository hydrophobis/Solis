import geom;
import stats;

func main() {
    // Types, static methods and methods all come through the module name.
    var v = geom.Vec2.of(3.0, 4.0);
    print("|v| = ${v.length()}");
    v.scale(2.0);
    print("after scale, |v| = ${v.length()}");

    // Enums from another module, including their variants.
    let shapes = [geom.Shape.Circle(1.0), geom.Shape.Rect(2.0, 3.0)];
    for s in shapes {
        print("area = ${geom.area(s)}");
    }

    // A second module, with no knowledge of the first.
    let nums = [4, 9, 2, 7, 5];
    print("sum  = ${stats.sum(nums)}");
    print("max  = ${stats.max(nums)}");
    print("mean = ${stats.mean(nums)}");
}

// A function here may take a type declared elsewhere.
func describe(v: geom.Vec2): str {
    return "(${v.x}, ${v.y})";
}
