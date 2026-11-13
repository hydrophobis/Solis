// matches the CLI's registered signatures
extern func nowMs(): int;
extern func hostName(): str;
extern func repeat(s: str, n: int): str;

func main() {
    print("${hostName()} ${repeat("ab", 2)} ${nowMs() >= 0}");
}
