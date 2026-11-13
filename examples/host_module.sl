// solis --show-host lists what `host` offers

import host;

func main() {
    print("running on: ${host.hostName()}");
    print(host.repeat("-", 28));

    let started = host.nowMs();
    var total = 0;
    var i = 0;
    while i < 100000 {
        total += i % 7;
        i += 1;
    }
    print("total = ${total}");
    print("took ${host.nowMs() - started} ms");

    print(host.repeat("=", 28));
}
