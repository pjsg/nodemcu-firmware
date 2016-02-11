/*
 * Module for interfacing with cheap rotary switches that
 * are much used in the automtive industry as the cntrols for 
 * CD players and the like.
 *
 * NodeMcu integration by Philip Gladstone, N1DQ
 */

#include "module.h"
#include "lauxlib.h"
#include "platform.h"
#include "c_types.h"
#include "driver/rotary.h"
#include "../libc/c_stdlib.h"

#define ROTARY_PRESS	1
#define ROTARY_RELEASE	2
#define ROTARY_TURN	4
#define ROTARY_ALL	7

typedef struct {
  int pressCallback;
  int releaseCallback;
  int turnCallback;
  int lastpos;
} DATA;

static DATA *data[ROTARY_CHANNEL_COUNT];
static task_handle_t tasknumber;

static void callbackFreeOne(lua_State *L, int *cbPtr) 
{
  if (*cbPtr != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, *cbPtr);
    *cbPtr = LUA_NOREF;
  }
}

static void callbackFree(lua_State* L, unsigned int id, int mask) 
{
  DATA *d = data[id];

  if (d) {
    if (mask & ROTARY_PRESS) {
      callbackFreeOne(L, &d->pressCallback);
    }
    if (mask & ROTARY_RELEASE) {
      callbackFreeOne(L, &d->releaseCallback);
    }
    if (mask & ROTARY_TURN) {
      callbackFreeOne(L, &d->turnCallback);
    }
  }
}

static int callbackSetOne(lua_State* L, int *cbPtr, int argNumber) 
{
  if (lua_type(L, argNumber) == LUA_TFUNCTION || lua_type(L, argNumber) == LUA_TLIGHTFUNCTION) {
    lua_pushvalue(L, argNumber);  // copy argument (func) to the top of stack
    callbackFreeOne(L, cbPtr);
    *cbPtr = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
  }

  return -1;
}

static int callbackSet(lua_State* L, int id, int mask, int argNumber) 
{
  DATA *d = data[id];
  int result = 0;

  if (mask & ROTARY_TURN) {
    result |= callbackSetOne(L, &d->turnCallback, argNumber);
  }
  if (mask & ROTARY_PRESS) {
    result |= callbackSetOne(L, &d->pressCallback, argNumber);
  }
  if (mask & ROTARY_RELEASE) {
    result |= callbackSetOne(L, &d->releaseCallback, argNumber);
  }

  return result;
}

static void callbackCallOne(lua_State* L, int cb, int mask, int arg) 
{
  if (cb != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, cb);

    lua_pushinteger(L, mask);
    lua_pushinteger(L, arg);

    lua_call(L, 2, 0);
  }
}

static void callbackCall(lua_State* L, unsigned int id, int mask, int arg) 
{
  DATA *d = data[id];
  if (d) {
    if (mask & ROTARY_TURN) {
      callbackCallOne(L, d->turnCallback, ROTARY_TURN, arg);
    }
    if (mask & ROTARY_PRESS) {
      callbackCallOne(L, d->pressCallback, ROTARY_PRESS, arg);
    }
    if (mask & ROTARY_RELEASE) {
      callbackCallOne(L, d->releaseCallback, ROTARY_RELEASE, arg);
    }
  }
}

int platform_rotary_exists( unsigned int id )
{
  return (id < ROTARY_CHANNEL_COUNT);
}

// Lua: setup(id, phaseA, phaseB [, press])
static int lrotary_setup( lua_State* L )
{
  unsigned int id;
  
  id = luaL_checkinteger( L, 1 );
  MOD_CHECK_ID( rotary, id );

  if (rotary_close(id)) {
    return luaL_error( L, "Unable to setup switch." );
  }
  callbackFree(L, id, ROTARY_ALL);

  if (!data[id]) {
    data[id] = (DATA *) c_zalloc(sizeof(DATA));
    if (!data[id]) {
      return -1;
    } 
  }

  DATA *d = data[id];
  memset(d, 0, sizeof(*d));
  
  d->pressCallback = LUA_NOREF;
  d->releaseCallback = LUA_NOREF;
  d->turnCallback = LUA_NOREF;

  int phaseA = luaL_checkinteger(L, 2);
  int phaseB = luaL_checkinteger(L, 3);
  int press;
  if (lua_gettop(L) >= 4) {
    press = luaL_checkinteger(L, 4);
  } else {
    press = -1;
  }

  if (phaseA <= 0 || phaseA >= GPIO_PIN_NUM
     || phaseB <= 0 || phaseB >= GPIO_PIN_NUM
     || press == 0 || press < -1 || press >= GPIO_PIN_NUM) {
    return luaL_error( L, "Pin number out of range." );
  }

  if (rotary_setup(id, phaseA, phaseB, press, tasknumber)) {
    return luaL_error(L, "Unable to setup rotary switch.");
  }
  return 0;  
}

// Lua: close( id )
static int lrotary_close( lua_State* L )
{
  unsigned int id;
  
  id = luaL_checkinteger( L, 1 );
  MOD_CHECK_ID( rotary, id );
  callbackFree(L, id, ROTARY_ALL);

  DATA *d = data[id];
  if (d) {
    data[id] = NULL;
    c_free(d);
  }

  if (rotary_close( id )) {
    return luaL_error( L, "Unable to close switch." );
  }
  return 0;  
}

// Lua: on( id, mask[, cb] )
static int lrotary_on( lua_State* L )
{
  unsigned int id;
  id = luaL_checkinteger( L, 1 );
  MOD_CHECK_ID( rotary, id );

  int mask = luaL_checkinteger(L, 2);

  if (lua_gettop(L) >= 3) {
    if (callbackSet(L, id, mask, 3)) {
      return luaL_error( L, "Unable to set callback." );
    }
  } else {
    callbackFree(L, id, mask);
  }

  return 0;  
}

// Lua: getpos( id ) -> pos, PRESS/RELEASE
static int lrotary_getpos( lua_State* L )
{
  unsigned int id;
  id = luaL_checkinteger( L, 1 );
  MOD_CHECK_ID( rotary, id );

  int pos = rotary_getpos(id);

  if (pos == -1) {
    return 0;
  }

  lua_pushnumber(L, (pos << 1) >> 1);
  lua_pushnumber(L, (pos & 0x80000000) ? ROTARY_PRESS : ROTARY_RELEASE);

  return 2;  
}

#ifdef ROTARY_DEBUG
// Lua: getqueue( id ) -> pos, PRESS/RELEASE, pos, PRESS/RLEASE, ... interrupt_count
static int lrotary_getqueue( lua_State* L )
{
  unsigned int id;
  id = luaL_checkinteger( L, 1 );
  MOD_CHECK_ID( rotary, id );

  int32_t buffer[16];
  size_t i = rotary_getstate(id, buffer, sizeof(buffer) / sizeof(buffer[0]));

  int j;

  for (j = 0; j < i; j++) {
    lua_pushnumber(L, (buffer[j] << 1) >> 1);
    lua_pushnumber(L, (buffer[j] & 0x80000000) ? ROTARY_PRESS : ROTARY_RELEASE);
  }

  extern uint32_t rotary_interrupt_count;
  lua_pushnumber(L, rotary_interrupt_count);

  return 1 + i * 2;  
}
#endif

static int lrotary_dequeue(lua_State* L)
{
  int id;

  for (id = 0; id < ROTARY_CHANNEL_COUNT; id++) {
    DATA *d = data[id];

    if (d) {
      // This chnnel is open
      int pos = rotary_getevent(id);

      if (pos != d->lastpos) {
	// We have something to enqueue
	if ((pos ^ d->lastpos) & 0x7fffffff) {
	  // Some turning has happened
	  callbackCall(L, id, ROTARY_TURN, (pos << 1) >> 1);
	}
	if ((pos ^ d->lastpos) & 0x80000000) {
	  // pressing or releasing has happened
	  callbackCall(L, id, (pos & 0x80000000) ? ROTARY_PRESS : ROTARY_RELEASE, (pos << 1) >> 1);
	}

	d->lastpos = pos;
      }
    }
  }
}

static void lrotary_task(os_param_t param, uint8_t prio) 
{
  (void) param;
  (void) prio;

  lrotary_dequeue(lua_getstate());
}

static int rotary_open(lua_State *L) 
{
  tasknumber = task_get_id(lrotary_task);
  return 0;
}

// Module function map
static const LUA_REG_TYPE rotary_map[] = {
  { LSTRKEY( "setup" ),    LFUNCVAL( lrotary_setup ) },
  { LSTRKEY( "close" ),    LFUNCVAL( lrotary_close ) },
  { LSTRKEY( "on" ),       LFUNCVAL( lrotary_on    ) },
  { LSTRKEY( "getpos" ),   LFUNCVAL( lrotary_getpos) },
#ifdef ROTARY_DEBUG
  { LSTRKEY( "getqueue" ), LFUNCVAL( lrotary_getqueue) },
  { LSTRKEY( "dequeue" ),  LFUNCVAL( lrotary_dequeue) },
#endif
  { LSTRKEY( "TURN" ),     LNUMVAL( ROTARY_TURN    ) },
  { LSTRKEY( "PRESS" ),    LNUMVAL( ROTARY_PRESS   ) },
  { LSTRKEY( "RELEASE" ),  LNUMVAL( ROTARY_RELEASE ) },
  { LSTRKEY( "ALL" ),      LNUMVAL( ROTARY_ALL     ) },

  { LNILKEY, LNILVAL }
};

NODEMCU_MODULE(ROTARY, "rotary", rotary_map, rotary_open);
