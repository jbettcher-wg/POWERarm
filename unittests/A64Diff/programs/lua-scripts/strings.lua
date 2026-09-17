-- Strings: patterns, formatting, packing and UTF-8.
local s = "The quick brown fox jumps over the lazy dog"
print(#s, s:upper(), s:lower():reverse())
print(s:find("fox"), s:find("o", 20, true), s:match("(%a+) dog"), s:sub(-8, -5))
print((s:gsub("%w+", function(w) return #w end)))
print((s:gsub("(%w)(%w*)", "%2%1ay")))
local words = {}
for w in s:gmatch("%a+") do words[#words + 1] = w end
table.sort(words, function(a, b) return a:lower() < b:lower() end)
print(table.concat(words, ","))
print(("x"):rep(5, "-"), ("ab"):rep(0), ("%d items"):format(3))
print(string.format("%q", "line1\nline2\0tab\t\"quote\"\\"))
print(string.format("[%10s][%-10s][%.3s]", "right", "left", "truncate"))
print(string.byte("ABC", 1, -1), string.char(72, 105))
print(("key = value"):match("^(%w+)%s*=%s*(%w+)$"))
print(("2026-09-16"):match("(%d+)-(%d+)-(%d+)"))
print(("  trim me  "):match("^%s*(.-)%s*$") .. "|")
print(("a,b,,c"):gsub(",", ";"))
print(("hello world"):find("o w"), ("hello"):find("l+"))
print(("%bxy"):len(), ("f(a(b)c)d"):match("%b()"))
print(("THE (quick) fox"):find("%((%a+)%)"))
print(tostring(nil), tostring(true), tostring(12), tostring(12.0), tostring(-0.0))
print("10" + 5, "3.5" * 2, 10 .. 20, "0x10" + 0)

local packed = string.pack("<i4 >i8 d s1 z", -2, 0x0102030405060708, 1.5, "abc", "zero")
print(#packed, (packed:gsub(".", function(c) return string.format("%02x", c:byte()) end)))
print(string.unpack("<i4 >i8 d s1 z", packed))
print(string.packsize("i4 i8 d"), pcall(string.pack, "i17", 1))

local u = "h\u{e9}llo \u{4e16}\u{754c} \u{1f600}"
print(utf8.len(u), #u, utf8.char(72, 0x4e16, 0x1f600))
local cps = {}
for _, c in utf8.codes(u) do cps[#cps + 1] = string.format("%X", c) end
print(table.concat(cps, " "), utf8.codepoint(u, 1, 3), utf8.offset(u, 3))

-- Build and hash a large string.
local parts = {}
for i = 1, 5000 do parts[i] = string.format("%05d:%s", i, ("abc"):rep(i % 7)) end
local big = table.concat(parts, ";")
local h = 0
for i = 1, #big, 7 do h = (h * 131 + big:byte(i)) & 0xffffffff end
print(#big, h, big:sub(1000, 1040))
