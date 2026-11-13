#include <stdio.h>
#include <stdlib.h>
typedef struct{double x,y,vx,vy;}Body;
static double step(Body*b,long n,double dt){ double t=0; for(long i=0;i<n;i++){ Body c=b[i]; c.x+=c.vx*dt; c.y+=c.vy*dt;
 if(c.x>100||c.x<0)c.vx=0-c.vx; if(c.y>100||c.y<0)c.vy=0-c.vy; b[i]=c; t+=c.x+c.y;} return t; }
int main(void){ Body*b=malloc(1000*sizeof *b); for(int i=0;i<1000;i++)b[i]=(Body){1,2,1.5,.5}; b[0]=(Body){1,1,1.5,.5};
 double a=0; for(int f=0;f<5000;f++)a+=step(b,1000,0.016); printf("%g\n",a); }
