#include <stdio.h>
#include <stdlib.h>
int main(void){ int n=200; double **a=malloc(n*sizeof*a),**b=malloc(n*sizeof*b);
 for(int i=0;i<n;i++){a[i]=malloc(n*8);b[i]=malloc(n*8);for(int j=0;j<n;j++){a[i][j]=1.0*i+j;b[i][j]=1.0*j-i;}}
 double sum=0; for(int i=0;i<n;i++)for(int j=0;j<n;j++){double s=0;for(int k=0;k<n;k++)s+=a[i][k]*b[k][j];sum+=s;} printf("%g\n",sum); }
