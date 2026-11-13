local function qsort(a, lo, hi)
  if lo >= hi then return end
  local pivot = a[(lo + hi) // 2 + 1]
  local i, j = lo, hi
  while i <= j do
    while a[i+1] < pivot do i = i + 1 end
    while a[j+1] > pivot do j = j - 1 end
    if i <= j then a[i+1], a[j+1] = a[j+1], a[i+1]; i = i + 1; j = j - 1 end
  end
  qsort(a, lo, j); qsort(a, i, hi)
end
local a = {0}
local seed = 12345
for k = 1, 999999 do seed = (seed * 1103515245 + 12345) % 2147483648; a[#a+1] = seed end
qsort(a, 0, #a - 1)
local ok = 0
for k = 2, #a do if a[k-1] <= a[k] then ok = ok + 1 end end
print(ok, a[1], a[#a])
