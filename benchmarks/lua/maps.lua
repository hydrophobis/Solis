local m={} for i=0,2999 do m[i]=i*2 end local s=0 for r=1,2000 do for i=0,2999 do s=s+m[i] end end print(s)
