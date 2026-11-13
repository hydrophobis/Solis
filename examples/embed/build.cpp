// --host takes a file holding the application's Vm::declarations()

#include "solis_compile.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    std::string in, out;
    const char *prelude = nullptr;  // nullptr: pick up a prelude.sl beside the script
    std::string host;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--prelude") && i + 1 < argc) prelude = argv[++i];
        else if (!std::strcmp(argv[i], "--no-prelude")) prelude = "";
        else if (!std::strcmp(argv[i], "--host") && i + 1 < argc) {
            std::ifstream h(argv[++i]);
            if (!h) { std::cerr << "cannot read " << argv[i] << "\n"; return 2; }
            host.assign(std::istreambuf_iterator<char>(h), std::istreambuf_iterator<char>());
        }
        else in = argv[i];
    }
    if (in.empty()) {
        std::cerr << "usage: build <script.sl> [-o out.slb] [--prelude file | --no-prelude] [--host host.sl]\n";
        return 2;
    }
    if (out.empty()) out = in.substr(0, in.rfind('.')) + ".slb";

    try {
        std::vector<uint8_t> image = solis::compile(in, prelude, host);
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
        if (!f) {
            std::cerr << "cannot write " << out << "\n";
            return 2;
        }
        std::cout << "wrote " << out << ": " << image.size() << " bytes\n";
    } catch (const solis::CompileError &e) {
        std::cerr << e.what() << "\n" << e.count() << " error(s); nothing emitted\n";
        return 1;
    }
    return 0;
}
