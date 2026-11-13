local function add(a,o) return {x=a.x+o.x,y=a.y+o.y} end
local function scale(a,k) return {x=a.x*k,y=a.y*k} end
local function dot(a,o) return a.x*o.x+a.y*o.y end
local p={x=0.0,y=0.0} local v={x=1.0,y=0.5} local acc=0.0
for i=1,3000000 do p=add(p,scale(v,0.001)) acc=acc+dot(p,v) end
print(p.x,p.y,acc)
