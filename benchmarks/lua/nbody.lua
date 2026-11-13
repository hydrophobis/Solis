local b={} for i=1,1000 do b[i]={x=1,y=2,vx=1.5,vy=0.5} end b[1]={x=1,y=1,vx=1.5,vy=0.5}
local function step(b,dt) local t=0 for i=1,#b do local c=b[i] c.x=c.x+c.vx*dt c.y=c.y+c.vy*dt
 if c.x>100 or c.x<0 then c.vx=-c.vx end if c.y>100 or c.y<0 then c.vy=-c.vy end t=t+c.x+c.y end return t end
local a=0 for f=1,5000 do a=a+step(b,0.016) end print(a)
