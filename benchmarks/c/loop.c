#include <stdio.h>
#include <stdint.h>
int main(void){ volatile int64_t lim=100000000; int64_t s=0; for(int64_t i=0;i<lim;i++) s+=i%7; printf("%lld\n",(long long)s); }
