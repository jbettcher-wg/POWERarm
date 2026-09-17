-- Errors, pcall/xpcall, error objects and tracebacks (C longjmp underneath).
print(pcall(error, "plain"))
print(pcall(error, "with level", 0))
print(select(2, pcall(error, {code = 42})).code)
print(select(2, pcall(error, setmetatable({}, {__tostring = function() return "custom error" end}))) ~= nil)
print(pcall(function() local x = nil; return x.field end))
print(pcall(function() return 1 + {} end))
print(pcall(function() return #5 end))
print(pcall(function() return 1 // 0 end))
print(pcall(function() return 1 % 0 end))
print(pcall(function() return math.tointeger(2^63) or error("no integer") end))
print(pcall(string.rep))
print(pcall(string.format, "%d", 1.5))
print(pcall(setmetatable, 1, {}))
print(pcall(function() for i = 1, 10, 0 do end end))
print(pcall(function() return ("x"):bad() end))
print(pcall(function() error() end))
print(pcall(function() local t = setmetatable({}, {__index = function() error("deep") end}); return t.k end))

local function handler(msg)
  return "handled: " .. tostring(msg)
end
print(xpcall(function() error("in xpcall") end, handler))
print(xpcall(function(a, b) return a + b end, handler, 2, 3))
print(xpcall(function() local y = undefined_global.x end, debug.traceback))

-- Nested pcall across many frames, and error rethrown through coroutines.
local function recurse(n)
  if n == 0 then error("bottom") end
  local ok, err = pcall(recurse, n - 1)
  error(err .. "<" .. n, 0)
end
print(pcall(recurse, 5))
local function overflow() return 1 + overflow() end
local ok, err = pcall(overflow)
print(ok, (tostring(err):gsub("^.-:%d+: ", "")))
local co = coroutine.wrap(function() error({tag = "from coroutine"}) end)
local ok2, e2 = pcall(co)
print(ok2, type(e2), e2.tag)
print(pcall(coroutine.wrap(function() coroutine.yield(1) end)))

-- close variables run on error.
do
  local closed = {}
  local ok3, e3 = pcall(function()
    local a <close> = setmetatable({}, {__close = function(_, e) closed[#closed + 1] = "a:" .. tostring(e) end})
    local b <close> = setmetatable({}, {__close = function() closed[#closed + 1] = "b" end})
    error("closing", 0)
  end)
  print(ok3, e3, table.concat(closed, " "))
end

-- load with syntax and runtime errors.
print(load("return +"))
print(load("x ="))
local f = load("local a, b = ...; return a * b, 'chunk'")
print(f(6, 7))
print(pcall(load("error('from chunk')", "=mychunk")))
print(string.format("%s", select(2, load("return 1 +* 2", "=expr"))))

-- os and io that don't depend on the clock or the host.
print(os.time({year = 2000, month = 1, day = 1, hour = 12}), os.date("!%Y-%m-%d %H:%M:%S", 86400 * 365))
print(os.getenv("LC_ALL"), os.getenv("TZ"), os.getenv("NOT_SET_ANYWHERE"))
local path = "io-test.txt"
local fh = assert(io.open(path, "w"))
fh:write("line one\n", 42, " ", 3.5, "\n", "last")
fh:close()
for l in io.lines(path) do io.write("[", l, "]") end
print()
fh = assert(io.open(path, "r"))
print(fh:read("l", "n", "n", "a"))
fh:close()
print(io.open("does/not/exist.txt"))
print(os.remove(path), os.remove(path))
