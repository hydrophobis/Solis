// The `io` module: standard input, output and error.
//
// `print` covers most output; this is for when the newline, the stream or
// the input matters. Only the standard interpreter opens it.

// Writes with no trailing newline.
extern func write(s: str);
extern func writeErr(s: str);

// Standard output is line buffered when it is a terminal and block buffered
// otherwise, so this matters when output is being piped somewhere watching.
extern func flush();

// One line from standard input, without its newline. Empty at end of input,
// which is also what an empty line gives; check `eof` to tell them apart.
extern func readLine(): str;

// Everything left on standard input.
extern func readAll(): str;

extern func eof(): bool;

func writeLine(s: str) {
    write(s);
    write("\n");
}

func errLine(s: str) {
    writeErr(s);
    writeErr("\n");
}

// Every remaining line of standard input. The last line counts whether or
// not the input ended with a newline.
func lines(): [str] {
    var out: [str] = [];
    while !eof() {
        let line = readLine();
        if eof() && len(line) == 0 {
            return out;
        }
        push(out, line);
    }
    return out;
}

// Prints `prompt` and reads one line, for the usual interactive case.
func ask(prompt: str): str {
    write(prompt);
    flush();
    return readLine();
}
