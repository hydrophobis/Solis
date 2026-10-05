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
