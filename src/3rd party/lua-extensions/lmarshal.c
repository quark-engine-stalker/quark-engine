/*
* lmarshal.c
* A Lua library for serializing and deserializing Lua values
* Richard Hundt <richardhundt@gmail.com>
*
* License: MIT
*
* Copyright (c) 2010 Richard Hundt
*
* Permission is hereby granted, free of charge, to any person
* obtaining a copy of this software and associated documentation
* files (the "Software"), to deal in the Software without
* restriction, including without limitation the rights to use,
* copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following
* conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
* OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
* HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lmarshal.h"


#define MAR_TREF 1
#define MAR_TVAL 2
#define MAR_TUSR 3

#define MAR_CHR 1
#define MAR_I32 4
#define MAR_I64 8

#define MAR_MAGIC 0x8e
#define SEEN_IDX  3

typedef struct mar_Buffer {
    size_t size;
    size_t seek;
    size_t head;
    char*  data;
} mar_Buffer;

static int mar_encode_table(lua_State *L, mar_Buffer *buf, size_t *idx);
static int mar_decode_table(lua_State *L, const char* buf, size_t len, size_t *idx);

static void buf_init(lua_State *L, mar_Buffer *buf)
{
    buf->size = 128;
    buf->seek = 0;
    buf->head = 0;
    if (!(buf->data = malloc(buf->size))) luaL_error(L, "Out of memory!");
}

static void buf_done(lua_State* L, mar_Buffer *buf)
{
    free(buf->data);
}

static int buf_write(lua_State* L, const char* str, size_t len, mar_Buffer *buf)
{
    if (len > UINT32_MAX) luaL_error(L, "buffer too long");
    if (buf->size - buf->head < len) {
        size_t new_size = buf->size << 1;
        size_t cur_head = buf->head;
        while (new_size - cur_head <= len) {
            new_size = new_size << 1;
        }
        if (!(buf->data = realloc(buf->data, new_size))) {
            luaL_error(L, "Out of memory!");
        }
        buf->size = new_size;
    }
    memcpy(&buf->data[buf->head], str, len);
    buf->head += len;
    return 0;
}

static const char* buf_read(lua_State *L, mar_Buffer *buf, size_t *len)
{
    if (buf->seek < buf->head) {
        buf->seek = buf->head;
        *len = buf->seek;
        return buf->data;
    }
    *len = 0;
    return NULL;
}

static void mar_encode_value(lua_State *L, mar_Buffer *buf, int val, size_t *idx)
{
    size_t l;
    int val_type = lua_type(L, val);
    lua_pushvalue(L, val);

    buf_write(L, (void*)&val_type, MAR_CHR, buf);
    switch (val_type) {
    case LUA_TBOOLEAN: {
        int int_val = lua_toboolean(L, -1);
        buf_write(L, (void*)&int_val, MAR_CHR, buf);
        break;
    }
    case LUA_TSTRING: {
        const char *str_val = lua_tolstring(L, -1, &l);
        buf_write(L, (void*)&l, MAR_I32, buf);
        buf_write(L, str_val, l, buf);
        break;
    }
    case LUA_TNUMBER: {
        lua_Number num_val = lua_tonumber(L, -1);
        buf_write(L, (void*)&num_val, MAR_I64, buf);
        break;
    }
    case LUA_TTABLE: {
        int tag, ref;
        lua_pushvalue(L, -1);
        lua_rawget(L, SEEN_IDX);
        if (!lua_isnil(L, -1)) {
            ref = lua_tointeger(L, -1);
            tag = MAR_TREF;
            buf_write(L, (void*)&tag, MAR_CHR, buf);
            buf_write(L, (void*)&ref, MAR_I32, buf);
            lua_pop(L, 1);
        }
        else {
            mar_Buffer rec_buf;
            lua_pop(L, 1); /* pop nil */
            if (luaL_getmetafield(L, -1, "__persist")) {
                tag = MAR_TUSR;

                lua_pushvalue(L, -2); /* self */
                lua_call(L, 1, 1);
                if (!lua_isfunction(L, -1)) {
                    luaL_error(L, "__persist must return a function");
                }

                lua_remove(L, -2); /* __persist */

                lua_newtable(L);
                lua_pushvalue(L, -2); /* callback */
                lua_rawseti(L, -2, 1);

                buf_init(L, &rec_buf);
                mar_encode_table(L, &rec_buf, idx);

                buf_write(L, (void*)&tag, MAR_CHR, buf);
                buf_write(L, (void*)&rec_buf.head, MAR_I32, buf);
                buf_write(L, rec_buf.data, rec_buf.head, buf);
                buf_done(L, &rec_buf);
                lua_pop(L, 1);
            }
            else {
                tag = MAR_TVAL;

                lua_pushvalue(L, -1);
                lua_pushinteger(L, (*idx)++);
                lua_rawset(L, SEEN_IDX);

                lua_pushvalue(L, -1);
                buf_init(L, &rec_buf);
                mar_encode_table(L, &rec_buf, idx);
                lua_pop(L, 1);

                buf_write(L, (void*)&tag, MAR_CHR, buf);
                buf_write(L, (void*)&rec_buf.head, MAR_I32, buf);
                buf_write(L, rec_buf.data,rec_buf.head, buf);
                buf_done(L, &rec_buf);
            }
        }
        break;
    }
    case LUA_TFUNCTION: {
        int tag, ref;
        lua_pushvalue(L, -1);
        lua_rawget(L, SEEN_IDX);
        if (!lua_isnil(L, -1)) {
            ref = lua_tointeger(L, -1);
            tag = MAR_TREF;
            buf_write(L, (void*)&tag, MAR_CHR, buf);
            buf_write(L, (void*)&ref, MAR_I32, buf);
            lua_pop(L, 1);
        }
        else {
            mar_Buffer rec_buf;
            int i;
            lua_Debug ar;
            lua_pop(L, 1); /* pop nil */

            lua_pushvalue(L, -1);
            lua_getinfo(L, ">nuS", &ar);
            if (ar.what[0] != 'L') {
                luaL_error(L, "attempt to persist a C function '%s'", ar.name);
            }
            tag = MAR_TVAL;
            lua_pushvalue(L, -1);
            lua_pushinteger(L, (*idx)++);
            lua_rawset(L, SEEN_IDX);

            lua_pushvalue(L, -1);
            buf_init(L, &rec_buf);
            lua_dump(L, (lua_Writer)buf_write, &rec_buf);

            buf_write(L, (void*)&tag, MAR_CHR, buf);
            buf_write(L, (void*)&rec_buf.head, MAR_I32, buf);
            buf_write(L, rec_buf.data, rec_buf.head, buf);
            buf_done(L, &rec_buf);
            lua_pop(L, 1);

            lua_newtable(L);
            for (i=1; i <= ar.nups; i++) {
                lua_getupvalue(L, -2, i);
                lua_rawseti(L, -2, i);
            }

            buf_init(L, &rec_buf);
            mar_encode_table(L, &rec_buf, idx);

            buf_write(L, (void*)&rec_buf.head, MAR_I32, buf);
            buf_write(L, rec_buf.data, rec_buf.head, buf);
            buf_done(L, &rec_buf);
            lua_pop(L, 1);
        }

        break;
    }
    case LUA_TUSERDATA: {
        int tag, ref;
        lua_pushvalue(L, -1);
        lua_rawget(L, SEEN_IDX);
        if (!lua_isnil(L, -1)) {
            ref = lua_tointeger(L, -1);
            tag = MAR_TREF;
            buf_write(L, (void*)&tag, MAR_CHR, buf);
            buf_write(L, (void*)&ref, MAR_I32, buf);
            lua_pop(L, 1);
        }
        else {
            mar_Buffer rec_buf;
            lua_pop(L, 1); /* pop nil */
            if (luaL_getmetafield(L, -1, "__persist")) {
                tag = MAR_TUSR;

                lua_pushvalue(L, -2);
                lua_pushinteger(L, (*idx)++);
                lua_rawset(L, SEEN_IDX);

                lua_pushvalue(L, -2);
                lua_call(L, 1, 1);
                if (!lua_isfunction(L, -1)) {
                    luaL_error(L, "__persist must return a function");
                }
                lua_newtable(L);
                lua_pushvalue(L, -2);
                lua_rawseti(L, -2, 1);
                lua_remove(L, -2);

                buf_init(L, &rec_buf);
                mar_encode_table(L, &rec_buf, idx);

                buf_write(L, (void*)&tag, MAR_CHR, buf);
                buf_write(L, (void*)&rec_buf.head, MAR_I32, buf);
		buf_write(L, rec_buf.data, rec_buf.head, buf);
		buf_done(L, &rec_buf);
            }
            else {
                luaL_error(L, "attempt to encode userdata (no __persist hook)");
            }
            lua_pop(L, 1);
        }
        break;
    }
    case LUA_TNIL: break;
    default:
        luaL_error(L, "invalid value type (%s)", lua_typename(L, val_type));
    }
    lua_pop(L, 1);
}

static int mar_encode_table(lua_State *L, mar_Buffer *buf, size_t *idx)
{
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        mar_encode_value(L, buf, -2, idx);
        mar_encode_value(L, buf, -1, idx);
        lua_pop(L, 1);
    }
    return 1;
}

static const char *mar_decode_bytes(lua_State *L, const char *buf, size_t len,
                                    const char **p, size_t count)
{
    size_t offset = (size_t)(*p - buf);
    const char *result;

    if (offset > len || count > len - offset) {
        luaL_error(L, "bad encoded data: truncated input");
    }

    result = *p;
    *p += count;
    return result;
}

static uint8_t mar_decode_u8(lua_State *L, const char *buf, size_t len,
                             const char **p)
{
    uint8_t result;
    memcpy(&result, mar_decode_bytes(L, buf, len, p, sizeof(result)),
           sizeof(result));
    return result;
}

static uint32_t mar_decode_u32(lua_State *L, const char *buf, size_t len,
                               const char **p)
{
    uint32_t result;
    memcpy(&result, mar_decode_bytes(L, buf, len, p, sizeof(result)),
           sizeof(result));
    return result;
}

static int32_t mar_decode_i32(lua_State *L, const char *buf, size_t len,
                              const char **p)
{
    int32_t result;
    memcpy(&result, mar_decode_bytes(L, buf, len, p, sizeof(result)),
           sizeof(result));
    return result;
}

static void mar_decode_reference(lua_State *L, int32_t ref, size_t idx)
{
    if (ref <= 0 || (size_t)ref >= idx) {
        luaL_error(L, "bad encoded data: invalid reference");
    }

    lua_rawgeti(L, SEEN_IDX, ref);
    if (lua_isnil(L, -1)) {
        luaL_error(L, "bad encoded data: missing reference");
    }
}

static void mar_decode_value
    (lua_State *L, const char *buf, size_t len, const char **p, size_t *idx)
{
    size_t l;
    uint8_t val_type = mar_decode_u8(L, buf, len, p);
    switch (val_type) {
    case LUA_TBOOLEAN:
        lua_pushboolean(L, mar_decode_u8(L, buf, len, p));
        break;
    case LUA_TNUMBER: {
        lua_Number value;
        if (sizeof(value) != MAR_I64) {
            luaL_error(L, "unsupported lua_Number size");
        }
        memcpy(&value, mar_decode_bytes(L, buf, len, p, MAR_I64), MAR_I64);
        lua_pushnumber(L, value);
        break;
    }
    case LUA_TSTRING: {
        const char *value;
        l = mar_decode_u32(L, buf, len, p);
        value = mar_decode_bytes(L, buf, len, p, l);
        lua_pushlstring(L, value, l);
        break;
    }
    case LUA_TTABLE: {
        uint8_t tag = mar_decode_u8(L, buf, len, p);
        if (tag == MAR_TREF) {
            int32_t ref = mar_decode_i32(L, buf, len, p);
            mar_decode_reference(L, ref, *idx);
        }
        else if (tag == MAR_TVAL) {
            const char *table_data;
            l = mar_decode_u32(L, buf, len, p);
            table_data = mar_decode_bytes(L, buf, len, p, l);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_rawseti(L, SEEN_IDX, (*idx)++);
            mar_decode_table(L, table_data, l, idx);
        }
        else if (tag == MAR_TUSR) {
            const char *table_data;
            l = mar_decode_u32(L, buf, len, p);
            table_data = mar_decode_bytes(L, buf, len, p, l);
            lua_newtable(L);
            mar_decode_table(L, table_data, l, idx);
            lua_rawgeti(L, -1, 1);
            if (!lua_isfunction(L, -1)) {
                luaL_error(L, "bad encoded data: invalid persist callback");
            }
            lua_call(L, 0, 1);
            lua_remove(L, -2);
            lua_pushvalue(L, -1);
            lua_rawseti(L, SEEN_IDX, (*idx)++);
        }
        else {
            luaL_error(L, "bad encoded data: invalid table tag");
        }
        break;
    }
    case LUA_TFUNCTION: {
        int load_status;
        int nups;
        int i;
        mar_Buffer dec_buf;
        uint8_t tag = mar_decode_u8(L, buf, len, p);
        if (tag == MAR_TREF) {
            int32_t ref = mar_decode_i32(L, buf, len, p);
            mar_decode_reference(L, ref, *idx);
        }
        else if (tag == MAR_TVAL) {
            const char *function_data;
            const char *upvalue_data;
            lua_Debug ar;

            l = mar_decode_u32(L, buf, len, p);
            function_data = mar_decode_bytes(L, buf, len, p, l);
            dec_buf.data = (char*)function_data;
            dec_buf.size = l;
            dec_buf.head = l;
            dec_buf.seek = 0;
            load_status = lua_load(L, (lua_Reader)buf_read, &dec_buf, "=marshal");
            if (load_status != 0) {
                const char *message = lua_tostring(L, -1);
                luaL_error(L, "cannot decode persisted Lua function: %s",
                           message ? message : "invalid bytecode");
            }

            lua_pushvalue(L, -1);
            lua_rawseti(L, SEEN_IDX, (*idx)++);

            l = mar_decode_u32(L, buf, len, p);
            upvalue_data = mar_decode_bytes(L, buf, len, p, l);
            lua_newtable(L);
            mar_decode_table(L, upvalue_data, l, idx);

            lua_pushvalue(L, -2);
            if (!lua_getinfo(L, ">u", &ar)) {
                luaL_error(L, "bad encoded data: invalid Lua function");
            }
            nups = ar.nups;
            for (i=1; i <= nups; i++) {
                lua_rawgeti(L, -1, i);
                if (lua_setupvalue(L, -3, i) == NULL) {
                    /* LuaJIT leaves the value on the stack when this fails. */
                    lua_pop(L, 1);
                    luaL_error(L, "bad encoded data: invalid function upvalues");
                }
            }
            lua_pop(L, 1);
        }
        else {
            luaL_error(L, "bad encoded data: invalid function tag");
        }
        break;
    }
    case LUA_TUSERDATA: {
        uint8_t tag = mar_decode_u8(L, buf, len, p);
        if (tag == MAR_TREF) {
            int32_t ref = mar_decode_i32(L, buf, len, p);
            mar_decode_reference(L, ref, *idx);
        }
        else if (tag == MAR_TUSR) {
            const char *table_data;
            l = mar_decode_u32(L, buf, len, p);
            table_data = mar_decode_bytes(L, buf, len, p, l);
            lua_newtable(L);
            mar_decode_table(L, table_data, l, idx);
            lua_rawgeti(L, -1, 1);
            if (!lua_isfunction(L, -1)) {
                luaL_error(L, "bad encoded data: invalid persist callback");
            }
            lua_call(L, 0, 1);
            lua_remove(L, -2);
            lua_pushvalue(L, -1);
            lua_rawseti(L, SEEN_IDX, (*idx)++);
        }
        else if (tag == MAR_TVAL) {
            lua_pushnil(L);
        }
        else {
            luaL_error(L, "bad encoded data: invalid userdata tag");
        }
        break;
    }
    case LUA_TNIL:
    case LUA_TTHREAD:
        lua_pushnil(L);
        break;
    default:
        luaL_error(L, "bad code");
    }
}

static int mar_decode_table(lua_State *L, const char* buf, size_t len, size_t *idx)
{
    const char* p;
    p = buf;
    while (p - buf < len) {
        mar_decode_value(L, buf, len, &p, idx);
        if ((size_t)(p - buf) == len) {
            luaL_error(L, "bad encoded data: table value is missing");
        }
        mar_decode_value(L, buf, len, &p, idx);
        lua_settable(L, -3);
    }
    return 1;
}

static int mar_encode(lua_State* L)
{
    const unsigned char m = MAR_MAGIC;
    size_t idx, len;
    mar_Buffer buf;

    if (lua_isnone(L, 1)) {
        lua_pushnil(L);
    }
    if (lua_isnoneornil(L, 2)) {
        lua_newtable(L);
    }
    else if (!lua_istable(L, 2)) {
        luaL_error(L, "bad argument #2 to encode (expected table)");
    }
    lua_settop(L, 2);

    len = lua_objlen(L, 2);
    lua_newtable(L);
    for (idx = 1; idx <= len; idx++) {
        lua_rawgeti(L, 2, idx);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushinteger(L, idx);
        lua_rawset(L, SEEN_IDX);
    }
    lua_pushvalue(L, 1);

    buf_init(L, &buf);
    buf_write(L, (void*)&m, 1, &buf);

    mar_encode_value(L, &buf, -1, &idx);

    lua_pop(L, 1);

    lua_pushlstring(L, buf.data, buf.head);

    buf_done(L, &buf);

    lua_remove(L, SEEN_IDX);

    return 1;
}

static int mar_decode(lua_State* L)
{
    size_t l, idx, len;
    const char *p;
    const char *s = luaL_checklstring(L, 1, &l);

    if (l < 1) luaL_error(L, "bad header");
    if (*(unsigned char *)s++ != MAR_MAGIC) luaL_error(L, "bad magic");
    l -= 1;

    if (lua_isnoneornil(L, 2)) {
        lua_newtable(L);
    }
    else if (!lua_istable(L, 2)) {
        luaL_error(L, "bad argument #2 to decode (expected table)");
    }
    lua_settop(L, 2);

    len = lua_objlen(L, 2);
    lua_newtable(L);
    for (idx = 1; idx <= len; idx++) {
        lua_rawgeti(L, 2, idx);
        lua_rawseti(L, SEEN_IDX, idx);
    }

    p = s;
    mar_decode_value(L, s, l, &p, &idx);

    lua_remove(L, SEEN_IDX);
    lua_remove(L, 2);

    return 1;
}

static int mar_clone(lua_State* L)
{
    mar_encode(L);
    lua_replace(L, 1);
    mar_decode(L);
    return 1;
}

static const struct luaL_Reg R[] =
{
    {"encode",      mar_encode},
    {"decode",      mar_decode},
    {"clone",       mar_clone},
    {NULL,	    NULL},
};

int luaopen_marshal(lua_State *L)
{
    //lua_newtable(L);
    //luaL_register(L, NULL, R);
		luaL_register(L, "marshal", R);
    return 1;
}
