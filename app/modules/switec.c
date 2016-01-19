/*
 * Module for interfacing with Switec instrument steppers (and
 * similar devices). These are the steppers that are used in automotive 
 * instrument panels and the like. Run off 5 volts at low current.
 *
 * Code inspired by:
 *
 * SwitecX25 Arduino Library
 *  Guy Carpenter, Clearwater Software - 2012
 *
 *  Licensed under the BSD2 license, see license.txt for details.
 *
 * NodeMcu integration by Philip Gladstone, N1DQ
 */

#include "module.h"
#include "lauxlib.h"
#include "platform.h"
#include "c_types.h"
#include "driver/switec.h"

static int stoppedCallback[2] = { LUA_NOREF, LUA_NOREF };

static void callbackFree(lua_State* L, unsigned int id) 
{
  if (stoppedCallback[id] != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, stoppedCallback[id]);
    stoppedCallback[id] = LUA_NOREF;
  }
}

static void callbackSet(lua_State* L, unsigned int id, int argNumber) 
{
  if (lua_type(L, argNumber) == LUA_TFUNCTION || lua_type(L, argNumber) == LUA_TLIGHTFUNCTION) {
    lua_pushvalue(L, argNumber);  // copy argument (func) to the top of stack
    callbackFree(L, id);
    stoppedCallback[id] = luaL_ref(L, LUA_REGISTRYINDEX);
  }
}

static void callbackExecute(lua_State* L, unsigned int id) 
{
  if (stoppedCallback[id] != LUA_NOREF) {
    int callback = stoppedCallback[id];
    lua_rawgeti(L, LUA_REGISTRYINDEX, callback);
    stoppedCallback[id] = LUA_NOREF;
    luaL_unref(L, LUA_REGISTRYINDEX, callback);

    lua_call(L, 0, 0);
  }
}

int platform_switec_exists( unsigned int id )
{
  return (id < 2);
}

// Lua: setup(id, P1, P2, P3, P4, maxSpeed)
static int lswitec_setup( lua_State* L )
{
  unsigned id;
  
  id = luaL_checkinteger( L, 1 );
  MOD_CHECK_ID( switec, id );
  int pin[4];

  if (switec_close(id)) {
    return luaL_error( L, "Unable to setup stepper." );
  }

  int i;
  for (i = 0; i < 4; i++) {
    uint32_t gpio = luaL_checkinteger(L, 2 + i);

    if (gpio == 0 || gpio >= GPIO_PIN_NUM) {
      return luaL_error( L, "Pin number out of range." );
    }

    pin[i] = pin_num[gpio];

    platform_gpio_mode(gpio, PLATFORM_GPIO_OUTPUT, PLATFORM_GPIO_PULLUP);
  }

  int degPerSec = 0;
  if (lua_gettop(L) >= 6) {
    degPerSec = luaL_checkinteger(L, 6);
  }

  if (switec_setup(id, pin, degPerSec)) {
    return luaL_error(L, "Unable to setup stepper.");
  }
  return 0;  
}

// Lua: close( id )
static int lswitec_close( lua_State* L )
{
  unsigned id;
  
  id = luaL_checkinteger( L, 1 );
  MOD_CHECK_ID( switec, id );
  callbackFree(L, id);
  if (switec_close( id )) {
    return luaL_error( L, "Unable to close stepper." );
  }
  return 0;  
}

// Lua: reset( id )
static int lswitec_reset( lua_State* L )
{
  unsigned id;
  id = luaL_checkinteger( L, 1 );
  MOD_CHECK_ID( switec, id );
  if (switec_reset( id )) {
    return luaL_error( L, "Unable to reset stepper." );
  }
  return 0;  
}

// Lua: moveto( id, pos [, cb] )
static int lswitec_moveto( lua_State* L )
{
  unsigned id;
  
  id = luaL_checkinteger( L, 1 );
  MOD_CHECK_ID( switec, id );
  int pos;
  pos = luaL_checkinteger( L, 2 );

  if (lua_gettop(L) >= 3) {
    callbackSet(L, id, 3);
  } else {
    callbackFree(L, id);
  }

  if (switec_moveto( id, pos )) {
    return luaL_error( L, "Unable to move stepper." );
  }
  return 0;
}

// Lua: getpos( id ) -> position, moving
static int lswitec_getpos( lua_State* L )
{
  unsigned id;
  
  id = luaL_checkinteger( L, 1 );
  MOD_CHECK_ID( switec, id );
  int32_t pos;
  int32_t dir;
  int32_t target;
  if (switec_getpos( id, &pos, &dir, &target )) {
    return luaL_error( L, "Unable to get position." );
  }
  lua_pushnumber(L, pos);
  lua_pushnumber(L, dir);
  return 2;
}

void lswitec_callback_check(lua_State* L)
{
  int id;

  for (id = 0; id < 2; id++) {
    if (stoppedCallback[id] != LUA_NOREF) {
      int32_t pos;
      int32_t dir;
      int32_t target;
      if (!switec_getpos( id, &pos, &dir, &target )) {
	if (dir == 0 && pos == target) {
	  callbackExecute(L, id);
	}
      }
    }
  }
}

// Module function map
static const LUA_REG_TYPE switec_map[] = {
  { LSTRKEY( "setup" ),    LFUNCVAL( lswitec_setup ) },
  { LSTRKEY( "close" ),    LFUNCVAL( lswitec_close ) },
  { LSTRKEY( "reset" ),    LFUNCVAL( lswitec_reset ) },
  { LSTRKEY( "moveto" ),   LFUNCVAL( lswitec_moveto) },
  { LSTRKEY( "getpos" ),   LFUNCVAL( lswitec_getpos) },
  { LNILKEY, LNILVAL }
};

NODEMCU_MODULE(SWITEC, "switec", switec_map, NULL);
