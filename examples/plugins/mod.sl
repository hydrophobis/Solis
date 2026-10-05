// A plugin. Not one extern declaration in here: everything the host provides
// comes from prelude.sl sitting next to this file.
//
//     solis examples/plugins/mod.sl

func main() {
    print("plugin running on ${hostName()}");
    print(rule);
    print(shout("loaded"));
    print("repeat: ${repeat("ab", 3)}");
    print("budget: ${prelude.Limits.default().frameMs} ms");
    print("the clock is reachable: ${nowMs() >= 0}");
}
