// The `httpd` module: a small HTTP/1.1 server, one request at a time.
//
//     let server = httpd.listen("127.0.0.1", 8080);
//     while true {
//         let req = httpd.next(server);
//         if !req.ok { continue; }
//         if req.path == "/" {
//             httpd.text(req, 200, "hello");
//         } else {
//             httpd.text(req, 404, "not found");
//         }
//     }
//
// `next` waits for a client, reads its request, and returns it; you answer
// with one of the `respond` functions, which also close the connection. It
// handles one connection at a time and does not keep connections alive, which
// suits tools, webhooks and local services rather than public traffic. There
// is no TLS; put a reverse proxy in front for HTTPS. Built on `net`.

import net;
import strings;

struct Request {
    ok: bool;                  // false when the client sent something unusable; just call `next` again
    method: str;               // "GET", "POST", ...
    path: str;                 // "/a/b", without the query
    query: str;                // after the `?`, undecoded; see `url.parseQuery`
    headers: {str: str};       // names in lower case
    body: str;
    peer: str;                 // the client's "ip:port"
    conn: int;                 // pass the request itself to `respond`
}

// The most header bytes and body bytes accepted.
var maxHeader = 65536;
var maxBody = 10485760;

func listen(host: str, port: int): int {
    return net.listen(host, port);
}

func stop(server: int) {
    net.close(server);
}

func emptyHeaders(): {str: str} {
    var h: {str: str} = {};
    return h;
}

func badRequest(conn: int): Request {
    return Request { ok: false, method: "", path: "", query: "", headers: emptyHeaders(), body: "", peer: "", conn: conn };
}

// Waits for the next client (forever) and reads its request. A client that
// connects and sends nothing for ten seconds, or sends garbage, gives a
// request with `ok` false after a 400 response.
func next(server: int): Request {
    let conn = net.accept(server, -1);
    if conn < 0 {
        return badRequest(-1);
    }
    return read(conn);
}

// Like `next` but gives up after `ms` milliseconds with `ok` false and `conn` -1.
func nextWithin(server: int, ms: int): Request {
    let conn = net.accept(server, ms);
    if conn < 0 {
        return badRequest(-1);
    }
    return read(conn);
}

func read(conn: int): Request {
    net.setTimeout(conn, 10000);
    let peer = net.peer(conn);
    let raw = net.readUntil(conn, "\r\n\r\n", maxHeader);
    let split = strings.indexOf(raw, "\r\n\r\n");
    if split < 0 {
        net.write(conn, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        net.close(conn);
        return badRequest(-1);
    }
    let head = strings.sub(raw, 0, split);
    var body = strings.sub(raw, split + 4, strings.len(raw));
    let lines = strings.split(head, "\r\n");
    let first = strings.split(lines[0], " ");
    if len(first) < 2 {
        net.write(conn, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        net.close(conn);
        return badRequest(-1);
    }
    var headers = emptyHeaders();
    var i = 1;
    while i < len(lines) {
        let colon = strings.indexOf(lines[i], ":");
        if colon > 0 {
            headers[strings.lower(strings.sub(lines[i], 0, colon))] = strings.trim(strings.sub(lines[i], colon + 1, strings.len(lines[i])));
        }
        i += 1;
    }
    // the body: Content-Length bytes, some of which may already be in `body`
    var want = 0;
    if contains(headers, "content-length") && strings.isInt(headers["content-length"]) {
        want = strings.toInt(headers["content-length"]);
    }
    if want > maxBody {
        net.write(conn, "HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        net.close(conn);
        return badRequest(-1);
    }
    let have = strings.byteLen(body);
    if have < want {
        body = body + net.readBytes(conn, want - have);
    }
    var target = first[1];
    var query = "";
    let q = strings.indexOf(target, "?");
    if q >= 0 {
        query = strings.sub(target, q + 1, strings.len(target));
        target = strings.sub(target, 0, q);
    }
    return Request { ok: true, method: first[0], path: target, query: query, headers: headers, body: body, peer: peer, conn: conn };
}

func statusText(status: int): str {
    if status == 200 { return "OK"; }
    if status == 201 { return "Created"; }
    if status == 202 { return "Accepted"; }
    if status == 204 { return "No Content"; }
    if status == 301 { return "Moved Permanently"; }
    if status == 302 { return "Found"; }
    if status == 303 { return "See Other"; }
    if status == 304 { return "Not Modified"; }
    if status == 307 { return "Temporary Redirect"; }
    if status == 400 { return "Bad Request"; }
    if status == 401 { return "Unauthorized"; }
    if status == 403 { return "Forbidden"; }
    if status == 404 { return "Not Found"; }
    if status == 405 { return "Method Not Allowed"; }
    if status == 409 { return "Conflict"; }
    if status == 413 { return "Payload Too Large"; }
    if status == 415 { return "Unsupported Media Type"; }
    if status == 422 { return "Unprocessable Entity"; }
    if status == 429 { return "Too Many Requests"; }
    if status == 500 { return "Internal Server Error"; }
    if status == 501 { return "Not Implemented"; }
    if status == 503 { return "Service Unavailable"; }
    return "Status";
}

// Answers with extra headers (name to value) and closes the connection.
func respondWith(req: Request, status: int, contentType: str, headers: {str: str}, body: str) {
    var out = "HTTP/1.1 " + strings.fromInt(status) + " " + statusText(status) + "\r\n";
    out = out + "Content-Type: " + contentType + "\r\n";
    out = out + "Content-Length: " + strings.fromInt(strings.byteLen(body)) + "\r\n";
    out = out + "Connection: close\r\n";
    for name in keys(headers) {
        out = out + name + ": " + headers[name] + "\r\n";
    }
    out = out + "\r\n";
    if req.method != "HEAD" {
        out = out + body;
    }
    net.write(req.conn, out);
    net.close(req.conn);
}

func respond(req: Request, status: int, contentType: str, body: str) {
    respondWith(req, status, contentType, emptyHeaders(), body);
}

func text(req: Request, status: int, body: str) {
    respond(req, status, "text/plain; charset=utf-8", body);
}

func html(req: Request, status: int, body: str) {
    respond(req, status, "text/html; charset=utf-8", body);
}

// `body` is JSON text, for instance from `json.stringify`.
func json(req: Request, status: int, body: str) {
    respond(req, status, "application/json", body);
}

func redirect(req: Request, location: str) {
    var h = emptyHeaders();
    h["Location"] = location;
    respondWith(req, 302, "text/plain; charset=utf-8", h, "");
}

// The MIME type for a file name, by extension; application/octet-stream if
// it is not one of the common ones.
func mimeType(file: str): str {
    let lower = strings.lower(file);
    if strings.endsWith(lower, ".html") || strings.endsWith(lower, ".htm") { return "text/html; charset=utf-8"; }
    if strings.endsWith(lower, ".css") { return "text/css; charset=utf-8"; }
    if strings.endsWith(lower, ".js") || strings.endsWith(lower, ".mjs") { return "text/javascript; charset=utf-8"; }
    if strings.endsWith(lower, ".json") { return "application/json"; }
    if strings.endsWith(lower, ".txt") || strings.endsWith(lower, ".md") { return "text/plain; charset=utf-8"; }
    if strings.endsWith(lower, ".svg") { return "image/svg+xml"; }
    if strings.endsWith(lower, ".png") { return "image/png"; }
    if strings.endsWith(lower, ".jpg") || strings.endsWith(lower, ".jpeg") { return "image/jpeg"; }
    if strings.endsWith(lower, ".gif") { return "image/gif"; }
    if strings.endsWith(lower, ".ico") { return "image/x-icon"; }
    if strings.endsWith(lower, ".wasm") { return "application/wasm"; }
    if strings.endsWith(lower, ".xml") { return "application/xml"; }
    if strings.endsWith(lower, ".pdf") { return "application/pdf"; }
    return "application/octet-stream";
}
