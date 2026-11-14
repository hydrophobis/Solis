// The `log` module: leveled messages on standard error, one line each.
//
//     log.info("listening on port ${port}");
//     // 2026-10-04T18:33:07Z INFO listening on port 8080
//
// Messages below the current level are dropped. The default level is `info`.
// Only the standard interpreter opens this, since it needs the clock and the
// error stream.

import io;
import strings;
import time;

let DEBUG = 0;
let INFO = 1;
let WARN = 2;
let ERROR = 3;
let OFF = 4;

var level = 1;

func setLevel(n: int) {
    level = n;
}

func getLevel(): int {
    return level;
}

// "debug", "info", "warn", "error" or "off" (any case); other text leaves the
// level unchanged. Returns whether it was understood.
func setLevelByName(name: str): bool {
    let n = strings.lower(name);
    if n == "debug" {
        level = DEBUG;
    } else if n == "info" {
        level = INFO;
    } else if n == "warn" || n == "warning" {
        level = WARN;
    } else if n == "error" {
        level = ERROR;
    } else if n == "off" || n == "none" {
        level = OFF;
    } else {
        return false;
    }
    return true;
}

func emit(at: int, label: str, message: str) {
    if at >= level {
        io.errLine(time.iso() + " " + label + " " + message);
    }
}

func debug(message: str) {
    emit(DEBUG, "DEBUG", message);
}

func info(message: str) {
    emit(INFO, "INFO", message);
}

func warn(message: str) {
    emit(WARN, "WARN", message);
}

func error(message: str) {
    emit(ERROR, "ERROR", message);
}
