#include "solis.hpp"
#include "solis_compile.hpp"

#include <chrono>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: interpret <script.sl>\n";
        return 2;
    }

    try {
        solis::Vm vm;
        vm.openStd();

        long long calls = 0;
        vm.define("print", [&calls](solis::Vm &, solis::Args args) {
            for (int i = 0; i < args.size(); i++) {
                if (i) std::cout << ' ';
                std::cout << args[i].toString();
            }
            std::cout << '\n';
            calls++;
            return solis::Value();
        });
        vm.define("nowMs", "()->int", [](solis::Vm &, solis::Args) {
            using namespace std::chrono;
            auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
            return solis::Value::integer(ms);
        });
        vm.define("hostName", "()->str", [](solis::Vm &v, solis::Args) { return v.string("my-host"); });
        vm.define("repeat", "(str,int)->str", [](solis::Vm &v, solis::Args args) {
            if (args.size() < 2) throw solis::Error("repeat(str, int) wants two arguments");
            std::string_view s = args[0].asString();
            std::string out;
            for (int64_t i = 0, n = args[1].asInt(); i < n; i++) out.append(s);
            return v.string(out);
        });

        // declarations() reflects the natives registered so far
        std::vector<uint8_t> image = solis::compile(argv[1], nullptr, vm.declarations());

        vm.load(image);
        vm.run();

        std::cerr << "[host] print() was called " << calls << " time(s); "
                  << vm.liveObjects() << " live object(s) at exit\n";
    } catch (const solis::CompileError &e) {
        std::cerr << e.what() << "\n" << e.count() << " error(s)\n";
        return 1;
    } catch (const solis::Error &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
