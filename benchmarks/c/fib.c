#include <stdio.h>
#include <stdint.h>
__attribute__((noinline)) int64_t fib(int64_t n){ return n<2?n:fib(n-1)+fib(n-2); }
int main(void){ printf("%lld\n",(long long)fib(32)); }
