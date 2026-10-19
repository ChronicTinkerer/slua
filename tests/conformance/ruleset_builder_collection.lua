-- Conformance tests for 'C' (string-csv) and 'M' (string-map) ruleset builder semantics.
--
-- The C++ harness registers ruleset_apply(params), which returns params coerced
-- using a synthetic descriptor table:
--   tag 5  'C'  whitelist
--   tag 7  'M'  custom_headers
--   tag 9  's'  label

local function check(params, expected)
    -- it's fine to compare the json-encoded rules because they will _always_
    -- be array-like tables with defined sort order. The whole intent of the functions
    -- is to turn mappings into flat lists.
    local got, want = lljson.slencode(ruleset_apply(params)), lljson.slencode(expected)
    assert(got == want, `expected {want}, got {got}`)
end

-- ─── 'C' string-csv ──────────────────────────────────────────────────────────

-- Single URL: emits {tag, "url"}.
check({ whitelist = {"https://example.com"} }, {5, "https://example.com"})

-- Multiple URLs: comma-joined, array index order preserved.
check(
    { whitelist = {"https://a.com", "https://b.com"} },
    {5, "https://a.com,https://b.com"}
)
check(
    { whitelist = {"first", "second", "third", "fourth", "fifth"} },
    {5, "first,second,third,fourth,fifth"}
)

-- Comma in a URL must be backslash-escaped; a backslash must be doubled.
check({ whitelist = {"https://a.com/p?x=1,y=2"} }, {5, "https://a.com/p?x=1\\,y=2"})
check({ whitelist = {"https://a.com/a\\b"} }, {5, "https://a.com/a\\\\b"})

-- Empty array or unset: tag must not appear in output.
check({ whitelist = {} }, {})
check({}, {})

-- Separators are only written once the string is non-empty, so leading empty strings vanish.
check({ whitelist = {"", "b", "", "c"} }, {5, "b,,c"})

-- ─── 'M' string-map ──────────────────────────────────────────────────────────

-- Single header: emits {tag, key, value}.
check({ custom_headers = {["X-Foo"] = "bar"} }, {7, "X-Foo", "bar"})

-- Multiple headers: tag appears once per pair, keys sorted.
check(
    { custom_headers = {["Z-Last"] = "z", ["A-First"] = "a"} },
    {7, "A-First", "a", 7, "Z-Last", "z"}
)

-- Empty table or unset: tag must not appear in output.
check({ custom_headers = {} }, {})
check({}, {})

-- Keys with embedded NULs are kept whole.
check({ custom_headers = {["a\0b"] = "x"} }, {7, "a\0b", "x"})
check({ custom_headers = {["a"] = "1", ["a\0b"] = "2"} }, {7, "a", "1", 7, "a\0b", "2"})

-- Built-in value types are converted as by tostring().
local id, rot, vec = uuid("00000000-0000-0000-0000-000000000001"), quaternion(0, 0, 0, 1), vector(1, 2, 3)
check(
    { custom_headers = {Q = rot, U = id, V = vec} },
    {7, "Q", tostring(rot), 7, "U", tostring(id), 7, "V", tostring(vec)}
)

-- Script __tostring metamethods are never called; such values are skipped.
local called = false
local obj = setmetatable({}, { __tostring = function() called = true return "x" end })
check({ whitelist = {obj, "a"}, custom_headers = {K = obj} }, {5, "a"})
assert(not called, "script __tostring must not run during serialization")

-- ─── Co-existence with scalar property ───────────────────────────────────────

-- All three descriptor types in one call, tag-sorted output.
check(
    { whitelist = {"https://x.com"}, custom_headers = {["K"] = "V"}, label = "hello" },
    {5, "https://x.com", 7, "K", "V", 9, "hello"}
)

-- ─── Coercion: dict vs sequential list ───────────────────────────────────────

-- Dict input is coerced to a flat tag/value list.
check({label = "solo"}, {9, "solo"})
check({whitelist = {"a", "b"}, label = "hi"}, {5, "a,b", 9, "hi"})

-- Sequential list (has integer key 1) passes through unchanged.
check({5, "prebuilt", 9, "label"}, {5, "prebuilt", 9, "label"})

-- Empty table passes through unchanged.
check({}, {})

-- Serializing through a relative stack index.
local rel = lljson.slencode(ruleset_serialize_rel({label = "rel"}))
assert(rel == lljson.slencode({9, "rel"}), `relative index: got {rel}`)

return "OK"
