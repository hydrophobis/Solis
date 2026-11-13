#include <stdio.h>
typedef struct{long x,y;}P;
__attribute__((noinline)) P mk(long i){return (P){i,i+1};}
int main(void){ long s=0; for(long i=0;i<5000000;i++){P p=mk(i); s+=p.x+p.y;} printf("%ld\n",s); }
