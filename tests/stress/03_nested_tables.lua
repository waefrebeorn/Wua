-- 03 deeply nested tables: build a 5000-deep linked list, walk it.
local head = {}
local cur = head
for i = 1, 5000 do cur.next = { i = i }; cur = cur.next end
local n, sum = 0, 0
cur = head
while cur.next do n = n + 1; cur = cur.next end
cur = head
while cur.next do sum = sum + (cur.next.i or 0); cur = cur.next end
print('nest', n, sum)
