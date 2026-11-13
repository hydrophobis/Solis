#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void){ long total=0; char s[128]; for(long i=0;i<40000;i++){ snprintf(s,sizeof s,"k%ld,v%ld,w%ld",i,i*3,i%7);
 int parts=1; size_t len=0; for(char*p=s;*p;p++){ if(*p==',')parts++; else len++; } len+=parts-1; total+=parts+len; } printf("%ld\n",total); }
