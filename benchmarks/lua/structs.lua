local function mk(i) return {x=i,y=i+1} end local s=0 for i=0,4999999 do local p=mk(i) s=s+p.x+p.y end print(s)
