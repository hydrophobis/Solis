// std/option.sl and std/result.sl, used across the module boundary.

import option;
import result;

func main() {
    let found: option.Option[int] = option.Option.Some(5);
    switch found {
        case option.Option.Some(value):
            print("found ${value}");

        case option.Option.None:
            print("not found");
    }

    let parsed: result.Result[int, str] = result.Result.Failure("bad input");
    switch parsed {
        case result.Result.Success(value):
            print("ok ${value}");

        case result.Result.Failure(error):
            print("err ${error}");
    }
}
