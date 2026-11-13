#include "solis.hpp"

#include <iostream>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: run_image <program.slb>\n";
        return 2;
    }
    try {
        solis::Vm vm;
        vm.openStd();
        vm.define("print", [](solis::Vm &, solis::Args args) {
            for (int i = 0; i < args.size(); i++) std::cout << (i ? " " : "") << args[i].toString();
            std::cout << '\n';
            return solis::Value();
        });
        vm.define("nowMs", "()->int", [](solis::Vm &, solis::Args) { return solis::Value::integer(0); });
        vm.define("hostName", "()->str", [](solis::Vm &v, solis::Args) { return v.string("run_image"); });
        vm.define("repeat", "(str,int)->str", [](solis::Vm &v, solis::Args args) {
            std::string out;
            for (int64_t i = 0, n = args[1].asInt(); i < n; i++) out.append(args[0].asString());
            return v.string(out);
        });
        vm.loadFile(argv[1]);
        vm.run();
    } catch (const solis::Error &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
