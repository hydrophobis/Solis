// Same limitation as option.sl: no convenience functions, use `switch`.

enum Result[Ok, Err] {
    Success(value: Ok),
    Failure(error: Err),
}
