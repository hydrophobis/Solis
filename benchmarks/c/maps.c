#include <stdio.h>
#include <stdlib.h>
typedef struct{long k,v;int used;}E;
static E tab[8192];
static E*slot(long k){ unsigned long h=(unsigned long)k*0x9E3779B97F4A7C15ul>>51; while(tab[h&8191].used&&tab[h&8191].k!=k)h++; return &tab[h&8191]; }
int main(void){ for(long i=0;i<3000;i++){E*e=slot(i);e->used=1;e->k=i;e->v=i*2;} long s=0; for(int r=0;r<2000;r++)for(long i=0;i<3000;i++)s+=slot(i)->v; printf("%ld\n",s); }
