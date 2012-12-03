local llthreads = require "llthreads"
local chan = require "chan"

local ch1 = chan.new("ch1", 4)

assert(ch1:send(1, 0))
assert(ch1:send(2, 0))
assert(ch1:send(3, 0))
assert(ch1:send(4, 0))
assert(not ch1:send(5, 0))

for i = 1, 4 do
    local v = ch1:recv(0)
    assert(v - i == 0)
end
assert(ch1:recv(0) == nil)

local ch2 = chan.new("ch2")
assert(not ch2:send(1, 0))
local code = [[
    local chan = require "chan"
    local ch = chan.get("ch2")
    ch:recv()
]]

local t = {}
for i = 1, 4 do
    t[i] = llthreads.new(code)
    t[i]:start()
end

for i = 1, 4 do
    assert(ch2:send(i, 0))
end

assert(not ch2:send(1, 0))

for i = 1, 4 do
    t[i]:join()
end
