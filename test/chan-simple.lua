local llthreads = require "llthreads"
local chan = require "chan"

local thread_code = [[
    local chan = require "chan"
    chan.get("ch1"):send(...)
    return chan.get("ch2"):recv()
]]

local ch1 = chan.new("ch1")
local ch2 = chan.new("ch2")

-- create detached child thread.
local thread = llthreads.new(thread_code, "Hello")
-- start joinable child thread.
assert(thread:start())

local msg = ch1:recv()
print("PARENT: recv: '"..msg.."'")

ch2:send("World");
local ret, val = thread:join()

print("PARENT: child returned: ", ret, val)
