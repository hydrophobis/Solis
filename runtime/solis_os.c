// The batteries: io, fs, os, time, rand. Separate from solis_std.c so an
// embedding host can leave this file out of the link and keep a sandbox

// -std=c99 sets __STRICT_ANSI__, which hides gettimeofday/nanosleep/getcwd/
// rmdir/opendir on glibc unless this is defined before any include.
#if !defined(_WIN32)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  ifdef __APPLE__
#    define _DARWIN_C_SOURCE 1
#  endif
#endif

#include "solis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#  include <direct.h>
#  include <windows.h>
#  define getcwd_ _getcwd
#  define mkdir_(p) _mkdir(p)
#  define rmdir_ _rmdir
#  define OS_NAME "windows"
#else
#  include <dirent.h>
#  include <sys/time.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  define getcwd_ getcwd
#  define mkdir_(p) mkdir((p), 0777)
#  define rmdir_ rmdir
#  if defined(__APPLE__)
#    define OS_NAME "macos"
#  elif defined(__linux__)
#    define OS_NAME "linux"
#  else
#    define OS_NAME "unix"
#  endif
#endif

static const char *want_s(sl_vm *vm, int argc, sl_value *argv, int i, const char *who,
                          size_t *n) {
    if (i >= argc || argv[i].type != SL_OBJ || !argv[i].as.o) {
        sl_fail(vm, who);
        *n = 0;
        return NULL;
    }
    return sl_as_str(argv[i], n);
}

static int64_t want_i(sl_vm *vm, int argc, sl_value *argv, int i, const char *who) {
    if (i >= argc || argv[i].type != SL_INT) { sl_fail(vm, who); return 0; }
    return argv[i].as.i;
}

// Solis strings aren't NUL-terminated; OS calls want one. Fixed-size so a
// script can't make this allocate without bound.
#define PATH_CAP 1024

static bool want_path(sl_vm *vm, int argc, sl_value *argv, int i, const char *who,
                      char *out) {
    size_t n;
    const char *p = want_s(vm, argc, argv, i, who, &n);
    if (!p) return false;
    if (n >= PATH_CAP) { sl_fail(vm, "path is too long"); return false; }
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

static sl_value io_write(sl_vm *vm, int argc, sl_value *argv) {
    size_t n;
    const char *p = want_s(vm, argc, argv, 0, "io.write expects a string", &n);
    if (p) fwrite(p, 1, n, stdout);
    return sl_void();
}

static sl_value io_write_err(sl_vm *vm, int argc, sl_value *argv) {
    size_t n;
    const char *p = want_s(vm, argc, argv, 0, "io.writeErr expects a string", &n);
    if (p) { fflush(stdout); fwrite(p, 1, n, stderr); }
    return sl_void();
}

static sl_value io_flush(sl_vm *vm, int argc, sl_value *argv) {
    (void)vm; (void)argc; (void)argv;
    fflush(stdout);
    return sl_void();
}

static sl_value read_until(sl_vm *vm, bool whole) {
    size_t cap = 256, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (!whole && c == '\n') break;
        if (len + 1 >= cap) {
            size_t ncap = cap * 2;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) { free(buf); sl_fail(vm, "out of memory"); return sl_void(); }
            buf = nb;
            cap = ncap;
        }
        buf[len++] = (char)c;
    }
    // strip a trailing \r from a CRLF line
    if (!whole && len && buf[len - 1] == '\r') len--;
    sl_value out = sl_str(vm, buf, len);
    free(buf);
    return out;
}

static sl_value io_read_line(sl_vm *vm, int argc, sl_value *argv) {
    (void)argc; (void)argv;
    return read_until(vm, false);
}

static sl_value io_read_all(sl_vm *vm, int argc, sl_value *argv) {
    (void)argc; (void)argv;
    return read_until(vm, true);
}

static sl_value io_eof(sl_vm *vm, int argc, sl_value *argv) {
    (void)vm; (void)argc; (void)argv;
    int c = fgetc(stdin);
    if (c == EOF) return sl_bool(true);
    ungetc(c, stdin);
    return sl_bool(false);
}

static sl_value fs_read(sl_vm *vm, int argc, sl_value *argv) {
    char path[PATH_CAP];
    if (!want_path(vm, argc, argv, 0, "fs.read expects a path", path)) return sl_void();

    FILE *f = fopen(path, "rb");
    if (!f) return sl_cstr(vm, "");

    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { fclose(f); sl_fail(vm, "out of memory"); return sl_void(); }
    for (;;) {
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (len < cap) break;
        char *nb = (char *)realloc(buf, cap * 2);
        if (!nb) { free(buf); fclose(f); sl_fail(vm, "out of memory"); return sl_void(); }
        buf = nb;
        cap *= 2;
    }
    fclose(f);
    sl_value out = sl_str(vm, buf, len);
    free(buf);
    return out;
}

static sl_value put(sl_vm *vm, int argc, sl_value *argv, const char *mode,
                    const char *who) {
    char path[PATH_CAP];
    if (!want_path(vm, argc, argv, 0, who, path)) return sl_void();
    size_t n;
    const char *text = want_s(vm, argc, argv, 1, who, &n);
    if (!text) return sl_void();

    FILE *f = fopen(path, mode);
    if (!f) return sl_bool(false);
    size_t wrote = n ? fwrite(text, 1, n, f) : 0;
    bool ok = fclose(f) == 0 && wrote == n;
    return sl_bool(ok);
}

static sl_value fs_write(sl_vm *vm, int argc, sl_value *argv) {
    return put(vm, argc, argv, "wb", "fs.write expects a path and a string");
}

static sl_value fs_append(sl_vm *vm, int argc, sl_value *argv) {
    return put(vm, argc, argv, "ab", "fs.append expects a path and a string");
}

static bool stat_path(const char *path, struct stat *st) {
    return stat(path, st) == 0;
}

static sl_value fs_exists(sl_vm *vm, int argc, sl_value *argv) {
    char path[PATH_CAP];
    struct stat st;
    if (!want_path(vm, argc, argv, 0, "fs.exists expects a path", path)) return sl_void();
    return sl_bool(stat_path(path, &st));
}

static sl_value fs_is_dir(sl_vm *vm, int argc, sl_value *argv) {
    char path[PATH_CAP];
    struct stat st;
    if (!want_path(vm, argc, argv, 0, "fs.isDir expects a path", path)) return sl_void();
    if (!stat_path(path, &st)) return sl_bool(false);
    return sl_bool((st.st_mode & S_IFMT) == S_IFDIR);
}

static sl_value fs_size(sl_vm *vm, int argc, sl_value *argv) {
    char path[PATH_CAP];
    struct stat st;
    if (!want_path(vm, argc, argv, 0, "fs.size expects a path", path)) return sl_void();
    if (!stat_path(path, &st)) return sl_int(-1);
    return sl_int((int64_t)st.st_size);
}

static sl_value fs_remove(sl_vm *vm, int argc, sl_value *argv) {
    char path[PATH_CAP];
    if (!want_path(vm, argc, argv, 0, "fs.remove expects a path", path)) return sl_void();
    struct stat st;
    if (stat_path(path, &st) && (st.st_mode & S_IFMT) == S_IFDIR)
        return sl_bool(rmdir_(path) == 0);
    return sl_bool(remove(path) == 0);
}

static sl_value fs_rename(sl_vm *vm, int argc, sl_value *argv) {
    char from[PATH_CAP], to[PATH_CAP];
    if (!want_path(vm, argc, argv, 0, "fs.rename expects two paths", from)) return sl_void();
    if (!want_path(vm, argc, argv, 1, "fs.rename expects two paths", to)) return sl_void();
    return sl_bool(rename(from, to) == 0);
}

static sl_value fs_mkdir(sl_vm *vm, int argc, sl_value *argv) {
    char path[PATH_CAP];
    if (!want_path(vm, argc, argv, 0, "fs.mkdir expects a path", path)) return sl_void();
    return sl_bool(mkdir_(path) == 0);
}

static sl_value fs_list(sl_vm *vm, int argc, sl_value *argv) {
    char path[PATH_CAP];
    if (!want_path(vm, argc, argv, 0, "fs.list expects a path", path)) return sl_void();

    sl_value arr = sl_array(vm, 8);
    if (arr.type != SL_OBJ) return sl_void();

#ifdef _WIN32
    char pattern[PATH_CAP + 4];
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return arr;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (!sl_array_push(vm, arr, sl_cstr(vm, fd.cFileName))) {
            FindClose(h);
            sl_release(vm, arr);
            return sl_void();
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) return arr;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (!sl_array_push(vm, arr, sl_cstr(vm, e->d_name))) {
            closedir(d);
            sl_release(vm, arr);
            return sl_void();
        }
    }
    closedir(d);
#endif
    return arr;
}

static int    os_argc = 0;
static char **os_argv = NULL;

static sl_value os_args(sl_vm *vm, int argc, sl_value *argv) {
    (void)argc; (void)argv;
    sl_value arr = sl_array(vm, os_argc ? os_argc : 1);
    if (arr.type != SL_OBJ) return sl_void();
    for (int i = 0; i < os_argc; i++) {
        if (!sl_array_push(vm, arr, sl_cstr(vm, os_argv[i]))) {
            sl_release(vm, arr);
            return sl_void();
        }
    }
    return arr;
}

static sl_value os_env(sl_vm *vm, int argc, sl_value *argv) {
    char name[PATH_CAP];
    if (!want_path(vm, argc, argv, 0, "os.env expects a name", name)) return sl_void();
    const char *v = getenv(name);
    return sl_cstr(vm, v ? v : "");
}

static sl_value os_has_env(sl_vm *vm, int argc, sl_value *argv) {
    char name[PATH_CAP];
    if (!want_path(vm, argc, argv, 0, "os.hasEnv expects a name", name)) return sl_void();
    return sl_bool(getenv(name) != NULL);
}

static sl_value os_exit(sl_vm *vm, int argc, sl_value *argv) {
    int64_t code = want_i(vm, argc, argv, 0, "os.exit expects an int");
    fflush(stdout);
    fflush(stderr);
    exit((int)code);
    return sl_void();      // unreachable
}

static sl_value os_platform(sl_vm *vm, int argc, sl_value *argv) {
    (void)argc; (void)argv;
    return sl_cstr(vm, OS_NAME);
}

static sl_value os_cwd(sl_vm *vm, int argc, sl_value *argv) {
    (void)argc; (void)argv;
    char buf[PATH_CAP];
    if (!getcwd_(buf, sizeof buf)) return sl_cstr(vm, "");
    return sl_cstr(vm, buf);
}

static sl_value os_run(sl_vm *vm, int argc, sl_value *argv) {
    size_t n;
    const char *p = want_s(vm, argc, argv, 0, "os.run expects a command", &n);
    if (!p) return sl_void();
    if (n >= 4096) { sl_fail(vm, "command is too long"); return sl_void(); }
    char cmd[4096];
    memcpy(cmd, p, n);
    cmd[n] = 0;
    fflush(stdout);
    int rc = system(cmd);
    if (rc == -1) return sl_int(-1);
#ifndef _WIN32
    if (WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    return sl_int((int64_t)rc);
}

static int64_t wall_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)(t / 10000ULL) - 11644473600000LL;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

static int64_t started_ms = 0;

static sl_value time_now_ms(sl_vm *vm, int argc, sl_value *argv) {
    (void)vm; (void)argc; (void)argv;
    return sl_int(wall_ms());
}

static sl_value time_mono_ms(sl_vm *vm, int argc, sl_value *argv) {
    (void)vm; (void)argc; (void)argv;
    return sl_int(wall_ms() - started_ms);
}

static sl_value time_sleep_ms(sl_vm *vm, int argc, sl_value *argv) {
    int64_t ms = want_i(vm, argc, argv, 0, "time.sleepMs expects an int");
    if (ms <= 0) return sl_void();
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
    return sl_void();
}

static sl_value time_iso(sl_vm *vm, int argc, sl_value *argv) {
    (void)argc; (void)argv;
    time_t t = (time_t)(wall_ms() / 1000);
    struct tm *g = gmtime(&t);
    char buf[32];
    if (!g) return sl_cstr(vm, "");
    snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", g->tm_year + 1900,
             g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min, g->tm_sec);
    return sl_cstr(vm, buf);
}

static uint64_t rng_state = 0x2545F4914F6CDD1DULL;

static uint64_t next_u64(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static sl_value rand_seed(sl_vm *vm, int argc, sl_value *argv) {
    int64_t n = want_i(vm, argc, argv, 0, "rand.seed expects an int");
    rng_state = (uint64_t)n ? (uint64_t)n : 0x9E3779B97F4A7C15ULL;
    return sl_void();
}

static sl_value rand_below(sl_vm *vm, int argc, sl_value *argv) {
    int64_t n = want_i(vm, argc, argv, 0, "rand.below expects an int");
    if (n <= 0) return sl_int(0);
    uint64_t limit = (uint64_t)n;
    uint64_t zone = UINT64_MAX - (UINT64_MAX % limit) - 1;
    uint64_t r;
    do { r = next_u64(); } while (r > zone);
    return sl_int((int64_t)(r % limit));
}

static sl_value rand_real(sl_vm *vm, int argc, sl_value *argv) {
    (void)vm; (void)argc; (void)argv;
    return sl_float((double)(next_u64() >> 11) * (1.0 / 9007199254740992.0));
}

void sl_open_os(sl_vm *vm, int argc, char **argv) {
    os_argc = argc;
    os_argv = argv;
    if (!started_ms) started_ms = wall_ms();
    rng_state ^= (uint64_t)wall_ms() * 0x9E3779B97F4A7C15ULL;
    if (!rng_state) rng_state = 0x9E3779B97F4A7C15ULL;

    struct { const char *name; sl_native_fn fn; } table[] = {
        { "io.write",      io_write },
        { "io.writeErr",   io_write_err },
        { "io.flush",      io_flush },
        { "io.readLine",   io_read_line },
        { "io.readAll",    io_read_all },
        { "io.eof",        io_eof },

        { "fs.read",       fs_read },
        { "fs.write",      fs_write },
        { "fs.append",     fs_append },
        { "fs.exists",     fs_exists },
        { "fs.isDir",      fs_is_dir },
        { "fs.size",       fs_size },
        { "fs.remove",     fs_remove },
        { "fs.rename",     fs_rename },
        { "fs.mkdir",      fs_mkdir },
        { "fs.list",       fs_list },

        { "os.args",       os_args },
        { "os.env",        os_env },
        { "os.hasEnv",     os_has_env },
        { "os.exit",       os_exit },
        { "os.platform",   os_platform },
        { "os.cwd",        os_cwd },
        { "os.run",        os_run },

        { "time.nowMs",    time_now_ms },
        { "time.monoMs",   time_mono_ms },
        { "time.sleepMs",  time_sleep_ms },
        { "time.iso",      time_iso },

        { "rand.seed",     rand_seed },
        { "rand.below",    rand_below },
        { "rand.real",     rand_real },
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        sl_register(vm, table[i].name, table[i].fn);
}
