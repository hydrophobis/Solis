#include <stdio.h>
typedef struct{double x,y;}V2;
static V2 add(V2 a,V2 o){return (V2){a.x+o.x,a.y+o.y};}
static V2 scale(V2 a,double k){return (V2){a.x*k,a.y*k};}
static double dot(V2 a,V2 o){return a.x*o.x+a.y*o.y;}
int main(void){ V2 p={0,0}; V2 v={1.0,0.5}; double acc=0; for(long i=0;i<3000000;i++){ p=add(p,scale(v,0.001)); acc+=dot(p,v);} printf("%g %g %g\n",p.x,p.y,acc); }
