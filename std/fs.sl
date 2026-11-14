// The `fs` module: files and directories.
//
// No optionals: a failed read gives "", a failed query gives -1 or false.
// Pair `read` with `exists` when empty-vs-missing matters. Only the
// standard interpreter opens this.

import strings;

// The whole file as text. Empty if it cannot be read.
extern func read(path: str): str;

// Replaces the file. False if it could not be written.
extern func write(path: str, text: str): bool;
extern func append(path: str, text: str): bool;

extern func exists(path: str): bool;
extern func isDir(path: str): bool;

// Size in bytes, or -1 if there is nothing there.
extern func size(path: str): int;

extern func remove(path: str): bool;
extern func rename(from: str, to: str): bool;

// Creates one directory. The parent has to exist already.
extern func mkdir(path: str): bool;

// The names in a directory, without `.` and `..` and without the directory
// prefix. Empty if it is not a readable directory.
extern func list(path: str): [str];

func isFile(path: str): bool {
    return exists(path) && !isDir(path);
}

// The file split into lines. A trailing newline does not produce a final
// empty line, and \r\n is handled.
func lines(path: str): [str] {
    var text = strings.replace(read(path), "\r\n", "\n");
    if strings.endsWith(text, "\n") {
        text = strings.sub(text, 0, strings.len(text) - 1);
    }
    var out: [str] = [];
    if strings.isEmpty(text) {
        return out;
    }
    return strings.split(text, "\n");
}

// Full paths of the entries in `dir`, so the result can be fed straight back
// into `read` or `list`.
func paths(dir: str): [str] {
    var out: [str] = [];
    for name in list(dir) {
        push(out, dir + "/" + name);
    }
    return out;
}

// Writes `text` only if the file is absent. False if it was already there.
func writeNew(path: str, text: str): bool {
    if exists(path) {
        return false;
    }
    return write(path, text);
}

// Creates `path` and any missing parents. False if it could not be made;
// true if it exists as a directory afterwards, including when it already did.
func mkdirs(path: str): bool {
    if isDir(path) {
        return true;
    }
    var built = "";
    if strings.startsWith(path, "/") {
        built = "/";
    }
    for part in strings.split(strings.replace(path, "\\", "/"), "/") {
        if strings.isEmpty(part) {
            continue;
        }
        if strings.isEmpty(built) || built == "/" {
            built = built + part;
        } else {
            built = built + "/" + part;
        }
        if !isDir(built) && !mkdir(built) {
            return false;
        }
    }
    return isDir(path);
}

// Every file and directory below `dir`, as paths starting with `dir`,
// directories before their contents. Empty if `dir` is not a directory.
func walk(dir: str): [str] {
    var out: [str] = [];
    for name in list(dir) {
        let full = dir + "/" + name;
        push(out, full);
        if isDir(full) {
            for inner in walk(full) {
                push(out, inner);
            }
        }
    }
    return out;
}

// Copies a file's contents to `to`, replacing it. False if either end failed.
func copy(from: str, to: str): bool {
    if !exists(from) {
        return false;
    }
    return write(to, read(from));
}

// Deletes a file, or a directory and everything in it. False if anything
// could not be removed.
func removeAll(path: str): bool {
    if !exists(path) {
        return true;
    }
    if isDir(path) {
        for name in list(path) {
            if !removeAll(path + "/" + name) {
                return false;
            }
        }
    }
    return remove(path);
}
