// The `net` and `sqlite` natives: TCP and UDP sockets, and SQLite loaded from
// the system at run time.
//
// Included from solis_os.c, which supplies want_s / want_i. Neither module has
// a build-time dependency: sockets are the operating system's, and the SQLite
// library is opened with dlopen / LoadLibrary the first time a script asks, so
// a machine without it only loses `sqlite`, and the web build (which has
// neither) reports "unavailable" instead of failing to link.

#if defined(__EMSCRIPTEN__)
#  define SL_NO_NET 1
#endif

// --- sockets -------------------------------------------------------------------

#ifndef SL_NO_NET
#  ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    ifdef _MSC_VER
#      pragma comment(lib, "ws2_32")
#    endif
typedef SOCKET net_fd;
#    define NET_BAD INVALID_SOCKET
#    define net_close_fd closesocket
#    define NET_WOULDBLOCK (WSAGetLastError() == WSAETIMEDOUT || WSAGetLastError() == WSAEWOULDBLOCK)
#  else
#    include <arpa/inet.h>
#    include <errno.h>
#    include <netdb.h>
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#    include <sys/select.h>
#    include <sys/socket.h>
typedef int net_fd;
#    define NET_BAD (-1)
#    define net_close_fd close
#    define NET_WOULDBLOCK (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
#  endif

enum { NET_FREE = 0, NET_LISTENER, NET_STREAM, NET_UDP };
#define NET_MAX 256

static struct { int kind; net_fd fd; } net_tab[NET_MAX];
static char net_error[160];
static char net_last_peer[96];
static bool net_started = false;

static void net_set_error(const char *what) {
    snprintf(net_error, sizeof net_error, "%s", what);
}

static void net_set_error_os(const char *what) {
#ifdef _WIN32
    snprintf(net_error, sizeof net_error, "%s (error %d)", what, WSAGetLastError());
#else
    snprintf(net_error, sizeof net_error, "%s: %s", what, strerror(errno));
#endif
}

static bool net_start(void) {
    if (net_started) return true;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { net_set_error("WSAStartup failed"); return false; }
#endif
    net_started = true;
    return true;
}

// Handles are slot numbers plus one, so 0 is never valid.
static int net_alloc(int kind, net_fd fd) {
    for (int i = 0; i < NET_MAX; i++) {
        if (net_tab[i].kind == NET_FREE) {
            net_tab[i].kind = kind;
            net_tab[i].fd = fd;
            return i + 1;
        }
    }
    net_close_fd(fd);
    net_set_error("too many open sockets");
    return -1;
}

static bool net_valid(int64_t h, int kind_a, int kind_b) {
    if (h < 1 || h > NET_MAX) return false;
    int k = net_tab[h - 1].kind;
    return k != NET_FREE && (k == kind_a || k == kind_b);
}

static void net_addr_text(const struct sockaddr *sa, char *out, size_t cap) {
    char host[64] = "?";
    int port = 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &in->sin_addr, host, sizeof host);
        port = ntohs(in->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in = (const struct sockaddr_in6 *)sa;
        inet_ntop(AF_INET6, &in->sin6_addr, host, sizeof host);
        port = ntohs(in->sin6_port);
    }
    snprintf(out, cap, "%s:%d", host, port);
}

// Resolves and walks the candidates, either binding (listen / udp) or connecting.
static net_fd net_open_inet(const char *host, int64_t port, int socktype, bool bind_it) {
    if (!net_start()) return NET_BAD;
    if (port < 0 || port > 65535) { net_set_error("port out of range"); return NET_BAD; }
    struct addrinfo hints, *res = NULL, *p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
    if (bind_it) hints.ai_flags = AI_PASSIVE;
    char service[16];
    snprintf(service, sizeof service, "%d", (int)port);
    int rc = getaddrinfo(host[0] ? host : NULL, service, &hints, &res);
    if (rc != 0) {
        snprintf(net_error, sizeof net_error, "cannot resolve %s: %s", host, gai_strerror(rc));
        return NET_BAD;
    }
    net_fd fd = NET_BAD;
    for (p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == NET_BAD) continue;
        if (bind_it) {
            int on = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
            if (bind(fd, p->ai_addr, (int)p->ai_addrlen) == 0 &&
                (socktype == SOCK_DGRAM || listen(fd, 128) == 0)) break;
        } else if (connect(fd, p->ai_addr, (int)p->ai_addrlen) == 0) {
            break;
        }
        net_close_fd(fd);
        fd = NET_BAD;
    }
    if (fd == NET_BAD) net_set_error_os(bind_it ? "cannot bind" : "cannot connect");
    freeaddrinfo(res);
    return fd;
}

static sl_value net_listen(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *host = want_s(vm, argc, argv, 0, "net.listen expects a host and a port", &n);
    int64_t port = want_i(vm, argc, argv, 1, "net.listen expects a host and a port");
    if (!host) return sl_void();
    net_fd fd = net_open_inet(host, port, SOCK_STREAM, true);
    return sl_int(fd == NET_BAD ? -1 : net_alloc(NET_LISTENER, fd));
}

static sl_value net_connect(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *host = want_s(vm, argc, argv, 0, "net.connect expects a host and a port", &n);
    int64_t port = want_i(vm, argc, argv, 1, "net.connect expects a host and a port");
    if (!host) return sl_void();
    net_fd fd = net_open_inet(host, port, SOCK_STREAM, false);
    if (fd == NET_BAD) return sl_int(-1);
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
    return sl_int(net_alloc(NET_STREAM, fd));
}

static sl_value net_udp(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *host = want_s(vm, argc, argv, 0, "net.udp expects a host and a port", &n);
    int64_t port = want_i(vm, argc, argv, 1, "net.udp expects a host and a port");
    if (!host) return sl_void();
    net_fd fd = net_open_inet(host, port, SOCK_DGRAM, true);
    return sl_int(fd == NET_BAD ? -1 : net_alloc(NET_UDP, fd));
}

// Waits up to `ms` (negative: forever) for the socket to be readable.
static int net_wait_readable(net_fd fd, int64_t ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv, *tvp = NULL;
    if (ms >= 0) { tv.tv_sec = (long)(ms / 1000); tv.tv_usec = (long)(ms % 1000) * 1000; tvp = &tv; }
    return select((int)fd + 1, &set, NULL, NULL, tvp);
}

static sl_value net_accept(sl_vm *vm, int argc, sl_value *argv) {
    int64_t h = want_i(vm, argc, argv, 0, "net.accept expects a listener and a timeout");
    int64_t ms = want_i(vm, argc, argv, 1, "net.accept expects a listener and a timeout");
    if (!net_valid(h, NET_LISTENER, NET_LISTENER)) { net_set_error("not a listening socket"); return sl_int(-1); }
    int ready = net_wait_readable(net_tab[h - 1].fd, ms);
    if (ready == 0) { net_set_error("timeout"); return sl_int(-1); }
    if (ready < 0) { net_set_error_os("accept failed"); return sl_int(-1); }
    struct sockaddr_storage peer;
    socklen_t len = sizeof peer;
    net_fd fd = accept(net_tab[h - 1].fd, (struct sockaddr *)&peer, &len);
    if (fd == NET_BAD) { net_set_error_os("accept failed"); return sl_int(-1); }
    net_addr_text((struct sockaddr *)&peer, net_last_peer, sizeof net_last_peer);
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
    return sl_int(net_alloc(NET_STREAM, fd));
}

static sl_value net_read(sl_vm *vm, int argc, sl_value *argv) {
    int64_t h = want_i(vm, argc, argv, 0, "net.read expects a socket and a size");
    int64_t max = want_i(vm, argc, argv, 1, "net.read expects a socket and a size");
    if (!net_valid(h, NET_STREAM, NET_STREAM)) { net_set_error("not a connection"); return sl_cstr(vm, ""); }
    if (max < 1) max = 1;
    if (max > (1 << 20)) max = 1 << 20;
    char *buf = (char *)malloc((size_t)max);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }
    net_error[0] = 0;
    int got = (int)recv(net_tab[h - 1].fd, buf, (int)max, 0);
    sl_value out;
    if (got > 0) out = sl_str(vm, buf, (size_t)got);
    else {
        if (got == 0) net_set_error("closed");
        else if (NET_WOULDBLOCK) net_set_error("timeout");
        else net_set_error_os("read failed");
        out = sl_cstr(vm, "");
    }
    free(buf);
    return out;
}

static sl_value net_write(sl_vm *vm, int argc, sl_value *argv) {
    int64_t h = want_i(vm, argc, argv, 0, "net.write expects a socket and a string");
    size_t n; const char *p = want_s(vm, argc, argv, 1, "net.write expects a socket and a string", &n);
    if (!p) return sl_void();
    if (!net_valid(h, NET_STREAM, NET_STREAM)) { net_set_error("not a connection"); return sl_int(-1); }
    size_t sent = 0;
    while (sent < n) {
        int k = (int)send(net_tab[h - 1].fd, p + sent, (int)(n - sent > (1 << 20) ? (1 << 20) : n - sent), 0);
        if (k <= 0) { net_set_error_os("write failed"); return sl_int(-1); }
        sent += (size_t)k;
    }
    return sl_int((int64_t)sent);
}

static sl_value net_send_to(sl_vm *vm, int argc, sl_value *argv) {
    int64_t h = want_i(vm, argc, argv, 0, "net.sendTo expects a socket, host, port and data");
    size_t hn; const char *host = want_s(vm, argc, argv, 1, "net.sendTo expects a socket, host, port and data", &hn);
    int64_t port = want_i(vm, argc, argv, 2, "net.sendTo expects a socket, host, port and data");
    size_t n; const char *p = want_s(vm, argc, argv, 3, "net.sendTo expects a socket, host, port and data", &n);
    if (!host || !p) return sl_void();
    if (!net_valid(h, NET_UDP, NET_UDP)) { net_set_error("not a UDP socket"); return sl_int(-1); }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    char service[16];
    snprintf(service, sizeof service, "%d", (int)port);
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res) { net_set_error("cannot resolve host"); return sl_int(-1); }
    int k = (int)sendto(net_tab[h - 1].fd, p, (int)n, 0, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (k < 0) { net_set_error_os("send failed"); return sl_int(-1); }
    return sl_int(k);
}

static sl_value net_recv_from(sl_vm *vm, int argc, sl_value *argv) {
    int64_t h = want_i(vm, argc, argv, 0, "net.recvFrom expects a socket and a size");
    int64_t max = want_i(vm, argc, argv, 1, "net.recvFrom expects a socket and a size");
    if (!net_valid(h, NET_UDP, NET_UDP)) { net_set_error("not a UDP socket"); return sl_cstr(vm, ""); }
    if (max < 1) max = 1;
    if (max > 65535) max = 65535;
    char *buf = (char *)malloc((size_t)max);
    if (!buf) { sl_fail(vm, "out of memory"); return sl_void(); }
    struct sockaddr_storage from;
    socklen_t flen = sizeof from;
    net_error[0] = 0;
    int got = (int)recvfrom(net_tab[h - 1].fd, buf, (int)max, 0, (struct sockaddr *)&from, &flen);
    sl_value out;
    if (got >= 0) {
        net_addr_text((struct sockaddr *)&from, net_last_peer, sizeof net_last_peer);
        out = sl_str(vm, buf, (size_t)got);
    } else {
        if (NET_WOULDBLOCK) net_set_error("timeout"); else net_set_error_os("receive failed");
        out = sl_cstr(vm, "");
    }
    free(buf);
    return out;
}

// Read and write timeout in milliseconds for a connection or UDP socket; 0 is forever.
static sl_value net_set_timeout(sl_vm *vm, int argc, sl_value *argv) {
    int64_t h = want_i(vm, argc, argv, 0, "net.setTimeout expects a socket and milliseconds");
    int64_t ms = want_i(vm, argc, argv, 1, "net.setTimeout expects a socket and milliseconds");
    if (!net_valid(h, NET_STREAM, NET_UDP)) { net_set_error("not a connection"); return sl_bool(false); }
    if (ms < 0) ms = 0;
#ifdef _WIN32
    DWORD tv = (DWORD)ms;
#else
    struct timeval tv;
    tv.tv_sec = (long)(ms / 1000);
    tv.tv_usec = (long)(ms % 1000) * 1000;
#endif
    int a = setsockopt(net_tab[h - 1].fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    int b = setsockopt(net_tab[h - 1].fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);
    return sl_bool(a == 0 && b == 0);
}

static sl_value net_close(sl_vm *vm, int argc, sl_value *argv) {
    int64_t h = want_i(vm, argc, argv, 0, "net.close expects a socket");
    if (h < 1 || h > NET_MAX || net_tab[h - 1].kind == NET_FREE) return sl_void();
    net_close_fd(net_tab[h - 1].fd);
    net_tab[h - 1].kind = NET_FREE;
    return sl_void();
}

// The local port a socket is bound to; useful after listening on port 0.
static sl_value net_port(sl_vm *vm, int argc, sl_value *argv) {
    int64_t h = want_i(vm, argc, argv, 0, "net.port expects a socket");
    if (!net_valid(h, NET_LISTENER, NET_UDP) && !net_valid(h, NET_STREAM, NET_STREAM)) return sl_int(-1);
    struct sockaddr_storage sa;
    socklen_t len = sizeof sa;
    if (getsockname(net_tab[h - 1].fd, (struct sockaddr *)&sa, &len) != 0) return sl_int(-1);
    if (sa.ss_family == AF_INET) return sl_int(ntohs(((struct sockaddr_in *)&sa)->sin_port));
    if (sa.ss_family == AF_INET6) return sl_int(ntohs(((struct sockaddr_in6 *)&sa)->sin6_port));
    return sl_int(-1);
}

static sl_value net_peer(sl_vm *vm, int argc, sl_value *argv) {
    int64_t h = want_i(vm, argc, argv, 0, "net.peer expects a socket");
    if (h == 0) return sl_cstr(vm, net_last_peer);          // the sender of the last accept or recvFrom
    if (!net_valid(h, NET_STREAM, NET_STREAM)) return sl_cstr(vm, "");
    struct sockaddr_storage sa;
    socklen_t len = sizeof sa;
    char text[96];
    if (getpeername(net_tab[h - 1].fd, (struct sockaddr *)&sa, &len) != 0) return sl_cstr(vm, "");
    net_addr_text((struct sockaddr *)&sa, text, sizeof text);
    return sl_cstr(vm, text);
}

static sl_value net_resolve(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *host = want_s(vm, argc, argv, 0, "net.resolve expects a host name", &n);
    if (!host) return sl_void();
    if (!net_start()) return sl_cstr(vm, "");
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) { net_set_error("cannot resolve host"); return sl_cstr(vm, ""); }
    char text[96];
    net_addr_text(res->ai_addr, text, sizeof text);
    freeaddrinfo(res);
    char *colon = strrchr(text, ':');
    if (colon) *colon = 0;
    return sl_cstr(vm, text);
}

static sl_value net_last_error(sl_vm *vm, int argc, sl_value *argv) {
    (void)argc; (void)argv;
    return sl_cstr(vm, net_error);
}

static sl_value net_is_available(sl_vm *vm, int argc, sl_value *argv) {
    (void)vm; (void)argc; (void)argv;
    return sl_bool(true);
}

#else   // SL_NO_NET: the same names, all failing, so scripts still link

static sl_value net_none_int(sl_vm *vm, int argc, sl_value *argv) { (void)vm; (void)argc; (void)argv; return sl_int(-1); }
static sl_value net_none_str(sl_vm *vm, int argc, sl_value *argv) { (void)argc; (void)argv; return sl_cstr(vm, ""); }
static sl_value net_none_bool(sl_vm *vm, int argc, sl_value *argv) { (void)vm; (void)argc; (void)argv; return sl_bool(false); }
static sl_value net_none_void(sl_vm *vm, int argc, sl_value *argv) { (void)vm; (void)argc; (void)argv; return sl_void(); }
static sl_value net_unavailable_text(sl_vm *vm, int argc, sl_value *argv) {
    (void)argc; (void)argv;
    return sl_cstr(vm, "sockets are not available on this platform");
}
#  define net_listen net_none_int
#  define net_connect net_none_int
#  define net_udp net_none_int
#  define net_accept net_none_int
#  define net_read net_none_str
#  define net_write net_none_int
#  define net_send_to net_none_int
#  define net_recv_from net_none_str
#  define net_set_timeout net_none_bool
#  define net_close net_none_void
#  define net_port net_none_int
#  define net_peer net_none_str
#  define net_resolve net_none_str
#  define net_last_error net_unavailable_text
#  define net_is_available net_none_bool
#endif

// --- sqlite ----------------------------------------------------------------------

#if defined(__EMSCRIPTEN__)
#  define SL_NO_SQLITE_LOAD 1
#endif

typedef struct sl_sqlite_db sl_sqlite_db;
typedef struct sl_sqlite_stmt sl_sqlite_stmt;

static struct {
    bool tried, ok;
    int  (*open_v2)(const char *, sl_sqlite_db **, int, const char *);
    int  (*close_v2)(sl_sqlite_db *);
    int  (*prepare_v2)(sl_sqlite_db *, const char *, int, sl_sqlite_stmt **, const char **);
    int  (*step)(sl_sqlite_stmt *);
    int  (*finalize)(sl_sqlite_stmt *);
    int  (*column_count)(sl_sqlite_stmt *);
    const char *(*column_name)(sl_sqlite_stmt *, int);
    const unsigned char *(*column_text)(sl_sqlite_stmt *, int);
    int  (*column_bytes)(sl_sqlite_stmt *, int);
    int  (*bind_text)(sl_sqlite_stmt *, int, const char *, int, void (*)(void *));
    int  (*bind_count)(sl_sqlite_stmt *);
    const char *(*errmsg)(sl_sqlite_db *);
    int  (*changes)(sl_sqlite_db *);
    long long (*last_rowid)(sl_sqlite_db *);
    const char *(*libversion)(void);
} lite;

static char lite_error[256];
#define LITE_MAX 32
static sl_sqlite_db *lite_tab[LITE_MAX];

#ifndef SL_NO_SQLITE_LOAD
#  ifdef _WIN32
#    define LITE_OPEN(name) ((void *)LoadLibraryA(name))
#    define LITE_SYM(h, name) ((void *)GetProcAddress((HMODULE)(h), name))
#  else
#    include <dlfcn.h>
#    define LITE_OPEN(name) dlopen(name, RTLD_NOW)
#    define LITE_SYM(h, name) dlsym(h, name)
#  endif

// Function pointers cannot portably be converted from void *, so copy the bits.
#  define LITE_BIND(field, name) do { void *p_ = LITE_SYM(lib, name); if (!p_) return false; memcpy(&lite.field, &p_, sizeof p_); } while (0)

static bool lite_load(void) {
    if (lite.tried) return lite.ok;
    lite.tried = true;
    static const char *const names[] = {
#  ifdef _WIN32
        "sqlite3.dll", "winsqlite3.dll",
#  elif defined(__APPLE__)
        "libsqlite3.dylib",
#  else
        "libsqlite3.so.0", "libsqlite3.so",
#  endif
        NULL };
    void *lib = NULL;
    for (int i = 0; names[i] && !lib; i++) lib = LITE_OPEN(names[i]);
    if (!lib) { snprintf(lite_error, sizeof lite_error, "the SQLite library was not found on this system"); return false; }
    LITE_BIND(open_v2, "sqlite3_open_v2");
    LITE_BIND(close_v2, "sqlite3_close_v2");
    LITE_BIND(prepare_v2, "sqlite3_prepare_v2");
    LITE_BIND(step, "sqlite3_step");
    LITE_BIND(finalize, "sqlite3_finalize");
    LITE_BIND(column_count, "sqlite3_column_count");
    LITE_BIND(column_name, "sqlite3_column_name");
    LITE_BIND(column_text, "sqlite3_column_text");
    LITE_BIND(column_bytes, "sqlite3_column_bytes");
    LITE_BIND(bind_text, "sqlite3_bind_text");
    LITE_BIND(bind_count, "sqlite3_bind_parameter_count");
    LITE_BIND(errmsg, "sqlite3_errmsg");
    LITE_BIND(changes, "sqlite3_changes");
    LITE_BIND(last_rowid, "sqlite3_last_insert_rowid");
    LITE_BIND(libversion, "sqlite3_libversion");
    lite.ok = true;
    return true;
}
#else
static bool lite_load(void) {
    snprintf(lite_error, sizeof lite_error, "SQLite is not available on this platform");
    return false;
}
#endif

static sl_value lite_available(sl_vm *vm, int argc, sl_value *argv) {
    (void)vm; (void)argc; (void)argv;
    return sl_bool(lite_load());
}

static sl_value lite_version(sl_vm *vm, int argc, sl_value *argv) {
    (void)argc; (void)argv;
    return sl_cstr(vm, lite_load() ? lite.libversion() : "");
}

static sl_value lite_last_error(sl_vm *vm, int argc, sl_value *argv) {
    (void)argc; (void)argv;
    return sl_cstr(vm, lite_error);
}

static sl_sqlite_db *lite_db(int64_t h) {
    if (h < 1 || h > LITE_MAX) return NULL;
    return lite_tab[h - 1];
}

static sl_value lite_open(sl_vm *vm, int argc, sl_value *argv) {
    size_t n; const char *path = want_s(vm, argc, argv, 0, "sqlite.open expects a path", &n);
    if (!path) return sl_void();
    if (!lite_load()) return sl_int(-1);
    int slot = -1;
    for (int i = 0; i < LITE_MAX; i++) if (!lite_tab[i]) { slot = i; break; }
    if (slot < 0) { snprintf(lite_error, sizeof lite_error, "too many open databases"); return sl_int(-1); }
    sl_sqlite_db *db = NULL;
    int rc = lite.open_v2(path, &db, 2 | 4, NULL);       // read-write, create
    if (rc != 0) {
        snprintf(lite_error, sizeof lite_error, "%s", db ? lite.errmsg(db) : "cannot open database");
        if (db) lite.close_v2(db);
        return sl_int(-1);
    }
    lite_tab[slot] = db;
    lite_error[0] = 0;
    return sl_int(slot + 1);
}

static sl_value lite_close(sl_vm *vm, int argc, sl_value *argv) {
    int64_t h = want_i(vm, argc, argv, 0, "sqlite.close expects a database");
    sl_sqlite_db *db = lite_db(h);
    if (db) { lite.close_v2(db); lite_tab[h - 1] = NULL; }
    return sl_void();
}

// Binds the text parameters to `st`. A parameter count that does not match is an error.
static bool lite_bind_all(sl_sqlite_stmt *st, sl_value params) {
    int64_t given = params.type == SL_OBJ ? sl_array_len(params) : 0;
    int wanted = lite.bind_count(st);
    if (given != wanted) {
        snprintf(lite_error, sizeof lite_error, "the statement has %d parameter(s) but %d were given", wanted, (int)given);
        return false;
    }
    for (int64_t i = 0; i < given; i++) {
        size_t n;
        sl_value v = sl_array_get(params, i);
        const char *p = sl_as_str(v, &n);
        if (lite.bind_text(st, (int)i + 1, p, (int)n, (void (*)(void *))(intptr_t)-1) != 0) return false;
    }
    return true;
}

// Runs every statement in `sql`; `?` parameters go to the first one. With
// `rows`, the first statement's result is collected: a row of column names,
// then one row per result, every cell as text (NULL is "").
static bool lite_run(sl_vm *vm, int64_t h, const char *sql, sl_value params, sl_value rows, int64_t *changed) {
    sl_sqlite_db *db = lite_db(h);
    if (!db) { snprintf(lite_error, sizeof lite_error, "not an open database"); return false; }
    const char *rest = sql;
    bool first = true;
    *changed = 0;
    while (*rest) {
        sl_sqlite_stmt *st = NULL;
        const char *tail = NULL;
        if (lite.prepare_v2(db, rest, -1, &st, &tail) != 0) {
            snprintf(lite_error, sizeof lite_error, "%s", lite.errmsg(db));
            return false;
        }
        rest = tail ? tail : "";
        if (!st) continue;                                  // only whitespace or a comment
        if (first && !lite_bind_all(st, params)) { lite.finalize(st); return false; }
        if (!first && lite.bind_count(st) > 0) {
            snprintf(lite_error, sizeof lite_error, "only the first statement can take parameters");
            lite.finalize(st);
            return false;
        }
        bool collect = first && rows.type == SL_OBJ;
        if (collect) {
            int cols = lite.column_count(st);
            sl_value head = sl_array(vm, cols);
            for (int c = 0; c < cols; c++) sl_array_push(vm, head, sl_cstr(vm, lite.column_name(st, c)));
            sl_array_push(vm, rows, head);
        }
        int rc;
        while ((rc = lite.step(st)) == 100) {              // SQLITE_ROW
            if (!collect) continue;
            int cols = lite.column_count(st);
            sl_value row = sl_array(vm, cols);
            for (int c = 0; c < cols; c++) {
                const unsigned char *t = lite.column_text(st, c);
                int len = lite.column_bytes(st, c);
                sl_array_push(vm, row, t ? sl_str(vm, (const char *)t, (size_t)len) : sl_cstr(vm, ""));
            }
            sl_array_push(vm, rows, row);
        }
        if (rc != 101) {                                    // SQLITE_DONE
            snprintf(lite_error, sizeof lite_error, "%s", lite.errmsg(db));
            lite.finalize(st);
            return false;
        }
        *changed += lite.changes(db);
        lite.finalize(st);
        first = false;
    }
    lite_error[0] = 0;
    return true;
}

// Rows changed by the statements, or -1 on an error (see sqlite.error).
static sl_value lite_exec(sl_vm *vm, int argc, sl_value *argv) {
    int64_t h = want_i(vm, argc, argv, 0, "sqlite.exec expects a database, SQL and parameters");
    size_t n; const char *sql = want_s(vm, argc, argv, 1, "sqlite.exec expects a database, SQL and parameters", &n);
    if (!sql || argc < 3) return sl_void();
    int64_t changed;
    if (!lite_run(vm, h, sql, argv[2], sl_void(), &changed)) return sl_int(-1);
    return sl_int(changed);
}

static sl_value lite_query(sl_vm *vm, int argc, sl_value *argv) {
    int64_t h = want_i(vm, argc, argv, 0, "sqlite.query expects a database, SQL and parameters");
    size_t n; const char *sql = want_s(vm, argc, argv, 1, "sqlite.query expects a database, SQL and parameters", &n);
    if (!sql || argc < 3) return sl_void();
    sl_value rows = sl_array(vm, 8);
    if (rows.type != SL_OBJ) return sl_void();
    int64_t changed;
    if (!lite_run(vm, h, sql, argv[2], rows, &changed)) {
        sl_release(vm, rows);
        return sl_array(vm, 0);
    }
    return rows;
}

static sl_value lite_last_insert_id(sl_vm *vm, int argc, sl_value *argv) {
    int64_t h = want_i(vm, argc, argv, 0, "sqlite.lastInsertId expects a database");
    sl_sqlite_db *db = lite_db(h);
    return sl_int(db && lite.ok ? (int64_t)lite.last_rowid(db) : -1);
}

#define NET_NATIVES \
        { "net.isAvailable",     net_is_available }, \
        { "net.listen",          net_listen }, \
        { "net.accept",          net_accept }, \
        { "net.connect",         net_connect }, \
        { "net.udp",             net_udp }, \
        { "net.read",            net_read }, \
        { "net.write",           net_write }, \
        { "net.sendTo",          net_send_to }, \
        { "net.recvFrom",        net_recv_from }, \
        { "net.setTimeout",      net_set_timeout }, \
        { "net.close",           net_close }, \
        { "net.port",            net_port }, \
        { "net.peer",            net_peer }, \
        { "net.resolve",         net_resolve }, \
        { "net.lastError",       net_last_error }, \
        { "sqlite.available",    lite_available }, \
        { "sqlite.version",      lite_version }, \
        { "sqlite.open",         lite_open }, \
        { "sqlite.close",        lite_close }, \
        { "sqlite.exec",         lite_exec }, \
        { "sqlite.query",        lite_query }, \
        { "sqlite.lastInsertId", lite_last_insert_id }, \
        { "sqlite.error",        lite_last_error },
