local total=0 for i=0,39999 do local s="k"..i..",v"..(i*3)..",w"..(i%7) local parts={} for p in s:gmatch("[^,]+") do parts[#parts+1]=p end total=total+#parts+#table.concat(parts,"-") end print(total)
