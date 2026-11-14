// The `datetime` module: dates and times in UTC, as plain numbers and text.
//
//     let t = datetime.fromUnix(1700000000);
//     datetime.format(t, "%Y-%m-%d %H:%M")       // "2023-11-14 22:13"
//     datetime.toUnix(datetime.make(2024, 2, 29, 12, 0, 0))
//
// This module is pure: it never reads the clock. Get the current time with
// `time.nowSec()` and pass it in. There are no time zones; everything is UTC.
// The calendar is the proleptic Gregorian one, so negative Unix times and
// years before 1970 work.

import result;
import strings;

struct DateTime {
    year: int;
    month: int;        // 1 to 12
    day: int;          // 1 to 31
    hour: int;
    minute: int;
    second: int;
    weekday: int;      // 0 is Sunday
}

// Division and remainder that round down, so negative times split properly.
func floorDiv(a: int, b: int): int {
    var q = a / b;
    if a % b != 0 && ((a < 0) != (b < 0)) {
        q -= 1;
    }
    return q;
}

func floorMod(a: int, b: int): int {
    return a - floorDiv(a, b) * b;
}

func isLeap(year: int): bool {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

func daysInMonth(year: int, month: int): int {
    if month == 2 {
        if isLeap(year) {
            return 29;
        }
        return 28;
    }
    if month == 4 || month == 6 || month == 9 || month == 11 {
        return 30;
    }
    return 31;
}

// Days since 1970-01-01 for a calendar date (Howard Hinnant's algorithm).
func daysFromCivil(year: int, month: int, day: int): int {
    var y = year;
    if month <= 2 {
        y -= 1;
    }
    let era = floorDiv(y, 400);
    let yoe = y - era * 400;
    var mp = month - 3;
    if month <= 2 {
        mp = month + 9;
    }
    let doy = (153 * mp + 2) / 5 + day - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// The time `t` seconds after 1970-01-01 00:00:00 UTC.
func fromUnix(t: int): DateTime {
    let days = floorDiv(t, 86400);
    let secs = floorMod(t, 86400);
    let z = days + 719468;
    let era = floorDiv(z, 146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    var month = mp + 3;
    if mp >= 10 {
        month = mp - 9;
    }
    var year = yoe + era * 400;
    if month <= 2 {
        year += 1;
    }
    return DateTime {
        year: year, month: month, day: day,
        hour: secs / 3600, minute: (secs % 3600) / 60, second: secs % 60,
        weekday: floorMod(days + 4, 7),
    };
}

func toUnix(d: DateTime): int {
    return daysFromCivil(d.year, d.month, d.day) * 86400 + d.hour * 3600 + d.minute * 60 + d.second;
}

// A date and time, with out-of-range parts carried over: month 13 is
// January of the next year, second 75 is a minute and fifteen seconds.
func make(year: int, month: int, day: int, hour: int, minute: int, second: int): DateTime {
    let y = year + floorDiv(month - 1, 12);
    let m = floorMod(month - 1, 12) + 1;
    return fromUnix(daysFromCivil(y, m, 1) * 86400 + (day - 1) * 86400 + hour * 3600 + minute * 60 + second);
}

func addSeconds(d: DateTime, n: int): DateTime {
    return fromUnix(toUnix(d) + n);
}

func addDays(d: DateTime, n: int): DateTime {
    return fromUnix(toUnix(d) + n * 86400);
}

// Months are calendar months; the day is clamped to the end of a shorter one
// (January 31st plus one month is February 28th or 29th).
func addMonths(d: DateTime, n: int): DateTime {
    let total = d.year * 12 + (d.month - 1) + n;
    let y = floorDiv(total, 12);
    let m = floorMod(total, 12) + 1;
    var day = d.day;
    let last = daysInMonth(y, m);
    if day > last {
        day = last;
    }
    return make(y, m, day, d.hour, d.minute, d.second);
}

// 1 for January 1st.
func dayOfYear(d: DateTime): int {
    return daysFromCivil(d.year, d.month, d.day) - daysFromCivil(d.year, 1, 1) + 1;
}

// Whole seconds from `a` to `b`.
func diffSeconds(a: DateTime, b: DateTime): int {
    return toUnix(b) - toUnix(a);
}

func monthName(month: int): str {
    let names = ["January", "February", "March", "April", "May", "June", "July",
                 "August", "September", "October", "November", "December"];
    if month < 1 || month > 12 {
        return "";
    }
    return names[month - 1];
}

func dayName(weekday: int): str {
    let names = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];
    if weekday < 0 || weekday > 6 {
        return "";
    }
    return names[weekday];
}

func pad(n: int, width: int): str {
    return strings.padLeft(strings.fromInt(n), width, "0");
}

// Formats with these codes, as in C's strftime:
//   %Y year  %y two-digit year  %m month  %d day  %e day, space padded
//   %H hour (24)  %I hour (12)  %M minute  %S second  %p AM or PM
//   %B month name  %b short month  %A day name  %a short day  %j day of year
//   %F is %Y-%m-%d  %T is %H:%M:%S  %s Unix seconds  %Z is UTC  %% is %
func format(d: DateTime, fmt: str): str {
    var out: [str] = [];
    let cs = strings.chars(fmt);
    var i = 0;
    while i < len(cs) {
        let c = cs[i];
        if c != "%" || i + 1 >= len(cs) {
            push(out, c);
            i += 1;
            continue;
        }
        let f = cs[i + 1];
        i += 2;
        if f == "Y" {
            push(out, strings.fromInt(d.year));
        } else if f == "y" {
            push(out, pad(floorMod(d.year, 100), 2));
        } else if f == "m" {
            push(out, pad(d.month, 2));
        } else if f == "d" {
            push(out, pad(d.day, 2));
        } else if f == "e" {
            push(out, strings.padLeft(strings.fromInt(d.day), 2, " "));
        } else if f == "H" {
            push(out, pad(d.hour, 2));
        } else if f == "I" {
            var h = d.hour % 12;
            if h == 0 {
                h = 12;
            }
            push(out, pad(h, 2));
        } else if f == "M" {
            push(out, pad(d.minute, 2));
        } else if f == "S" {
            push(out, pad(d.second, 2));
        } else if f == "p" {
            if d.hour < 12 {
                push(out, "AM");
            } else {
                push(out, "PM");
            }
        } else if f == "B" {
            push(out, monthName(d.month));
        } else if f == "b" {
            push(out, strings.sub(monthName(d.month), 0, 3));
        } else if f == "A" {
            push(out, dayName(d.weekday));
        } else if f == "a" {
            push(out, strings.sub(dayName(d.weekday), 0, 3));
        } else if f == "j" {
            push(out, pad(dayOfYear(d), 3));
        } else if f == "F" {
            push(out, format(d, "%Y-%m-%d"));
        } else if f == "T" {
            push(out, format(d, "%H:%M:%S"));
        } else if f == "s" {
            push(out, strings.fromInt(toUnix(d)));
        } else if f == "Z" {
            push(out, "UTC");
        } else if f == "%" {
            push(out, "%");
        } else {
            push(out, "%" + f);
        }
    }
    return strings.join(out, "");
}

// "2023-11-14T22:13:20Z".
func iso(d: DateTime): str {
    return format(d, "%Y-%m-%dT%H:%M:%SZ");
}

// A run of digits `n` characters long at `at`, or -1.
func digits(s: str, at: int, n: int): int {
    if at + n > strings.len(s) {
        return -1;
    }
    let piece = strings.sub(s, at, at + n);
    var i = 0;
    while i < n {
        if !strings.contains("0123456789", strings.at(piece, i)) {
            return -1;
        }
        i += 1;
    }
    return strings.toInt(piece);
}

// Reads `YYYY-MM-DD`, optionally followed by `T` or a space and `HH:MM` or
// `HH:MM:SS`, optional fractional seconds (dropped), and `Z` or an offset
// like `+02:00` (applied, so the result is UTC).
func parse(text: str): result.Result[DateTime, str] {
    let s = strings.trim(text);
    let bad: result.Result[DateTime, str] = result.Result.Failure("not a date: " + text);
    let year = digits(s, 0, 4);
    let month = digits(s, 5, 2);
    let day = digits(s, 8, 2);
    if year < 0 || month < 1 || month > 12 || day < 1 || strings.at(s, 4) != "-" || strings.at(s, 7) != "-" {
        return bad;
    }
    if day > daysInMonth(year, month) {
        return bad;
    }
    var hour = 0;
    var minute = 0;
    var second = 0;
    var offset = 0;
    var pos = 10;
    if strings.len(s) > 10 {
        let sep = strings.at(s, 10);
        if sep != "T" && sep != " " {
            return bad;
        }
        hour = digits(s, 11, 2);
        minute = digits(s, 14, 2);
        if hour < 0 || hour > 23 || minute < 0 || minute > 59 || strings.at(s, 13) != ":" {
            return bad;
        }
        pos = 16;
        if strings.at(s, 16) == ":" {
            second = digits(s, 17, 2);
            if second < 0 || second > 60 {
                return bad;
            }
            pos = 19;
            if strings.at(s, 19) == "." {
                pos += 1;
                while pos < strings.len(s) && strings.contains("0123456789", strings.at(s, pos)) {
                    pos += 1;
                }
            }
        }
        if pos < strings.len(s) {
            let z = strings.at(s, pos);
            if z == "Z" || z == "z" {
                pos += 1;
            } else if z == "+" || z == "-" {
                let oh = digits(s, pos + 1, 2);
                var om = 0;
                var used = 3;
                if strings.at(s, pos + 3) == ":" {
                    om = digits(s, pos + 4, 2);
                    used = 6;
                } else if digits(s, pos + 3, 2) >= 0 {
                    om = digits(s, pos + 3, 2);
                    used = 5;
                }
                if oh < 0 || om < 0 {
                    return bad;
                }
                offset = oh * 3600 + om * 60;
                if z == "-" {
                    offset = 0 - offset;
                }
                pos += used;
            }
        }
        if pos != strings.len(s) {
            return bad;
        }
    }
    let ok: result.Result[DateTime, str] =
        result.Result.Success(fromUnix(toUnix(make(year, month, day, hour, minute, second)) - offset));
    return ok;
}
