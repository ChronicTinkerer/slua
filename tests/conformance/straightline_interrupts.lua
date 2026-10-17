-- Straight-line code: no call, loop or return between these statements, so
-- the statements themselves are the only places a break can land. Each is an
-- allocation, one per opcode the engine checks before executing.
local s = string.rep("x", 64)
local m = 0
local n = 0

t = {} n += 1
t = {a = 1} n += 1
t = s .. "y" n += 1
-- Over a written local: a closure over an immutable one is shared (DUPCLOSURE)
m += 1 t = function() return m end n += 1

-- One allocating builtin per fastcall opcode
t = tostring(n) n += 1
t = string.sub(s, n) n += 1
t = string.sub(s, 2) n += 1
t = string.sub(s, 1, 8) n += 1
t = string.char(n, n, n, n) n += 1

-- Builtins on the trivial list get no check
t = math.abs(n) n += 1
t = math.max(n, m) n += 1

-- Every broken statement ran exactly once after its resume
assert(n == 11)

return "OK"
