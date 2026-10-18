-- Break propagation in dangerouslyexecuterequiredmodule
function require()
    error("don't actually call me")
end

-- Basic break propagation
local result = dangerouslyexecuterequiredmodule(function()
    breaker()
    return "after break"
end)
assert(result == "after break", "Should continue after break")

-- Break with return values
local a, b, c = dangerouslyexecuterequiredmodule(function()
    breaker()
    return 1, 2, 3
end)
assert(a == 1 and b == 2 and c == 3, "Should return values after break")

-- Nested breaks
local nested_result = dangerouslyexecuterequiredmodule(function()
    breaker()
    return dangerouslyexecuterequiredmodule(function()
        breaker()
        return "nested break"
    end)
end)
assert(nested_result == "nested break", "Should handle nested breaks")

-- Extra arguments are dropped, so the continuation still finds the thread
local extra = dangerouslyexecuterequiredmodule(function()
    breaker()
    return "extra ignored"
end, 1, 2)
assert(extra == "extra ignored", "Should ignore extra arguments across a break")

return "OK"
