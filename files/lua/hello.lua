-- A first script from the Gravitas image (/lua/hello.lua).
print("hello from " .. _VERSION .. " on Gravitas")

local t = { 1, 2, 3, 5, 8, 13 }
local sum = 0
for _, v in ipairs(t) do sum = sum + v end
print("sum", sum)

print(("gravitas"):upper(), #("gravitas"), 2 ^ 10, math.pi)
print(os.date("%Y-%m-%d %H:%M:%S"))

local ok, err = pcall(function() error("expected") end)
print("pcall:", ok, err)

local function range(n)
    local i = 0
    return function() i = i + 1; if i <= n then return i end end
end
local acc = {}
for i in range(5) do acc[#acc + 1] = i * i end
print("squares:", table.concat(acc, " "))
