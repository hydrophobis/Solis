// C++17 wrapper over solis.h
//
//     solis::Vm vm;
//     vm.openStd();
//     int calls = 0;
//     vm.define("print", [&](solis::Vm &v, solis::Args a) {
//         calls++;
//         for (int i = 0; i < a.size(); i++) std::cout << a[i].toString();
//         return solis::Value();
//     });
//     vm.loadFile("program.slb");
//     vm.run();

#ifndef SOLIS_HPP
#define SOLIS_HPP

#include "solis.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace solis {

class Error : public std::runtime_error {
public:
    explicit Error(const std::string &what) : std::runtime_error(what) {}
};

class Vm;

// An owning handle on a Solis value. Copying retains, destruction releases,
// so the retain/release discipline the C API documents is enforced by the
// type instead of by the caller remembering.
class Value {
public:
    Value() noexcept : vm_(nullptr), v_(sl_void()) {}

    // sl_str, sl_array and sl_to_string hand back rc=1, so adopt those.
    // sl_array_get hands back a borrow, so that one needs a retain.
    static Value adopt(sl_vm *vm, sl_value v) noexcept { return Value(vm, v); }
    static Value borrow(sl_vm *vm, sl_value v) noexcept {
        sl_retain(v);
        return Value(vm, v);
    }

    static Value integer(int64_t n) noexcept { return Value(nullptr, sl_int(n)); }
    static Value number(double d) noexcept { return Value(nullptr, sl_float(d)); }
    static Value boolean(bool b) noexcept { return Value(nullptr, sl_bool(b)); }

    Value(const Value &o) noexcept : vm_(o.vm_), v_(o.v_) { sl_retain(v_); }
    Value(Value &&o) noexcept : vm_(o.vm_), v_(o.v_) { o.vm_ = nullptr; o.v_ = sl_void(); }
    ~Value() { reset(); }

    Value &operator=(const Value &o) noexcept {
        if (this != &o) {
            sl_retain(o.v_);
            reset();
            vm_ = o.vm_;
            v_ = o.v_;
        }
        return *this;
    }

    Value &operator=(Value &&o) noexcept {
        if (this != &o) {
            reset();
            vm_ = o.vm_;
            v_ = o.v_;
            o.vm_ = nullptr;
            o.v_ = sl_void();
        }
        return *this;
    }

    bool isVoid() const noexcept { return v_.type == SL_VOID; }
    bool isInt() const noexcept { return v_.type == SL_INT; }
    bool isFloat() const noexcept { return v_.type == SL_FLOAT; }
    bool isBool() const noexcept { return v_.type == SL_BOOL; }
    bool isObject() const noexcept { return v_.type == SL_OBJ && v_.as.o != nullptr; }
    bool isNull() const noexcept { return v_.type == SL_OBJ && v_.as.o == nullptr; }

    bool isString() const noexcept { return isObject() && v_.as.o->kind == SL_STR; }
    bool isArray() const noexcept { return isObject() && v_.as.o->kind == SL_ARRAY; }
    bool isStruct() const noexcept { return isObject() && v_.as.o->kind == SL_STRUCT; }
    bool isVariant() const noexcept { return isObject() && v_.as.o->kind == SL_VARIANT; }

    int64_t asInt() const {
        if (v_.type == SL_INT) return v_.as.i;
        if (v_.type == SL_FLOAT) return static_cast<int64_t>(v_.as.f);
        throw Error("Solis value is not a number");
    }

    double asFloat() const {
        if (v_.type == SL_FLOAT) return v_.as.f;
        if (v_.type == SL_INT) return static_cast<double>(v_.as.i);
        throw Error("Solis value is not a number");
    }

    bool asBool() const {
        if (v_.type != SL_BOOL) throw Error("Solis value is not a bool");
        return v_.as.b;
    }

    // Points into the runtime's own buffer, so it lives as long as this
    // handle does and no longer.
    std::string_view asString() const {
        if (!isString()) throw Error("Solis value is not a string");
        size_t n = 0;
        const char *p = sl_as_str(v_, &n);
        return std::string_view(p, n);
    }

    // Renders anything, nested arrays and structs included, the way `print`
    // does.
    std::string toString() const;

    int64_t size() const noexcept { return sl_array_len(v_); }

    Value at(int64_t i) const {
        if (!isArray()) throw Error("Solis value is not an array");
        if (i < 0 || i >= sl_array_len(v_)) throw Error("array index out of range");
        return borrow(vm_, sl_array_get(v_, i));
    }

    Value operator[](int64_t i) const { return at(i); }

    // Takes ownership, matching sl_array_push.
    void push(Value item);

    sl_value raw() const noexcept { return v_; }
    sl_vm *vm() const noexcept { return vm_; }

    // Hand ownership to the C API and stop tracking it here.
    sl_value release() noexcept {
        sl_value v = v_;
        vm_ = nullptr;
        v_ = sl_void();
        return v;
    }

private:
    Value(sl_vm *vm, sl_value v) noexcept : vm_(vm), v_(v) {}

    void reset() noexcept {
        if (vm_) sl_release(vm_, v_);
        vm_ = nullptr;
        v_ = sl_void();
    }

    sl_vm *vm_;
    sl_value v_;
};

// The arguments a native was called with. A view, not a container: wrapping
// one costs a retain only when you actually touch it.
class Args {
public:
    Args(sl_vm *vm, int argc, sl_value *argv) noexcept
        : vm_(vm), argc_(argc), argv_(argv) {}

    int size() const noexcept { return argc_; }
    bool empty() const noexcept { return argc_ == 0; }

    Value operator[](int i) const {
        if (i < 0 || i >= argc_) throw Error("native argument index out of range");
        return Value::borrow(vm_, argv_[i]);
    }

private:
    sl_vm *vm_;
    int argc_;
    sl_value *argv_;
};

using Native = std::function<Value(Vm &, Args)>;

namespace detail {

// sl_native_fn is a bare function pointer with no user-data slot, so a
// capturing lambda cannot be registered directly. Each registration takes one
// of a fixed set of trampolines, distinguished at compile time by index; the
// trampoline recovers its Vm from the sl_vm* it was handed, then calls the
// callable stored at that index.
//
// The index is per-VM, so the cap is the C runtime's own SL_NATIVES_MAX rather
// than a process-wide budget, and the number of VMs is unlimited.
constexpr int kMaxNatives = 128;

using Entry = sl_value (*)(sl_vm *, int, sl_value *);

template <int N>
sl_value trampoline(sl_vm *vm, int argc, sl_value *argv);

template <int... Is>
constexpr void fill(Entry *out, std::integer_sequence<int, Is...>) {
    ((out[Is] = &trampoline<Is>), ...);
}

inline const Entry *entries() noexcept {
    static Entry table[kMaxNatives] = {};
    static const bool ready = [&] {
        fill(table, std::make_integer_sequence<int, kMaxNatives>{});
        return true;
    }();
    (void)ready;
    return table;
}

// sl_vm carries no user data either, so the mapping back to the owning Vm
// lives here. Mutated only when a Vm is created, moved or destroyed.
struct Binding {
    sl_vm *key;
    Vm *owner;
};

inline std::mutex &registryMutex() noexcept {
    static std::mutex m;
    return m;
}

inline std::vector<Binding> &registry() noexcept {
    static std::vector<Binding> table;
    return table;
}

inline void bind(sl_vm *key, Vm *owner) {
    std::lock_guard<std::mutex> lock(registryMutex());
    for (Binding &b : registry()) {
        if (b.key == key) { b.owner = owner; return; }
    }
    registry().push_back(Binding{key, owner});
}

inline void unbind(sl_vm *key) noexcept {
    std::lock_guard<std::mutex> lock(registryMutex());
    std::vector<Binding> &table = registry();
    for (size_t i = 0; i < table.size(); i++) {
        if (table[i].key == key) {
            table[i] = table.back();
            table.pop_back();
            return;
        }
    }
}

inline Vm *lookup(sl_vm *key) noexcept {
    std::lock_guard<std::mutex> lock(registryMutex());
    for (const Binding &b : registry()) {
        if (b.key == key) return b.owner;
    }
    return nullptr;
}

}  // namespace detail

// Owns an sl_vm. Movable, not copyable.
class Vm {
public:
    Vm() : vm_(sl_new()) {
        if (!vm_) throw Error("cannot create a Solis VM");
        detail::bind(vm_, this);
    }

    ~Vm() { close(); }

    Vm(const Vm &) = delete;
    Vm &operator=(const Vm &) = delete;

    Vm(Vm &&o) noexcept : vm_(o.vm_), natives_(std::move(o.natives_)) {
        o.vm_ = nullptr;
        if (vm_) detail::bind(vm_, this);
    }

    Vm &operator=(Vm &&o) noexcept {
        if (this != &o) {
            close();
            vm_ = o.vm_;
            natives_ = std::move(o.natives_);
            o.vm_ = nullptr;
            if (vm_) detail::bind(vm_, this);
        }
        return *this;
    }

    // The `math` and `strings` modules. Without this a script importing them
    // fails to resolve its primitives.
    void openStd() { sl_open_std(vm_); }

    // Register a native. Any callable with signature
    // Value(Vm &, Args) works, capturing lambdas included.
    template <class F>
    void define(const std::string &name, F &&fn) {
        const int slot = static_cast<int>(natives_.size());
        if (slot >= detail::kMaxNatives) {
            throw Error("a Solis VM accepts at most " +
                        std::to_string(detail::kMaxNatives) + " natives");
        }
        natives_.emplace_back(std::forward<F>(fn));
        if (!sl_register(vm_, name.c_str(), detail::entries()[slot])) {
            natives_.pop_back();
            throw Error("cannot register native `" + name + "`");
        }
    }

    // How many more natives this VM will take. Asks the runtime rather than
    // counting our own, since sl_new and openStd() register some too.
    int nativesRemaining() const noexcept {
        int c_free = sl_natives_free(vm_);
        int trampolines_left = detail::kMaxNatives - static_cast<int>(natives_.size());
        return c_free < trampolines_left ? c_free : trampolines_left;
    }

    void load(const uint8_t *bytes, size_t len) {
        if (sl_load(vm_, bytes, len) != SL_OK) throw Error(lastError());
    }

    void load(const std::vector<uint8_t> &image) { load(image.data(), image.size()); }

    void loadFile(const std::string &path) {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) throw Error("cannot open " + path);
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n < 0) { std::fclose(f); throw Error("cannot size " + path); }

        std::vector<uint8_t> buf(static_cast<size_t>(n));
        size_t got = buf.empty() ? 0 : std::fread(buf.data(), 1, buf.size(), f);
        std::fclose(f);
        if (got != buf.size()) throw Error("cannot read " + path);
        load(buf);
    }

    void run() {
        if (sl_run(vm_) != SL_OK) throw Error(lastError());
    }

    Value string(std::string_view s) {
        return Value::adopt(vm_, sl_str(vm_, s.data(), s.size()));
    }

    Value array(int64_t capacity = 0) {
        return Value::adopt(vm_, sl_array(vm_, capacity));
    }

    // Should be zero once a program has finished, unless the script built a
    // reference cycle.
    int64_t liveObjects() const { return sl_live_objects(vm_); }

    std::string lastError() const {
        const char *e = sl_error(vm_);
        return e ? std::string(e) : std::string("unknown Solis error");
    }

    void fail(const std::string &message) { sl_fail(vm_, message.c_str()); }

    sl_vm *raw() const noexcept { return vm_; }

private:
    void close() noexcept {
        if (vm_) {
            detail::unbind(vm_);
            sl_free(vm_);
        }
        natives_.clear();
        vm_ = nullptr;
    }

    sl_vm *vm_ = nullptr;
    std::vector<Native> natives_;

    template <int N>
    friend sl_value detail::trampoline(sl_vm *, int, sl_value *);
};

inline std::string Value::toString() const {
    if (!vm_) {
        // A primitive carries no VM, and rendering one needs somewhere to
        // allocate the result, so handle those directly.
        switch (v_.type) {
            case SL_INT: return std::to_string(v_.as.i);
            case SL_BOOL: return v_.as.b ? "true" : "false";
            case SL_VOID: return "()";
            default: break;
        }
    }
    if (!vm_) throw Error("cannot render this value without a VM");
    Value s = Value::adopt(vm_, sl_to_string(vm_, v_));
    return std::string(s.asString());
}

inline void Value::push(Value item) {
    if (!isArray()) throw Error("Solis value is not an array");
    sl_vm *vm = vm_ ? vm_ : item.vm();
    if (!vm) throw Error("cannot push without a VM");
    if (!sl_array_push(vm, v_, item.release())) throw Error("cannot grow array");
}

namespace detail {

template <int N>
sl_value trampoline(sl_vm *vm, int argc, sl_value *argv) {
    Vm *owner = lookup(vm);
    if (!owner || N >= static_cast<int>(owner->natives_.size())) {
        sl_fail(vm, "native called after its VM was destroyed");
        return sl_void();
    }
    // Nothing may unwind into C.
    try {
        return owner->natives_[N](*owner, Args(vm, argc, argv)).release();
    } catch (const std::exception &e) {
        sl_fail(vm, e.what());
    } catch (...) {
        sl_fail(vm, "unknown C++ exception in a Solis native");
    }
    return sl_void();
}

}  // namespace detail

}  // namespace solis

#endif // SOLIS_HPP
