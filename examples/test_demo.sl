import test;

func main() {
    test.assert(1 + 1 == 2, "addition");
    test.assertEqual(3, 3, "ints");
    test.assertEqual("a", "a", "strings");
    print("done");
}
