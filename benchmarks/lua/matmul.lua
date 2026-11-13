local n=200 local a,b={},{}
for i=0,n-1 do a[i]={} b[i]={} for j=0,n-1 do a[i][j]=1.0*i+j b[i][j]=1.0*j-i end end
local sum=0.0 for i=0,n-1 do for j=0,n-1 do local s=0.0 for k=0,n-1 do s=s+a[i][k]*b[k][j] end sum=sum+s end end print(sum)
