// Overlays `b` onto `a` in place (`b`'s keys win on conflict) and returns `a`.
func merge[K, V](a: {K: V}, b: {K: V}): {K: V} {
    for k in keys(b) {
        a[k] = b[k];
    }
    return a;
}
