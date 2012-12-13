// Copyright (C) 2012 EaseWay (easeway@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#ifdef DEBUG
#include <stdio.h>
#define TRACE(x...) printf(x)
#else
#define TRACE(...)
#endif

struct msg_t
{
    union {
        char* str;
        lua_Number num;
        int bool_val;
    };
    size_t str_len;
    int type;
    struct msg_t* next;
};

struct queue_t
{
    char* name;
    struct msg_t* msg_head;
    struct msg_t* msg_tail;
    int limit;
    int count;
    pthread_mutex_t lock;
    pthread_cond_t send_sig;
    pthread_cond_t recv_sig;
    struct queue_t* prev;
    struct queue_t* next;
    int refs;
    int bucket;
};

static struct queue_t* queue_create(const char* name, int limit)
{
    size_t name_len = strlen(name);
    struct queue_t* q = (struct queue_t*)malloc(sizeof(struct queue_t) + name_len + 1);
    q->name = (char*)q + sizeof(struct queue_t);
    memcpy(q->name, name, name_len + 1);
    q->msg_head = q->msg_tail = NULL;
    q->limit = limit;
    q->count = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->send_sig, NULL);
    pthread_cond_init(&q->recv_sig, NULL);
    q->prev = q->next = NULL;
    q->refs = 1;
    TRACE("queue_create: %s, limit=%d\n", name, limit);
    return q;
}

static void queue_destroy(struct queue_t* q)
{
    struct msg_t *msgs = q->msg_head, *last = NULL;
    TRACE("queue_destroy: %s\n", q->name);
    free(q);
    while (msgs)
    {
        last = msgs;
        msgs = msgs->next;
        free(last);
    }
}

static void queue_lock(struct queue_t* q)
{
    pthread_mutex_lock(&q->lock);
}

static void queue_unlock(struct queue_t* q)
{
    pthread_mutex_unlock(&q->lock);
}

static long queue_acquire(struct queue_t* q)
{
    long refs;
    queue_lock(q);
    refs = ++ q->refs;
    queue_unlock(q);
    TRACE("queue_acquire: %s, refs=%d\n", q->name, q->refs);
    return refs;
}

static void queues_detach(struct queue_t* q);

static long queue_release(struct queue_t* q)
{
    long refs;
    queue_lock(q);
    refs = -- q->refs;
    TRACE("queue_release: %s, refs=%d\n", q->name, q->refs);
    if (refs == 0)
    {
        queues_detach(q);
    }
    queue_unlock(q);
    if (refs == 0)
        queue_destroy(q);
    return refs;
}

static int timeout_to_timespec(int timeout, struct timespec* ts)
{
    if (timeout < 0)
        return 0;
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += timeout * 1000000L;
    ts->tv_sec += ts->tv_nsec / 1000000000L;
    ts->tv_nsec = ts->tv_nsec % 1000000000L;
    return 1;
}

static int queue_send(struct queue_t* q, struct msg_t* msg, int timeout)
{
    struct timespec ts;

    queue_lock(q);

    if (timeout > 0 && q->limit >= 0 && q->count + 1 > q->limit)
        timeout_to_timespec(timeout, &ts);
    while (timeout != 0 && q->limit >= 0 && q->count + 1 > q->limit)
    {
        if (timeout > 0)
        {
            if (pthread_cond_timedwait(&q->send_sig, &q->lock, &ts) != 0)
                break;
        }
        else
            pthread_cond_wait(&q->send_sig, &q->lock);
    }

    if (q->limit < 0 || q->count + 1 <= q->limit)
    {
        msg->next = NULL;
        if (q->msg_tail)
            q->msg_tail->next = msg;
        q->msg_tail = msg;
        if (q->msg_head == NULL)
            q->msg_head = msg;
        q->count ++;
        pthread_cond_signal(&q->recv_sig);
    }
    else
    {
        msg = NULL;
    }

    queue_unlock(q);
    return msg ? 1 : 0;
}

static struct msg_t* queue_recv(struct queue_t* q, int timeout)
{
    struct msg_t* msg = NULL;
    struct timespec ts;

    queue_lock(q);
    if (q->limit >= 0)
    {
        q->limit ++;
        pthread_cond_signal(&q->send_sig);
    }

    if (timeout > 0 && q->count <= 0)
        timeout_to_timespec(timeout, &ts);
    while (timeout != 0 && q->count <= 0)
    {
        if (timeout > 0)
        {
            if (pthread_cond_timedwait(&q->recv_sig, &q->lock, &ts) != 0)
                break;
        }
        else
            pthread_cond_wait(&q->recv_sig, &q->lock);
    }

    if (q->count > 0)
    {
        msg = q->msg_head;
        if (msg)
        {
            q->msg_head = msg->next;
            if (q->msg_head == NULL)
                q->msg_tail = NULL;
            msg->next = NULL;
        }
        q->count --;
        pthread_cond_signal(&q->send_sig);
    }

    if (q->limit > 0)
        q->limit --;

    queue_unlock(q);
    return msg;
}

#define BUCKET_SIZE 16
struct entry_t
{
    struct queue_t* head;
    struct queue_t* tail;
};

static struct entry_t _queues[BUCKET_SIZE];
static pthread_mutex_t _queues_lock = PTHREAD_MUTEX_INITIALIZER;

static int name_hash(const char* name)
{
    int hash = 0;
    char ch;
    while ((ch = *name ++) != 0)
    {
        hash += ch;
        hash &= 0xff;
    }
    return hash & (BUCKET_SIZE - 1);
}

static struct queue_t* bucket_search(int bucket, const char* name)
{
    struct queue_t* q = _queues[bucket].head;
    for (; q; q = q->next)
    {
        if (strcmp(q->name, name) == 0)
            return q;
    }
    return NULL;
}

static int queues_add(struct queue_t* q)
{
    int hash = name_hash(q->name);
    pthread_mutex_lock(&_queues_lock);
    if (bucket_search(hash, q->name))
    {
        pthread_mutex_unlock(&_queues_lock);
        return 0;
    }
    q->next = NULL;
    q->prev = _queues[hash].tail;
    if (_queues[hash].tail)
        _queues[hash].tail->next = q;
    _queues[hash].tail = q;
    if (!_queues[hash].head)
        _queues[hash].head = q;
    q->bucket = hash;
    pthread_mutex_unlock(&_queues_lock);
    TRACE("queues_add: %s bucket=%d\n", q->name, hash);
    return 1;
}

static struct queue_t* queues_get(const char* name)
{
    int hash = name_hash(name);
    struct queue_t* q = NULL;
    pthread_mutex_lock(&_queues_lock);
    q = bucket_search(hash, name);
    if (q)
        queue_acquire(q);
    pthread_mutex_unlock(&_queues_lock);
    return q;
}

static void queues_detach(struct queue_t* q)
{
    TRACE("queues_detach: %s, bucket=%d\n", q->name, q->bucket);
    if (q->prev) q->prev->next = q->next;
    else _queues[q->bucket].head = q->next;
    if (q->next) q->next->prev = q->prev;
    else _queues[q->bucket].tail = q->prev;
    q->next = q->prev = NULL;
    q->bucket = -1;
}

static void _lua_usage(lua_State* L, const char* usage)
{
    lua_pushstring(L, usage);
    lua_error(L);
}

static const char* _lua_arg_string(lua_State* L, int index, const char *def_val, const char* usage)
{
    if (lua_gettop(L) >= index)
    {
        const char* str = lua_tostring(L, index);
        if (str)
            return str;
    }
    else if (def_val)
    {
        return def_val;
    }
    _lua_usage(L, usage);
    return NULL;
}

static int _lua_arg_integer(lua_State* L, int index, int optional, int def_val, const char* usage)
{
    if (lua_gettop(L) >= index)
    {
        if (lua_isnumber(L, index))
            return (int)lua_tointeger(L, index);
    }
    else if (optional)
    {
        return def_val;
    }
    _lua_usage(L, usage);
    return 0;
}

static struct queue_t* _lua_arg_queue(lua_State* L)
{
    struct queue_t* q = (struct queue_t*)lua_topointer(L, 1);
    if (q == NULL)
    {
        lua_pushstring(L, "invalid queue object");
        lua_error(L);
    }
    return q;
}

static const char* _usage_send = "chan:send(string|number|boolean, timeout = -1)";

static int chan_send(lua_State* L)
{
    int type, timeout, ret;
    struct msg_t* msg;
    struct queue_t* q = _lua_arg_queue(L);
    if (lua_gettop(L) < 2)
        _lua_usage(L, _usage_send);

    if (lua_isstring(L, 2)) type = LUA_TSTRING;
    else if (lua_isnumber(L, 2)) type = LUA_TNUMBER;
    else if (lua_isboolean(L, 2)) type = LUA_TBOOLEAN;
    else
        _lua_usage(L, _usage_send);

    timeout = _lua_arg_integer(L, 3, 1, -1, _usage_send);

    switch (type)
    {
        case LUA_TSTRING:
            {
                size_t len = 0;
                const char* str = lua_tolstring(L, 2, &len);
                msg = (struct msg_t*)malloc(sizeof(struct msg_t) + len + 1);
                msg->str = (char*)msg + sizeof(struct msg_t);
                memcpy(msg->str, str, msg->str_len = len);
                msg->str[len] = 0;
            }
            break;
        case LUA_TNUMBER:
            {
                msg = (struct msg_t*)malloc(sizeof(struct msg_t));
                msg->num = lua_tonumber(L, 2);
            }
            break;
        case LUA_TBOOLEAN:
            {
                msg = (struct msg_t*)malloc(sizeof(struct msg_t));
                msg->bool_val = lua_toboolean(L, 2);
            }
            break;
    }
    msg->type = type;

    ret = queue_send(q, msg, timeout);
    if (!ret)
    {
        free(msg);
    }
    lua_pushboolean(L, ret);
    return 1;
}

static const char* _usage_recv = "chan:recv(timeout = -1)";

static int chan_recv(lua_State* L)
{
    struct queue_t* q = _lua_arg_queue(L);
    int timeout = _lua_arg_integer(L, 2, 1, -1, _usage_recv);
    struct msg_t* msg = queue_recv(q, timeout);
    if (msg)
    {
        switch (msg->type)
        {
            case LUA_TSTRING:
                lua_pushlstring(L, msg->str, msg->str_len);
                break;
            case LUA_TNUMBER:
                lua_pushnumber(L, msg->num);
                break;
            case LUA_TBOOLEAN:
                lua_pushboolean(L, msg->bool_val);
                break;
            default:
                lua_pushstring(L, "bad internal state");
                lua_error(L);
                break;
        }
        free (msg);
    }
    else
    {
        lua_pushnil(L);
    }
    return 1;
}

static int chan_gc(lua_State* L)
{
    struct queue_t* q = _lua_arg_queue(L);
    TRACE("chan_gc: %s, refs=%d\n", q->name, q->refs);
    pthread_mutex_lock(&_queues_lock);
    queue_release(q);
    pthread_mutex_unlock(&_queues_lock);
    return 0;
}

#define METATABLE_NAME  "LUA_CHAN_METATABLE"

static chan_pushqueue(lua_State* L, struct queue_t* q)
{
    lua_pushlightuserdata(L, q);
    luaL_getmetatable(L, METATABLE_NAME);
    lua_setmetatable(L, -2);
}

static const char* _usage_new = "chan.new(name, limit = 0)";

static int chan_new(lua_State* L)
{
    const char* name = _lua_arg_string(L, 1, NULL, _usage_new);
    int limit = _lua_arg_integer(L, 2, 1, 0, _usage_new);
    struct queue_t* q = queue_create(name, limit);
    if (!queues_add(q))
    {
        queue_destroy(q);
        lua_pushnil(L);
        lua_pushstring(L, "chan name duplicated");
        return 2;
    }
    chan_pushqueue(L, q);
    return 1;
}

static const char* _usage_get = "chan get(name)";

static int chan_get(lua_State* L)
{
    const char* name = _lua_arg_string(L, 1, NULL, _usage_get);
    struct queue_t* q = queues_get(name);
    if (q)
    {
        chan_pushqueue(L, q);
        return 1;
    }
    else
    {
        lua_pushnil(L);
        lua_pushstring(L, "not found");
        return 2;
    }
};

static const luaL_Reg chan_meta_fns[] = {
    { "send", chan_send },
    { "recv", chan_recv },
    { "__gc", chan_gc },
    { NULL, NULL }
};

static const luaL_Reg chan_fns[] = {
    { "new", chan_new },
    { "get", chan_get },
    { NULL, NULL }
};

int luaopen_chan(lua_State* L)
{
    luaL_newmetatable(L, METATABLE_NAME);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, chan_meta_fns);
    luaL_register(L, "chan", chan_fns);
    return 1;
}
