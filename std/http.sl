// The `http` module: an HTTP and HTTPS client.
//
//     let r = http.get("https://example.com/api");
//     if http.ok(r) { print(r.body); } else { print("failed: ${r.status} ${r.error}"); }
//
// It runs the `curl` program, which ships with Windows 10 and later, macOS,
// and almost every Linux, so HTTPS, redirects and proxies behave as they do
// in the shell. Without `curl` on the PATH every request comes back with
// status 0 and an error saying so. Only the standard interpreter opens this.
//
// Failures that are not HTTP statuses (no network, bad host, timeout) give
// status 0 and the reason in `error`. An HTTP error status such as 404 is a
// normal response: look at `status`, or call `ok`.
//
// Bodies and headers are passed through files and quoted arguments, so any
// text is safe to send. On Windows the command goes through `cmd.exe`, so
// avoid `%` in a URL or header.

import fs;
import os;
import rand;
import strings;
import time;

struct Response {
    status: int;               // 0 if no response arrived
    body: str;
    headers: {str: str};       // names in lower case; the last response if redirected
    error: str;                // empty when a response arrived
}

// Seconds before a request is given up. Change with `setTimeout`.
var timeoutSeconds = 30;

func setTimeout(seconds: int) {
    timeoutSeconds = seconds;
}

// 2xx, and nothing went wrong on the way.
func ok(r: Response): bool {
    return r.status >= 200 && r.status < 300 && strings.isEmpty(r.error);
}

func shellQuote(s: str): str {
    if os.isWindows() {
        return "\"" + strings.replace(s, "\"", "\\\"") + "\"";
    }
    return "'" + strings.replace(s, "'", "'\\''") + "'";
}

func tempBase(): str {
    var dir = os.envOr("TMPDIR", "");
    if strings.isEmpty(dir) {
        if os.isWindows() {
            dir = os.envOr("TEMP", ".");
        } else {
            dir = "/tmp";
        }
    }
    return dir + "/solis-http-" + strings.fromInt(time.nowMs()) + "-" + strings.fromInt(rand.below(1000000));
}

func noHeaders(): {str: str} {
    var h: {str: str} = {};
    return h;
}

func parseHeaders(text: str): {str: str} {
    var out: {str: str} = {};
    for raw in strings.split(strings.replace(text, "\r\n", "\n"), "\n") {
        if strings.startsWith(raw, "HTTP/") {
            out = noHeaders();
            continue;
        }
        let colon = strings.indexOf(raw, ":");
        if colon > 0 {
            out[strings.lower(strings.sub(raw, 0, colon))] = strings.trim(strings.sub(raw, colon + 1, strings.len(raw)));
        }
    }
    return out;
}

// Sends one request. `body` may be empty; `headers` maps names to values.
func request(method: str, url: str, body: str, headers: {str: str}): Response {
    let base = tempBase();
    let headerFile = base + ".h";
    let bodyFile = base + ".b";
    let errFile = base + ".e";
    let sendFile = base + ".s";
    var cmd = "curl -sS -L --max-time " + strings.fromInt(timeoutSeconds) + " -X " + shellQuote(method);
    cmd = cmd + " -D " + shellQuote(headerFile) + " -o " + shellQuote(bodyFile);
    cmd = cmd + " -w " + shellQuote("%{http_code}");
    for name in keys(headers) {
        cmd = cmd + " -H " + shellQuote(name + ": " + headers[name]);
    }
    if !strings.isEmpty(body) {
        fs.write(sendFile, body);
        cmd = cmd + " --data-binary " + shellQuote("@" + sendFile);
    }
    cmd = cmd + " " + shellQuote(url) + " 2> " + shellQuote(errFile);

    let printed = os.output(cmd);
    let rc = os.lastStatus();
    var message = strings.trim(fs.read(errFile));
    var status = 0;
    if strings.isInt(strings.trim(printed)) {
        status = strings.toInt(strings.trim(printed));
    }
    if rc != 0 && strings.isEmpty(message) {
        message = "curl failed with status ${rc}";
        if rc == -1 || rc == 127 {
            message = "could not run curl";
        }
    }
    if rc == 0 {
        message = "";
    }
    let response = Response {
        status: status,
        body: fs.read(bodyFile),
        headers: parseHeaders(fs.read(headerFile)),
        error: message,
    };
    fs.remove(headerFile);
    fs.remove(bodyFile);
    fs.remove(errFile);
    fs.remove(sendFile);
    return response;
}

func get(url: str): Response {
    return request("GET", url, "", noHeaders());
}

func delete(url: str): Response {
    return request("DELETE", url, "", noHeaders());
}

func post(url: str, body: str): Response {
    return request("POST", url, body, noHeaders());
}

func put(url: str, body: str): Response {
    return request("PUT", url, body, noHeaders());
}

// POSTs `body` with a `Content-Type` of application/json.
func postJson(url: str, body: str): Response {
    var h: {str: str} = {};
    h["Content-Type"] = "application/json";
    return request("POST", url, body, h);
}

// Saves a URL's body to `path`. False if it failed or the status was not 2xx.
func download(url: str, path: str): bool {
    let r = get(url);
    if !ok(r) {
        return false;
    }
    return fs.write(path, r.body);
}
