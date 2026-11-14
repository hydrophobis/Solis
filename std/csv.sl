// The `csv` module: comma-separated values, as in RFC 4180.
//
//     let rows = csv.parse("name,age\n\"Lee, Ann\",31\n");
//     // [["name", "age"], ["Lee, Ann", "31"]]
//
// A field may be wrapped in double quotes to hold the separator, a newline or
// a quote (written twice). Lines end with \n or \r\n. A final newline does
// not make an extra empty row. Every cell is text; convert with
// `strings.toInt` and friends.

import strings;

func noCells(): [str] {
    var cells: [str] = [];
    return cells;
}

// Rows of cells, split on `sep` (one character).
func parseWith(text: str, sep: str): [[str]] {
    var rows: [[str]] = [];
    var row: [str] = [];
    var cell = "";
    var inQuotes = false;
    var pending = false;       // anything seen on this row yet
    let cs = strings.chars(text);
    let n = len(cs);
    var i = 0;
    while i < n {
        let c = cs[i];
        if inQuotes {
            if c == "\"" {
                if i + 1 < n && cs[i + 1] == "\"" {
                    cell = cell + "\"";
                    i += 1;
                } else {
                    inQuotes = false;
                }
            } else {
                cell = cell + c;
            }
        } else if c == "\"" && strings.isEmpty(cell) {
            inQuotes = true;
            pending = true;
        } else if c == sep {
            push(row, cell);
            cell = "";
            pending = true;
        } else if c == "\n" || c == "\r" {
            if c == "\r" && i + 1 < n && cs[i + 1] == "\n" {
                i += 1;
            }
            if pending || !strings.isEmpty(cell) || len(row) > 0 {
                push(row, cell);
                push(rows, row);
            }
            row = noCells();
            cell = "";
            pending = false;
        } else {
            cell = cell + c;
            pending = true;
        }
        i += 1;
    }
    if pending || !strings.isEmpty(cell) || len(row) > 0 {
        push(row, cell);
        push(rows, row);
    }
    return rows;
}

func parse(text: str): [[str]] {
    return parseWith(text, ",");
}

// The first row names the columns; each later row becomes a map from column
// name to cell. Short rows leave the missing columns out.
func records(text: str): [{str: str}] {
    let rows = parse(text);
    var out: [{str: str}] = [];
    if len(rows) == 0 {
        return out;
    }
    let header = rows[0];
    var r = 1;
    while r < len(rows) {
        var rec: {str: str} = {};
        var c = 0;
        while c < len(header) && c < len(rows[r]) {
            rec[header[c]] = rows[r][c];
            c += 1;
        }
        push(out, rec);
        r += 1;
    }
    return out;
}

// One cell, quoted only if it has to be.
func quote(cell: str, sep: str): str {
    if strings.contains(cell, sep) || strings.contains(cell, "\"") || strings.contains(cell, "\n") ||
       strings.contains(cell, "\r") || strings.startsWith(cell, " ") || strings.endsWith(cell, " ") {
        return "\"" + strings.replace(cell, "\"", "\"\"") + "\"";
    }
    return cell;
}

func formatWith(rows: [[str]], sep: str): str {
    var lines: [str] = [];
    for row in rows {
        var cells: [str] = [];
        for cell in row {
            push(cells, quote(cell, sep));
        }
        push(lines, strings.join(cells, sep));
    }
    if len(lines) == 0 {
        return "";
    }
    return strings.join(lines, "\n") + "\n";
}

// Text with one line per row, each ending in \n.
func format(rows: [[str]]): str {
    return formatWith(rows, ",");
}
