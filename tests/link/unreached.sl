// never called, but still must be registered
extern func ghost(): int;

func main() {
    print("before");
    if false {
        print("${ghost()}");
    }
}
