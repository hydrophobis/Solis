struct Stack[T] {
    items: [T];

    func add(item: T) {
        push(this.items, item);
    }

    func isEmpty(): bool {
        return len(this.items) == 0;
    }
}

enum Option[T] {
    Some(value: T),
    None,
}

enum Result[Ok, Err] {
    Success(value: Ok),
    Failure(error: Err),
}

interface Comparable {
    func compareTo(other: Score): int;
}

struct Score : Comparable {
    value: int;

    func compareTo(other: Score): int {
        return this.value - other.value;
    }
}

struct SortedList[T: Comparable] {
    items: [T];

    func add(item: T) {
        push(this.items, item);
    }

    func contains(item: T): bool {
        for it in this.items {
            if it.compareTo(item) == 0 {
                return true;
            }
        }
        return false;
    }
}

interface Container[T] {
    func add(item: T);
    func contains(item: T): bool;
}

struct Bag[T] : Container[T] {
    items: [T];

    func add(item: T) {
        push(this.items, item);
    }

    func contains(item: T): bool {
        for it in this.items {
            if it == item {
                return true;
            }
        }
        return false;
    }
}

func main() {
    var ints: Stack[int] = Stack { items: [] };
    ints.add(1);
    ints.add(2);
    print("stack empty = ${ints.isEmpty()}");

    let found: Option[str] = Option.Some("hi");
    switch found {
        case Option.Some(value):
            print("got ${value}");

        case Option.None:
            print("nothing");
    }

    let parsed: Result[int, str] = Result.Failure("bad input");
    switch parsed {
        case Result.Success(value):
            print("ok: ${value}");

        case Result.Failure(error):
            print("err: ${error}");
    }

    var sorted: SortedList[Score] = SortedList { items: [] };
    sorted.add(Score { value: 5 });
    sorted.add(Score { value: 1 });
    print("has score 1 = ${sorted.contains(Score { value: 1 })}");

    var bag: Bag[int] = Bag { items: [] };
    bag.add(5);
    bag.add(9);
    print("bag has 9 = ${bag.contains(9)}");
}
