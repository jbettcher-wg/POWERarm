-- Numbers: integer and float arithmetic and their formatting.
local function show(label, ...)
  local parts = {}
  for i = 1, select("#", ...) do parts[#parts + 1] = tostring((select(i, ...))) end
  print(label, table.concat(parts, " "))
end

show("int", 1, -1, 0, math.maxinteger, math.mininteger, math.maxinteger + 1 == math.mininteger)
show("idiv", 7 // 2, -7 // 2, 7 % -3, -7 % 3, 7.5 // 2, -7.5 % 2)
show("float", 0.1 + 0.2, 1 / 3, -1 / 3, 1e300 * 1e10, -1e300 * 1e10, 2^53, 2^63, 2^-1074)
show("fdiv", 1 / 0, -1 / 0, 0 / 0 ~= 0 / 0, math.huge, -math.huge)
-- The sign of a generated NaN is architecture-defined (AArch64: positive).
show("nan", 0 / 0, -(0 / 0), math.huge - math.huge, math.sqrt(-1), string.format("%f %g", 0 / 0, -(0 / 0)))
show("conv", math.tointeger(3.0), math.tointeger(3.5), 3 | 0, math.type(1), math.type(1.0), math.type("1"))
show("str2num", tonumber("0x10"), tonumber("  12  "), tonumber("1e3"), tonumber("0x1p4"), tonumber("z", 36), tonumber("7fffffffffffffff", 16))
show("bits", 0xF0 & 0x3C, 0xF0 | 0x0F, 0xF0 ~ 0xFF, ~0, 1 << 63, 1 << 64, -1 >> 1, -1 >> 63)
show("math", math.floor(-3.5), math.ceil(-3.5), math.abs(math.mininteger), math.fmod(-7, 3), math.fmod(7.5, -2))
show("math2", math.sqrt(2), math.exp(1), math.log(10), math.log(8, 2), math.sin(1), math.cos(1), math.atan(1, -1))
show("minmax", math.max(1, 2.5, -3), math.min(1, 2.5, -3), math.ult(1, -1))
show("cmp", 1 == 1.0, 2^63 == math.mininteger, math.maxinteger < 2^63, -0.0 == 0.0, 1 / -0.0)

local fmts = {"%d", "%5.2f", "%.14g", "%g", "%e", "%.3e", "%a", "%x", "%X", "%o", "%10.4f", "%-10.3g|"}
local vals = {0, 1, -1, 255, 3.14159265358979, -2.5e-8, 1e21, 123456789012}
for _, f in ipairs(fmts) do
  local row = {}
  for _, v in ipairs(vals) do
    local ok, s = pcall(string.format, f, v)
    row[#row + 1] = ok and s or "ERR"
  end
  print(f, table.concat(row, " "))
end
print(string.format("%.17g %.17g %.17g", 0.1, 1 / 3, 2 / 3))
print(string.format("%q %q %q", 1 / 3, math.mininteger, 2^63))

-- Deterministic PRNG (xoshiro256**) from a fixed seed.
math.randomseed(42)
local r = {}
for i = 1, 6 do r[i] = math.random(1, 1000) end
print("random", table.concat(r, " "), string.format("%.10f", math.random()))

-- Accumulated float error, exercising many FP operations.
local s, p = 0.0, 1.0
for i = 1, 100000 do
  s = s + 1 / i
  p = p * (1 + 1 / (i * i))
end
print(string.format("harmonic %.15g product %.15g", s, p))
local acc = 0
for i = 1, 1000 do acc = (acc * 31 + i * i) % 1000000007 end
print("intmix", acc)
