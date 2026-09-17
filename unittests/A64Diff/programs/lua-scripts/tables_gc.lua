-- Tables, metatables, closures, coroutines and the garbage collector.
local t = {}
for i = 1, 1000 do t[i] = (i * 7919) % 1009 end
table.sort(t)
print(#t, t[1], t[500], t[1000], table.concat(t, ",", 1, 10))
table.insert(t, 1, -1)
table.remove(t, 2)
print(#t, t[1], t[2], select("#", table.unpack(t, 1, 5)), table.unpack(t, 1, 5))
print(table.concat(table.move({1, 2, 3, 4, 5}, 2, 4, 1), " "))

-- Keys are sorted before printing: pairs order depends on a per-run seed.
local d = {apple = 3, banana = 1, cherry = 2, [10] = "ten", [2.5] = "float", [true] = "bool"}
local keys = {}
for k in pairs(d) do keys[#keys + 1] = tostring(k) end
table.sort(keys)
print(table.concat(keys, " "))

local Vec = {}
Vec.__index = Vec
Vec.__add = function(a, b) return setmetatable({x = a.x + b.x, y = a.y + b.y}, Vec) end
Vec.__eq = function(a, b) return a.x == b.x and a.y == b.y end
Vec.__tostring = function(v) return string.format("(%g, %g)", v.x, v.y) end
Vec.__len = function() return 2 end
Vec.__call = function(v, k) return v.x * k end
local function vec(x, y) return setmetatable({x = x, y = y}, Vec) end
local v = vec(1, 2) + vec(3.5, -4)
print(tostring(v), #v, v(10), vec(1, 1) == vec(1, 1), rawequal(vec(1, 1), vec(1, 1)))

local proxy = setmetatable({}, {__index = function(_, k) return k .. "!" end, __newindex = function(t2, k, val) rawset(t2, k, val * 2) end})
proxy.a = 21
print(proxy.a, proxy.zzz, rawget(proxy, "zzz"))

local function counter()
  local n = 0
  return function() n = n + 1; return n end
end
local c1, c2 = counter(), counter()
c1(); c1()
print(c1(), c2())

local gen = coroutine.wrap(function(a, b)
  for i = a, b do coroutine.yield(i * i) end
  return "done"
end)
print(gen(3, 6), gen(), gen(), gen(), gen())
local co = coroutine.create(function(x) local y = coroutine.yield(x + 1); error("inner " .. y) end)
print(coroutine.resume(co, 1))
print(coroutine.resume(co, "boom"))
print(coroutine.status(co), coroutine.resume(co))

-- Finalizers run in reverse order of marking; weak tables drop collected keys.
local log = {}
do
  for i = 1, 5 do setmetatable({}, {__gc = function() log[#log + 1] = i end}) end
end
collectgarbage()
collectgarbage()
print("finalized", #log)
local weak = setmetatable({}, {__mode = "k"})
do
  for i = 1, 100 do weak[{}] = i end
end
local strong = {}
weak[strong] = "kept"
collectgarbage()
local n = 0
for _ in pairs(weak) do n = n + 1 end
print("weak entries", n, weak[strong])

local before = collectgarbage("count")
local junk = {}
for i = 1, 200000 do junk[i] = {i, tostring(i)} end
local during = collectgarbage("count")
junk = nil
collectgarbage()
local after = collectgarbage("count")
print("gc grew", during > before, "gc shrank", after < during)
print(collectgarbage("isrunning"), collectgarbage("incremental"), collectgarbage("generational"), collectgarbage("incremental"))

-- A deep recursive structure and a sum over it.
local function tree(depth)
  if depth == 0 then return {v = 1} end
  return {l = tree(depth - 1), r = tree(depth - 1), v = depth}
end
local function sum(nd) return nd.v + (nd.l and sum(nd.l) or 0) + (nd.r and sum(nd.r) or 0) end
print("tree", sum(tree(16)))
