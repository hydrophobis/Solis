#ifndef SOLIS_COMPILE_HPP
#define SOLIS_COMPILE_HPP

#include "solis_compile.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace solis {

class CompileError : public std::runtime_error {
public:
    CompileError(const std::string &text, int count) : std::runtime_error(text), count_(count) {}
    int count() const noexcept { return count_; }

private:
    int count_;
};

// prelude: nullptr uses a prelude.sl beside the script, "" disables it. host: Vm::declarations(), or empty for none.
inline std::vector<uint8_t> compile(const std::string &path, const char *prelude = nullptr,
                                    const std::string &host = std::string()) {
    sl_compiled c;
    if (sl_compile_file_with_host(path.c_str(), prelude, host.empty() ? nullptr : host.c_str(), &c) != 0) {
        CompileError err(c.errors ? c.errors : "compilation failed", c.error_count);
        sl_compiled_free(&c);
        throw err;
    }
    std::vector<uint8_t> image(c.image, c.image + c.image_len);
    sl_compiled_free(&c);
    return image;
}

}  // namespace solis

#endif  // SOLIS_COMPILE_HPP
