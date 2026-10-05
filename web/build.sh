#!/bin/sh
# Builds web/solis.wasm + web/solis_core.mjs: the compiler and the pure
# (os-less) runtime, with web/solis_wasm.c as the JS-facing glue.
#
# Needs the Emscripten SDK on PATH (emcc). web/solis.mjs is the hand-written
# wrapper that loads this and exposes the ergonomic API; this script only
# builds the low-level module it wraps.
set -e
cd "$(dirname "$0")/.."

EXPORTED_FUNCS='_solis_compile,_solis_image_ptr,_solis_image_len,_solis_last_error,_solis_vm_new,_solis_vm_free,_solis_open_std,_solis_load,_solis_run,_solis_register,_solis_arg_type,_solis_arg_int,_solis_arg_float,_solis_arg_bool,_solis_arg_cstr,_solis_ret_void,_solis_ret_int,_solis_ret_float,_solis_ret_bool,_solis_ret_str,_solis_ret_fail,_malloc,_free'

emcc -O2 -std=c99 -Icompiler -Iruntime \
    -DNDEBUG \
    compiler/lex.c compiler/parse.c compiler/check.c compiler/type.c \
    compiler/common.c compiler/module.c compiler/gen.c compiler/emit.c \
    compiler/std_src.c \
    runtime/solis.c runtime/solis_std.c \
    web/solis_wasm.c \
    -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=createSolisCore \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s FORCE_FILESYSTEM=1 \
    -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","FS","getValue","UTF8ToString"]' \
    -s EXPORTED_FUNCTIONS="[$(echo "$EXPORTED_FUNCS" | sed "s/[^,]*/'&'/g")]" \
    -o web/solis_core.mjs

echo "built web/solis_core.mjs + web/solis_core.wasm"
