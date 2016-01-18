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

// Lua: moveto( id, pos )
static int lswitec_moveto( lua_State* L )
{
  unsigned id;
  
  id = luaL_checkinteger( L, 1 );
  MOD_CHECK_ID( switec, id );
  int pos;
  pos = luaL_checkinteger( L, 2 );
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
  if (switec_getpos( id, &pos, &dir )) {
    return luaL_error( L, "Unable to get position." );
  }
  lua_pushnumber(L, pos);
  lua_pushnumber(L, dir);
  return 2;
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
