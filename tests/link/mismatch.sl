// the host returns str
extern func hostName(): int;

func main() {
    print("${hostName() + 1}");
}
