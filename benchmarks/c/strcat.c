#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void){ char*s=strdup(""); long n=0; for(long i=0;i<300000;i++){ char*t=malloc(64); snprintf(t,64,"item %ld: x",i); n++; if(i%1000==0){free(s);s=t;} else free(t);} printf("%ld %s\n",n,s); }
