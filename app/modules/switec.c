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

// Lua: setup(id, P1, P2, P3, P4)
static int lswitec_setup( lua_State* L )
{
  unsigned id;
  
  id = luaL_checkinteger( L, 1 );
  MOD_CHECK_ID( switec, id );
  int pin[4];

  int i;
  for (i = 0; i < 4; i++) {
    uint32_t gpio = luaL_checkinteger(L, 2 + i);

    if (gpio == 0 || gpio >= GPIO_PIN_NUM) {
      return luaL_error( L, "Pin number out of range." );
    }

    pin[i] = pin_num[gpio];
  }
  if (switec_setup( id, pin )) {
    return luaL_error( L, "Unable to setup stepper." );
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

// Module function map
static const LUA_REG_TYPE switec_map[] = {
  { LSTRKEY( "setup" ),    LFUNCVAL( lswitec_setup ) },
  { LSTRKEY( "close" ),    LFUNCVAL( lswitec_close ) },
  { LSTRKEY( "reset" ),    LFUNCVAL( lswitec_reset ) },
  { LSTRKEY( "moveto" ),   LFUNCVAL( lswitec_moveto) },
  { LNILKEY, LNILVAL }
};

NODEMCU_MODULE(SWITEC, "switec", switec_map, NULL);
