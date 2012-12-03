Overview
========

"lua-chan" is a simple and fast message queue implementation for isolated Lua states
running in multiple threads. It is quite similar to "go chan".

The philosophy that writing a multi-threading Lua program is to isolate the threads
with separate Lua states. There is no shared data across the Lua states in threads.
All communication and data exchange can be performed with message queues. -- This is
actually the same philosophy on multi-threading as Dart language.

Usage
=====

"lua-chan" helps you to create message queues that can be accessed by all the Lua
states in the same process. A message queue is identified uniquely by a name which
can be any string.

First, we need to create a "chan":
```lua
local chan = require "chan"
local ch = chan.new("mychan")
```

Then, we can use this "chan" to send/recv messages. In other Lua state, we can use
the name to get the chan:
```lua
local chan = require "chan"
local ch = chan.get("mychan")
```
To use `chan.get`, the chan must be created using `chan.new`, otherwise, nil is returned.

Next, we can send/recv messages:
```lua
ch:send("message")
```
or
```lua
local msg = ch:recv()
```

"lua-chan" only implements message queues. For Lua states, lua-rings can be used, for
multi-threading, lua-llthreads, lua-lanes, etc. are good options.

References
==========

Create a chan:
```lua
chan.new(name, limit = 0)
```
* `name`: the unique name in string to identify the chan
* `limit`: the maximum messages can be queued in the chan, default is 0 which means
if there is no receiver, the sender will wait; specifying -1 here means there's no
limitation on the number of messages queued in the chan.

Get a chan:
```lua
chan.get(name)
```
* `name`: the unique name of the chan to be retrieved
* _return_: the retrieved chan or nil if not found

Send a message:
```lua
ch:send(message, timeout = -1)
```
* `message`: the message to push to queue; right now the message can only be boolean,
number or string.
* `timeout`: the timeout in milliseconds that the sender should wait if the chan has
no more capacity for queueing a message. -1 means waiting infinitely.
* _return_: `true` if queued successfully, or `false` if timeout.

Receive a message:
```lua
ch:recv(timeout = -1)
```
* `timeout`: the timeout in milliseconds that the receiver should wait until message arrives,
and specifying -1 means waiting infinitely.
* _return_: message or nil if timeout.

Dependencies
============

Only developed and tested for Lua 5.1.x on Linux.
No other dependencies.
