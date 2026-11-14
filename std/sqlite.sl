// The `sqlite` module: SQLite databases.
//
//     let db = sqlite.open("app.db");
//     sqlite.exec(db, "create table if not exists users (id integer primary key, name text)", []);
//     sqlite.exec(db, "insert into users (name) values (?)", ["Ada"]);
//     for row in sqlite.records(db, "select id, name from users where name = ?", ["Ada"]) {
//         print("${row["id"]} ${row["name"]}");
//     }
//     sqlite.close(db);
//
// The SQLite library is loaded from the system when first needed (libsqlite3
// on Linux and macOS, sqlite3.dll on Windows), so Solis itself has no
// dependency on it; `available()` tells whether it was found. Open a file
// path, or ":memory:" for a database that lives as long as the handle.
//
// Parameters are passed as text in the `?` slots and SQLite converts them by
// the column's type, so `["42"]` fills an integer column. Results come back
// as text too: numbers print the way SQLite prints them, and NULL is "".
// Use `strings.toInt` and `toFloat` on what you read. A failed call returns
// -1 (or an empty result) and `error()` gives the message.
//
// Only the standard interpreter opens this.

extern func available(): bool;

// The SQLite library's version, or "" if it is not available.
extern func version(): str;

// A handle to use with the other functions, or -1 (see `error`).
extern func open(path: str): int;
extern func close(db: int);

// Runs one or more statements. Parameters apply to the first. Returns the
// number of rows changed, or -1 on an error.
extern func exec(db: int, sql: str, params: [str]): int;

// Runs the first statement and returns its rows as text. The first row holds
// the column names. An error gives an empty array.
extern func query(db: int, sql: str, params: [str]): [[str]];

// The rowid of the last insert.
extern func lastInsertId(db: int): int;

extern func error(): str;

// Each result row as a map from column name to value.
func records(db: int, sql: str, params: [str]): [{str: str}] {
    let rows = query(db, sql, params);
    var out: [{str: str}] = [];
    if len(rows) == 0 {
        return out;
    }
    let names = rows[0];
    var r = 1;
    while r < len(rows) {
        var rec: {str: str} = {};
        var c = 0;
        while c < len(names) && c < len(rows[r]) {
            rec[names[c]] = rows[r][c];
            c += 1;
        }
        push(out, rec);
        r += 1;
    }
    return out;
}

// The first cell of the first row, or `fallback` if there is none.
func value(db: int, sql: str, params: [str], fallback: str): str {
    let rows = query(db, sql, params);
    if len(rows) < 2 || len(rows[1]) == 0 {
        return fallback;
    }
    return rows[1][0];
}

// Runs `sql` with no parameters.
func run(db: int, sql: str): int {
    var none: [str] = [];
    return exec(db, sql, none);
}
