func assert(cond: bool, msg: str) {
    if !cond {
        print("FAIL: ${msg}");
    }
}

func assertEqual[T](got: T, want: T, msg: str) {
    if got != want {
        print("FAIL: ${msg}");
    }
}
