import math;
func main() {
    let n = 200;
    var a: [[float]] = [[0.0]];
    var b: [[float]] = [[0.0]];
    var i = 0;
    while i < n {
        var ra = [math.toFloat(i)];
        var rb = [math.toFloat(0 - i)];
        var j = 1;
        while j < n { push(ra, math.toFloat(i + j)); push(rb, math.toFloat(j - i)); j += 1; }
        if i == 0 { a[0] = ra; b[0] = rb; } else { push(a, ra); push(b, rb); }
        i += 1;
    }
    var sum = 0.0;
    i = 0;
    while i < n {
        var j = 0;
        while j < n {
            var s = 0.0;
            var k = 0;
            while k < n { s += a[i][k] * b[k][j]; k += 1; }
            sum += s;
            j += 1;
        }
        i += 1;
    }
    print("${sum}");
}
