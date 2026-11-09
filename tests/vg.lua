local i=0; local s=0
::top:: i=i+1; if i>5 then goto done end; s=s+i; goto top
::done:: print('sum', s)
for k=1,4 do if k==2 then break end; print('k',k) end
local j=0; repeat j=j+1 until j>=3; print('repeat', j)
