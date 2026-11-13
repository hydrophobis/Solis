local s="" local n=0 for i=0,299999 do local t="item "..i..": ".."x" n=n+1 if i%1000==0 then s=t end end print(n,s)
