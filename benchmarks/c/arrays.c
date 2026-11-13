#include <stdio.h>
#include <stdlib.h>
int main(void){ long cap=1,n=1; long*a=malloc(8); a[0]=0; for(long i=1;i<2000000;i++){ if(n==cap){cap*=2;a=realloc(a,cap*8);} a[n++]=i; }
 long s=0; for(long i=0;i<n;i++)s+=a[i]; for(int r=0;r<5;r++)for(long i=0;i<n;i++)a[i]=a[i]+1; printf("%ld %ld\n",s,a[5]); }
