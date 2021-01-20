/*
 * 唯一id生成器
 * 参考snowflake算法
 * 0 00000000 00 00000000 0000 00000000 00000000 00000000 00000000 00000000 0
 * | |reserved | |serial num | |---------------timestamp--------------------|
 * 最高位符号位不使用
 * serial num:每毫秒最多可生成4096个
 * timestamp够用69年,单位毫秒
 */

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

#include "skynet.h"
#include "skynet_timer.h"
#include "spinlock.h"

#define SERIAL_SHIFT 12
#define RESERVED_SHIFT 10
#define TIME_SHIFT 41
#define START_TIME 1610021791000
#define MAX_TIMESTAMP ((((uint64_t)1) << TIME_SHIFT) - 1)
#define MAX_SERIAL ((1 << SERIAL_SHIFT) - 1)

struct Record {
    uint64_t last_gen_time;
    uint32_t serial_num;
    struct spinlock lock;
};

static struct Record *record;
static int kind_num;
static int reg_key;

static struct Record *get_record(lua_State *L, int idx);
static uint64_t get_time(void);
static uint64_t get_pass_time(lua_State *L, uint64_t now, const char *tag);
static uint64_t gen_uniqid(lua_State *L, struct Record *r, 
        uint64_t pass_time, uint64_t now, const char *tag);
static struct skynet_context *get_skynet_context(lua_State *L);
//static void dump_stack(lua_State *L);

/*-------------lua api------------*/

/*
 * 参数:
 *     1.idx:id种类的下标
 * 返回值:
 *     生成的id
 * 说明:
 *     该接口生成的id保证进程内唯一，即可以保证不同服务(线程)间生成的id唯一
 * */
static int lnextid_p(lua_State *L) {
    int idx = luaL_checkinteger(L, 1);
    if(idx <= 0 || idx > kind_num)
        return luaL_error(L, "invalid idx");
    struct Record *r = record + (idx - 1);
    uint64_t now = get_time();
    uint64_t pass_time = get_pass_time(L, now, "lnextid_p");
    assert(r->last_gen_time >= 0);
    SPIN_LOCK(r)
    uint64_t uniqid = gen_uniqid(L, r, pass_time, now, "lnextid_p");
    SPIN_UNLOCK(r)
    lua_pushinteger(L, uniqid);
    return 1;
}

/*
 * 参数:
 *     1.idx:id种类的下标
 * 返回值:
 *     生成的id
 * 说明:
 *     该接口生成的id只能保证单服务(线程)内唯一,不能保证进程内唯一
 * */
static int lnextid_s(lua_State *L) {
    int idx = luaL_checkinteger(L, 1);
    struct Record *r = get_record(L, idx);
    uint64_t now = get_time();
    uint64_t pass_time = get_pass_time(L, now, "lnextid_s");
    assert(r->last_gen_time >= 0);
    uint64_t uniqid = gen_uniqid(L, r, pass_time, now, "lnextid_s");
    lua_pushinteger(L, uniqid);
    return 1;
}

/*
 * 参数:
 *     1.kind_num:支持的id种类数量
 * 返回值:
 *     无
 * 说明:
 *     该接口仅用来初始化lnextid_p接口的数据,必须在所有服务启动前完成初始化
 * */
static int linit(lua_State *L) {
    kind_num = luaL_checkinteger(L, 1);
    assert(kind_num > 0);
    record = calloc(kind_num, sizeof(struct Record));
    if(record == NULL) {
        perror("malloc record");
        exit(EXIT_FAILURE);
    }
    return 0;
}

/*--------------------private function---------------*/

static uint64_t get_time(void) {
    return ((uint64_t)skynet_starttime() * 1000) + skynet_now();
}

static struct skynet_context *get_skynet_context(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "skynet_context");
    luaL_checktype(L, -1, LUA_TLIGHTUSERDATA);
    struct skynet_context *ctx = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ctx;
}

static uint64_t get_pass_time(lua_State *L, uint64_t now, const char *tag) {
    assert(now >= START_TIME);
    uint64_t pass_time = now - START_TIME;

    //如果timestamp部分超出上限，只打印错误日志，但依然可以分配出id，会重复
    //此时会引起逻辑错误，但进程不退出，避免伤害过大
    if(pass_time > MAX_TIMESTAMP) {
        struct skynet_context *ctx = get_skynet_context(L);
        skynet_error(ctx, "ERROR:get_pass_time in %s timestamp overflow:%ld", tag, pass_time);
    }
    return pass_time;
}

static uint64_t gen_uniqid(lua_State *L, struct Record *r, 
        uint64_t pass_time, uint64_t now, const char *tag) {
    uint64_t uniqid = 0;
    if(r->last_gen_time == 0) {
        r->last_gen_time = now;
        r->serial_num = 0;
        uniqid = pass_time;
    }
    else if(r->last_gen_time > 0) {
        if(r->last_gen_time == now) {
            uniqid = (((uint64_t)(++r->serial_num)) << TIME_SHIFT) | pass_time;
            //如果serial num超出上限，跟timestamp策略相同
            if(r->serial_num > MAX_SERIAL) {
                struct skynet_context *ctx = get_skynet_context(L);
                skynet_error(ctx, "ERROR:gen_uniqid in %s serial overflow:%ld", tag, r->serial_num);
            }
        }
        else if(r->last_gen_time < now) {
            r->serial_num = 0;
            r->last_gen_time = now;
            uniqid = pass_time;
        }
        //时钟回拨，CLOCK_MONOTONIC下不可能发生
        else if(r->last_gen_time > now) {
            struct skynet_context *ctx = get_skynet_context(L);
            skynet_error(ctx, "ERROR:gen_uniqid in %s clock back,now:%ld,last_gen_time:%ld", tag, now, r->last_gen_time);
        }
    }
    return uniqid;
}

static struct Record *get_record(lua_State *L, int idx) {
    void *reg_key_addr = (void *)&reg_key;
    lua_rawgetp(L, LUA_REGISTRYINDEX, reg_key_addr);
    if(lua_isnil(L, -1)) {
        struct Record *r = calloc(1, sizeof(struct Record));
        if(r == NULL) {
            perror("malloc record");
            exit(EXIT_FAILURE);
        }
        lua_newtable(L);
        lua_pushinteger(L, idx);
        lua_pushlightuserdata(L, (void *)r);
        lua_rawset(L, -3);
        lua_rawsetp(L, LUA_REGISTRYINDEX, reg_key_addr);
        lua_pop(L, 1);
        lua_rawgetp(L, LUA_REGISTRYINDEX, reg_key_addr);
    }
    luaL_checktype(L, -1, LUA_TTABLE);
    lua_pushinteger(L, idx);
    lua_rawget(L, -2);
    if(lua_isnil(L, -1)) {
        struct Record *r = calloc(1, sizeof(struct Record));
        if(r == NULL) {
            perror("malloc record");
            exit(EXIT_FAILURE);
        }
        lua_pushinteger(L, idx);
        lua_pushlightuserdata(L, (void *)r);
        lua_rawset(L, -4);
        lua_pop(L, 1);
        lua_pushinteger(L, idx);
        lua_rawget(L, -2);
    }
    luaL_checktype(L, -1, LUA_TLIGHTUSERDATA);
    struct Record *r = (struct Record *)lua_touserdata(L, -1);
    lua_pop(L, 2);
    return r;
}

/*
static void dump_stack(lua_State *L) {
	int i;
	int top = lua_gettop(L);
	for (i = 1; i <= top; i++) {
	    int t = lua_type(L, i);
	    switch (t) {
		    case LUA_TSTRING: {
			    printf("'%s'", lua_tostring(L, i));
			    break;
		    }
		    case LUA_TBOOLEAN: {
			    printf(lua_toboolean(L, i) ? "true" : "false");
			    break;
		    }
		    case LUA_TNUMBER: {
			    printf("%g", lua_tonumber(L, i));
			    break;
		    }
		    default: {
			    printf("%s", lua_typename(L, t));
			    break;
		    }
	    }
	    printf(" ");
    }
    printf("\n");
}*/

static const struct luaL_Reg arraylib_f[] = {
    {"init", linit},
    {"nextid_p", lnextid_p},
    {"nextid_s", lnextid_s},
    {NULL, NULL}
};

int luaopen_id_generator(lua_State *L){
    luaL_checkversion(L);
    luaL_newlib(L, arraylib_f);
    return 1;
}
