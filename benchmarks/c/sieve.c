#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void){ const long limit=10000000; char*f=malloc(limit); memset(f,1,limit);
 long c=0; for(long n=2;n<limit;n++) if(f[n]){c++; for(long m=n+n;m<limit;m+=n) f[m]=0;} printf("%ld\n",c); }
