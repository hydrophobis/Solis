import math;
import strings;
func main() {
    var total = 0;
    var i = 0;
    while i < 40000 {
        let s = "k${i},v${i * 3},w${i % 7}";
        let parts = strings.split(s, ",");
        total += len(parts) + strings.len(strings.join(parts, "-"));
        i += 1;
    }
    print("${total}");
}
