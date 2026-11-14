// The `net` module: TCP and UDP sockets.
//
// A socket is an int handle: 1 or more when it works, -1 when it failed, and
// then `lastError()` says why. Handles are only meaningful to this module.
// Everything blocks; use the timeout parameters (or `setTimeout`) to bound
// the wait. Text is read and written as raw bytes, so any string can be sent.
//
//     let server = net.listen("127.0.0.1", 8080);
//     let conn = net.accept(server, -1);
//     let request = net.read(conn, 4096);
//     net.write(conn, "HTTP/1.0 200 OK\r\n\r\nhi");
//     net.close(conn);
//
// Only the standard interpreter opens this, and it is unavailable in the web
// build (`isAvailable()` is false there).

import strings;

extern func isAvailable(): bool;

// Starts listening. An empty host means every interface; port 0 picks a free
// one, which `port` then reports.
extern func listen(host: str, port: int): int;

// Waits up to `timeoutMs` for a client (negative: forever) and returns its
// connection, or -1 on timeout (`lastError()` is "timeout") or failure.
extern func accept(server: int, timeoutMs: int): int;

extern func connect(host: str, port: int): int;

// Up to `max` bytes, as soon as some arrive. "" means the other side closed
// (`lastError()` is "closed"), the timeout passed ("timeout"), or an error.
extern func read(conn: int, max: int): str;

// Sends everything; returns the byte count, or -1.
extern func write(conn: int, data: str): int;

// Milliseconds a read or write may wait on a connection or UDP socket;
// 0 waits forever.
extern func setTimeout(sock: int, ms: int): bool;

// A UDP socket bound to `host:port`.
extern func udp(host: str, port: int): int;
extern func sendTo(sock: int, host: str, port: int, data: str): int;

// One datagram, "" on timeout or error. `peer(0)` is the sender afterwards.
extern func recvFrom(sock: int, max: int): str;

extern func close(sock: int);

// The local port of a listener, connection or UDP socket, or -1.
extern func port(sock: int): int;

// "ip:port" of the remote end of a connection. `peer(0)` is the address of
// the last client accepted or datagram received.
extern func peer(sock: int): str;

// The first IP address a host name resolves to, or "".
extern func resolve(host: str): str;

extern func lastError(): str;

// Reads until `marker` has been seen or the connection ends, then returns
// everything read, which can include bytes after the marker.
func readUntil(conn: int, marker: str, limit: int): str {
    var got = "";
    while strings.byteLen(got) < limit && !strings.contains(got, marker) {
        let chunk = read(conn, 4096);
        if strings.isEmpty(chunk) {
            return got;
        }
        got = got + chunk;
    }
    return got;
}

// Reads exactly `count` bytes, or fewer if the connection ends first.
func readBytes(conn: int, count: int): str {
    var got = "";
    var have = 0;
    while have < count {
        let chunk = read(conn, count - have);
        if strings.isEmpty(chunk) {
            return got;
        }
        got = got + chunk;
        have += strings.byteLen(chunk);
    }
    return got;
}
