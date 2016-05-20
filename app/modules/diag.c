/*
 * Module for doing various horrible things 
 *
 * Philip Gladstone, N1DQ
 */

#include "module.h"
#include "lauxlib.h"
#include "platform.h"
#include "c_types.h"
#include "../libc/c_stdlib.h"
#include "user_interface.h"
#include "driver/uart.h"
#include "osapi.h"

static void check_aligned(lua_State *L, uint32_t address) 
{
  if (address & 3) {
    luaL_error( L, "Address must be aligned on 32-bit boundary" );
  }
}

// Lua: osprint(true/false)
static int ldiag_osprint( lua_State* L )
{
  if (lua_toboolean(L, 1)) {
    system_set_os_print(1);
    //os_install_putc1((void *)uart0_write_char);
  } else {
    system_set_os_print(0);
    //os_install_putc1((void *)uart1_write_char);
  }

  return 0;  
}

// Lua: peek(address) -> value
static int ldiag_peek( lua_State* L )
{
  uint32_t address;
  address = luaL_checkinteger( L, 1 );

  check_aligned(L, address);

  lua_pushnumber(L, *(uint32_t *) address);

  return 1;  
}

// Lua: poke(address, value)
static int ldiag_poke( lua_State* L )
{
  uint32_t address;
  address = luaL_checkinteger( L, 1 );

  check_aligned(L, address);

  uint32_t value;
  value = luaL_checkinteger(L, 2);

  *(uint32_t *) address = value;

  return 0;  
}

// Lua: witlb(address, attr)
static int ldiag_witlb( lua_State* L )
{
  uint32_t addr = luaL_checknumber(L, 1);
  uint32_t attr = luaL_checknumber(L, 2);
  asm (
      "j 1f;"
      ".align 16;"
      "1: witlb  %0, %1;"
      "isync;"
      : 
      : "r" (attr), "r" (addr)
  );

  return 0;  
}


// Lua: ritlb1() -> 8 values
static int ldiag_ritlb1( lua_State* L )
{
  int i;
  for (i = 0; i < 8; i++) {
    uint32_t value;
    asm (
        "ritlb1  %0, %1;"
	: "=r" (value)
	: "r" (i << 29)
    );
    lua_pushnumber(L, value);
  }

  return 8;  
}

static void ldiag_task(os_param_t param, uint8_t prio)
{
    (void) param;
    (void) prio;
}

// Lua: taskid() -> 1 values
static int ldiag_taskid( lua_State* L )
{
  lua_pushnumber(L, task_get_id(ldiag_task));

  return 1;  
}

// Module function map
static const LUA_REG_TYPE diag_map[] = {
  { LSTRKEY( "peek" ),    LFUNCVAL( ldiag_peek    ) },
  { LSTRKEY( "poke" ),    LFUNCVAL( ldiag_poke    ) },
  { LSTRKEY( "osprint" ),    LFUNCVAL( ldiag_osprint    ) },
  { LSTRKEY( "taskid" ),    LFUNCVAL( ldiag_taskid    ) },
#ifdef DIAG_INCLUDE_TLB
  { LSTRKEY( "ritlb1" ),    LFUNCVAL( ldiag_ritlb1    ) },
  { LSTRKEY( "witlb" ),    LFUNCVAL( ldiag_witlb    ) },
#endif

  { LNILKEY, LNILVAL }
};

NODEMCU_MODULE(DIAG, "diag", diag_map, NULL);
