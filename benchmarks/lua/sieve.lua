local limit=10000000 local f={} for i=1,limit do f[i]=true end
local c=0 for n=2,limit-1 do if f[n+1] then c=c+1 for m=n+n,limit-1,n do f[m+1]=false end end end print(c)
