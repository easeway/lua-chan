local llthreads = require "llthreads"
local chan = require "chan"

local ch1 = chan.new("ch1")
local ch2 = chan.new("ch2")

local thread_code = [[
    local chan = require "chan"
    local name = select(1, ...)
    chn_s = chan.get(select(2, ...))
    chn_r = chan.get(select(3, ...))
    local start = select(4, ...)
    if start then
        print(name .. ": send " .. start)
        chn_s:send(start)
    end
    while true do
        local val = chn_r:recv()
        print(name .. ": recv " .. val)
        val = val + 1
        print(name .. ": send " .. val)
        local done = chn_s:send(val, 3000)
        if val >= 100 then
            return val
        end
        if not done then
            print(name .. ": send timeout")
            return val
        end
    end
]]

-- create detached child thread.
local t1 = llthreads.new(thread_code, "1", "ch1", "ch2")
local t2 = llthreads.new(thread_code, "2", "ch2", "ch1", 1)

-- start joinable child thread.
assert(t1:start())
assert(t2:start())

local _, r1 = t1:join()
local _, r2 = t2:join()

print("T1", r1)
print("T2", r2)
