#include <stdio.h>
#include <stdlib.h>
static void qsort_(long *a, long lo, long hi){ if(lo>=hi)return; long pivot=a[(lo+hi)/2]; long i=lo,j=hi;
 while(i<=j){ while(a[i]<pivot)i++; while(a[j]>pivot)j--; if(i<=j){long t=a[i];a[i]=a[j];a[j]=t;i++;j--;} }
 qsort_(a,lo,j); qsort_(a,i,hi); }
int main(void){ long cap=1,n=1; long*a=malloc(8); a[0]=0; long seed=12345;
 for(long k=1;k<1000000;k++){ seed=(seed*1103515245+12345)%2147483648L; if(n==cap){cap*=2;a=realloc(a,cap*8);} a[n++]=seed; }
 qsort_(a,0,n-1); long ok=0; for(long k=1;k<n;k++) if(a[k-1]<=a[k]) ok++; printf("%ld %ld %ld\n",ok,a[0],a[n-1]); }
