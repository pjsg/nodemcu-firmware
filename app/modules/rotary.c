/*
 * Module for interfacing with cheap rotary switches that
 * are much used in the automtive industry as the cntrols for 
 * CD players and the like.
 *
 * Philip Gladstone, N1DQ
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
  int press_callback;
  int release_callback;
  int turn_callback;
  int lastpos;
} DATA;

static DATA *data[ROTARY_CHANNEL_COUNT];
static task_handle_t tasknumber;

static void callback_free_one(lua_State *L, int *cb_ptr) 
{
  if (*cb_ptr != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, *cb_ptr);
    *cb_ptr = LUA_NOREF;
  }
}

static void callback_free(lua_State* L, unsigned int id, int mask) 
{
  DATA *d = data[id];

  if (d) {
    if (mask & ROTARY_PRESS) {
      callback_free_one(L, &d->press_callback);
    }
    if (mask & ROTARY_RELEASE) {
      callback_free_one(L, &d->release_callback);
    }
    if (mask & ROTARY_TURN) {
      callback_free_one(L, &d->turn_callback);
    }
  }
}

static int callback_setOne(lua_State* L, int *cb_ptr, int arg_number) 
{
  if (lua_type(L, arg_number) == LUA_TFUNCTION || lua_type(L, arg_number) == LUA_TLIGHTFUNCTION) {
    lua_pushvalue(L, arg_number);  // copy argument (func) to the top of stack
    callback_free_one(L, cb_ptr);
    *cb_ptr = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
  }

  return -1;
}

static int callback_set(lua_State* L, int id, int mask, int arg_number) 
{
  DATA *d = data[id];
  int result = 0;

  if (mask & ROTARY_TURN) {
    result |= callback_setOne(L, &d->turn_callback, arg_number);
  }
  if (mask & ROTARY_PRESS) {
    result |= callback_setOne(L, &d->press_callback, arg_number);
  }
  if (mask & ROTARY_RELEASE) {
    result |= callback_setOne(L, &d->release_callback, arg_number);
  }

  return result;
}

static void callback_callOne(lua_State* L, int cb, int mask, int arg) 
{
  if (cb != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, cb);

    lua_pushinteger(L, mask);
    lua_pushinteger(L, arg);

    lua_call(L, 2, 0);
  }
}

static void callback_call(lua_State* L, unsigned int id, int mask, int arg) 
{
  DATA *d = data[id];
  if (d) {
    if (mask & ROTARY_TURN) {
      callback_callOne(L, d->turn_callback, ROTARY_TURN, arg);
    }
    if (mask & ROTARY_PRESS) {
      callback_callOne(L, d->press_callback, ROTARY_PRESS, arg);
    }
    if (mask & ROTARY_RELEASE) {
      callback_callOne(L, d->release_callback, ROTARY_RELEASE, arg);
    }
  }
}

int platform_rotary_exists( unsigned int id )
{
  return (id < ROTARY_CHANNEL_COUNT);
}

// Lua: setup(id, phase_a, phase_b [, press])
static int lrotary_setup( lua_State* L )
{
  unsigned int id;
  
  id = luaL_checkinteger( L, 1 );
  MOD_CHECK_ID( rotary, id );

  if (rotary_close(id)) {
    return luaL_error( L, "Unable to close switch." );
  }
  callback_free(L, id, ROTARY_ALL);

  if (!data[id]) {
    data[id] = (DATA *) c_zalloc(sizeof(DATA));
    if (!data[id]) {
      return -1;
    } 
  }

  DATA *d = data[id];
  memset(d, 0, sizeof(*d));
  
  d->press_callback = LUA_NOREF;
  d->release_callback = LUA_NOREF;
  d->turn_callback = LUA_NOREF;

  int phase_a = luaL_checkinteger(L, 2);
  luaL_argcheck(L, platform_gpio_exists(phase_a) && phase_a > 0, 1, "Invalid pin");
  int phase_b = luaL_checkinteger(L, 3);
  luaL_argcheck(L, platform_gpio_exists(phase_b) && phase_b > 0, 2, "Invalid pin");
  int press;
  if (lua_gettop(L) >= 4) {
    press = luaL_checkinteger(L, 4);
    luaL_argcheck(L, platform_gpio_exists(press) && press > 0, 3, "Invalid pin");
  } else {
    press = -1;
  }

  if (rotary_setup(id, phase_a, phase_b, press, tasknumber)) {
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
  callback_free(L, id, ROTARY_ALL);

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
    if (callback_set(L, id, mask, 3)) {
      return luaL_error( L, "Unable to set callback." );
    }
  } else {
    callback_free(L, id, mask);
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
	  callback_call(L, id, ROTARY_TURN, (pos << 1) >> 1);
	}
	if ((pos ^ d->lastpos) & 0x80000000) {
	  // pressing or releasing has happened
	  callback_call(L, id, (pos & 0x80000000) ? ROTARY_PRESS : ROTARY_RELEASE, (pos << 1) >> 1);
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

  uint8_t *task_queue_ptr = (uint8_t*) param;
  if (task_queue_ptr) {
    // Signal that new events may need another task post
    *task_queue_ptr = 0;
  }

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
