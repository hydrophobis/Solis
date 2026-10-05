//     solis-run++ [--check-leaks] program.slb

#include "solis.hpp"

#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    bool checkLeaks = false;
    std::string path;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--check-leaks") == 0) checkLeaks = true;
        else path = argv[i];
    }
    if (path.empty()) {
        std::cerr << "usage: solis-run++ [--check-leaks] <program.slb>\n";
        return 2;
    }

    try {
        solis::Vm vm;
        vm.openStd();

        // Capturing lambdas, which sl_native_fn cannot express.
        long long printed = 0;

        vm.define("print", [&printed](solis::Vm &, solis::Args args) {
            for (int i = 0; i < args.size(); i++) {
                if (i) std::cout << ' ';
                std::cout << args[i].toString();
            }
            std::cout << '\n';
            printed++;
            return solis::Value();
        });

        vm.define("nowMs", [](solis::Vm &, solis::Args) {
            return solis::Value::integer(
                static_cast<int64_t>(clock() * 1000.0 / CLOCKS_PER_SEC));
        });

        vm.define("hostName", [](solis::Vm &v, solis::Args) {
            return v.string("solis-c");
        });

        vm.define("repeat", [](solis::Vm &v, solis::Args args) {
            if (args.size() < 2) throw solis::Error("repeat(str, int) wants two arguments");
            std::string_view s = args[0].asString();
            int64_t n = args[1].asInt();
            std::string out;
            out.reserve(s.size() * static_cast<size_t>(n > 0 ? n : 0));
            for (int64_t i = 0; i < n; i++) out.append(s);
            return v.string(out);
        });

        vm.loadFile(path);
        vm.run();

        if (checkLeaks) {
            int64_t live = vm.liveObjects();
            std::cerr << "live objects after run: " << live << '\n';
            return live == 0 ? 0 : 3;
        }
        return 0;
    } catch (const solis::Error &e) {
        std::cout.flush();
        std::cerr << "solis: " << e.what() << '\n';
        return 1;
    }
}
