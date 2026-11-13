local a={0} for i=1,1999999 do a[#a+1]=i end local s=0 for _,x in ipairs(a) do s=s+x end
for r=1,5 do for i=1,#a do a[i]=a[i]+1 end end print(s,a[6])
