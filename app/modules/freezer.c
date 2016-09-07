// Module for interfacing with freezing: copying code into flash to save RAM

#include "module.h"
#include "lauxlib.h"

#include "lmem.h"

#include "platform.h"
#include "lrodefs.h"
#include "lstring.h"

#include "c_types.h"
#include "c_stdlib.h"
#include "c_string.h"
#include "user_interface.h"
#include "flash_api.h"
#include "user_version.h"
#include "rom.h"

#undef NODE_DBG
#define NODE_DBG c_printf

// First 32 bits of the flash area contain flags
// flash_area[0]:
//     1:    0 -- erase on next boot
//     2:    1 -- erased on this boot
//
#define ERASE_ON_NEXT_BOOT0 	1
#define ERASED_ON_THIS_BOOT1	2

static volatile uint8_t flash_area[65536] __attribute__ ((aligned(4096), section(".irom0.text")));
static uint32_t flash_area_phys;

#define OPT_CONSTANTS	1
#define OPT_CONSTANT_VECTOR 2
#define OPT_LOCVARS	4
#define OPT_LOCVAR_VECTOR 8
#define OPT_UPVALS	 0x10
#define OPT_UPVAL_VECTOR 0x20
#define OPT_SOURCE 	 0x40
#define OPT_CODE	 0x80
#define OPT_LINEINFO     0x100
#define OPT_WRITE	 0x200
#define OPT_DEBUG        0x10000
static uint32_t opts = (-1) & ~OPT_DEBUG;

#define NODE_DBG_OPT(...) if (opts & OPT_DEBUG) { NODE_DBG(__VA_ARGS__); }

static inline bool is_flash(void *ptr) {
  return (uint8_t *) ptr >= flash_area && (uint8_t *) ptr < flash_area + sizeof(flash_area);
}

static void move_to_flash(uint8_t *dest, const uint8_t *src, size_t len) {
  lua_assert(dest >= flash_area && dest + len < flash_area + sizeof(flash_area));
  platform_flash_write(src, dest - flash_area + flash_area_phys, len);
}

static void needs_erase() {
  uint8_t byte = flash_area[0];

  byte &= ~ERASE_ON_NEXT_BOOT0;

  move_to_flash((uint8_t *) &flash_area[0], &byte, sizeof(byte));
}

static void *find_data(const void *src, size_t len) {
  if (!len) {
    return (void *) (flash_area + 4);
  }
  size_t blen = (len + 7) & ~7;

  if (opts & OPT_DEBUG) {
    NODE_DBG("Looking for %d bytes (blocklen %d): ", len, blen);
    int i;
    for (i = 0; i < len && i < 24; i++) {
      NODE_DBG("%02x ", ((unsigned char *) src)[i]);
    }
    NODE_DBG("\n");
  }

  int32_t *ptr = ((int32_t *) flash_area) + 1;

  while (*ptr > 0) {
    int blocklen = *ptr;
    if (blocklen == blen) {
      if (memcmp(ptr + 1, src, len) == 0) {

	NODE_DBG_OPT(".. found at 0x%08x\n", ptr);
	return (void *) (ptr + 1);
      }
    }
    ptr = (uint32_t *) (((uint8_t *) ptr) + blocklen + 4);
    if (*ptr != blocklen) {
      needs_erase();
      return NULL;
    }
    ptr++;
  }

  if (*ptr != -1) {
    // corrupt
    needs_erase();
    return NULL;
  }

  if (((uint8_t *) ptr) + 8 + blen > flash_area + sizeof(flash_area)) {
    // doesn't fit
    if (flash_area[0] & ERASED_ON_THIS_BOOT1) {
      // not much to do.
    } else {
      // Lets clean out
      needs_erase();
    }
    return NULL;
  }

  NODE_DBG_OPT("Adding block at 0x%x\n", ptr);

  int32_t *blockbase = ptr;

  move_to_flash((uint8_t *) ptr, (uint8_t *) &blen, sizeof(blen));
  ptr++;
  move_to_flash((uint8_t *) ptr, src, len);
  move_to_flash(((uint8_t *) ptr) + blen, (uint8_t *) &blen, sizeof(blen));

  int32_t *blockend = (int32_t *) (((uint8_t *) ptr) + blen + sizeof(int32_t));

  // Try and flush the cache
  int offset;

  for (offset = 0x2000; offset <= 0x6000; offset += 0x2000) {
    volatile int32_t *p;
    for (p = blockbase; p < blockend; p++) {
      p[offset];
    }
  }

  if (*blockbase != blen) {
    NODE_DBG("0x%x: HDR %d != %d\n", blockbase, *blockbase, blen);
  }
  if (memcmp(ptr, src, len) != 0) {
    NODE_DBG("0x%x, %d: BLK differs\n", ptr, len);
  }
  if (blockend[-1] != blen) {
    NODE_DBG("0x%x: TRL %d != %d\n", blockend - 1, blockend[-1], blen);
  }

  return (void *) ptr;
}

static bool check_consistency() {
  int32_t *ptr = ((int32_t *) flash_area) + 1;

  while (*ptr > 0) {
    int blocklen = *ptr;

    ptr = (uint32_t *) (((uint8_t *) ptr) + blocklen + 4);
    if (*ptr != blocklen) {
      NODE_DBG("consistency fail: %d != %d\n", *ptr, blocklen);
      return 0;
    }
    ptr++;
  }

  if (*ptr == -1) {
    return 1;
  }

  NODE_DBG("consistency fail: %d != -1\n", *ptr);
  return 0;
}

static int freezer_info(lua_State *L) {
  int32_t *ptr = ((int32_t *) flash_area) + 1;

  while (*ptr > 0) {
    int blocklen = *ptr;

    ptr = (uint32_t *) (((uint8_t *) ptr) + blocklen + 4);
    if (*ptr != blocklen) {
      needs_erase();
      break;
    }
    ptr++;
  }

  lua_pushnumber(L, flash_area[0]);
  lua_pushnumber(L, ((uint8_t *) ptr) - flash_area);
  lua_pushnumber(L, sizeof(flash_area));
  lua_pushnumber(L, check_consistency());

  return 4;
}

static TString *freeze_tstring(lua_State *L, TString *s, size_t *freedp) {
  if ((uint8_t *) s >= flash_area) {
    return s;
  }

  NODE_DBG_OPT("Freezing string '%s'\n", getstr(s));

  // Make a new readonly TString object
  size_t len = sizestring(&s->tsv);
  TString *tstr = (TString *) alloca(len);
  memcpy(tstr, s, len);
  tstr->tsv.next = NULL;
  tstr->tsv.marked &= READONLYMASK;
  ((unsigned short *) tstr)[3] = -1;  // Zap unused field

  tstr = find_data(tstr, len);

  if (!tstr) {
    return s;
  }

  *freedp += len;

  if (!(opts & OPT_WRITE)) {
    return s;
  }

  return tstr;
}

static int do_freeze_proto(lua_State *L, Proto *f, int depth) {
  int i;

  if (!f || is_flash(f->code) || is_flash(f->source) || depth > 4) {
    NODE_DBG_OPT("Early exit proto=0x%x\n", f);
    return 0;
  }

  size_t freed = 0;

  if (opts & OPT_SOURCE) {
    if (f->source) {
      f->source = freeze_tstring(L, f->source, &freed);
    }
  }

  if (opts & OPT_CODE) {
    Instruction *newcode = (Instruction *) find_data(f->code, sizeof(f->code[0]) * f->sizecode);
    if (newcode && (opts & OPT_WRITE)) {
      luaM_freearray(L, f->code, f->sizecode, Instruction);
      f->code = newcode;
      freed += f->sizecode * sizeof(Instruction);
    }
  }

  bool all_readonly = TRUE;
  
  if (opts & OPT_UPVALS) {
    for (i = 0; i < f->sizeupvalues; i++) {
      TString *str = f->upvalues[i];

      f->upvalues[i] = freeze_tstring(L, str, &freed);
      if ((uint8_t *) f->upvalues[i] < flash_area) {
	all_readonly = FALSE;
      }
    }

    if (opts & OPT_UPVAL_VECTOR) {
      if (all_readonly && f->sizeupvalues && !is_flash(f->upvalues)) {
	TString **ro_upvalues = find_data(f->upvalues, f->sizeupvalues * sizeof(TString));
	if (ro_upvalues) {
	  luaM_freearray(L, f->upvalues, f->sizeupvalues, TString);
	  f->upvalues = ro_upvalues;
	  freed += f->sizeupvalues * sizeof(TString);
	}
      }
    }
  }

  all_readonly = TRUE;
  
  if (opts & OPT_LOCVARS) {
    for (i = 0; i < f->sizelocvars; i++) {
      TString *str = f->locvars[i].varname;

      f->locvars[i].varname = freeze_tstring(L, str, &freed);
      if ((uint8_t *) f->locvars[i].varname < flash_area) {
	all_readonly = FALSE;
      }
    }

    if (opts & OPT_LOCVAR_VECTOR) {
      if (all_readonly && f->sizelocvars && !is_flash(f->locvars)) {
	LocVar *ro_locvars = find_data(f->locvars, f->sizelocvars * sizeof(LocVar));
	if (ro_locvars) {
	  luaM_freearray(L, f->locvars, f->sizelocvars, LocVar);
	  f->locvars = ro_locvars;
	  freed += f->sizelocvars * sizeof(LocVar);
	}
      }
    }
  }

  if (opts & OPT_CONSTANTS) {
    all_readonly = TRUE;
    
    for (i = 0; i < f->sizek; i++) {
      TValue *val = &f->k[i];

      if (ttisstring(val)) {
	TString *frozen = freeze_tstring(L, rawtsvalue(val), &freed);
	if ((uint8_t *) frozen > flash_area) {
	  setsvalue(L, val, frozen);
	} else {
	  all_readonly = FALSE;
	}
      } else if (iscollectable(val)) {
	all_readonly = FALSE;
      }
    }

    if (opts & OPT_CONSTANT_VECTOR) {
      if (all_readonly && f->sizek && !is_flash(f->k)) {
	TValue *ro_k = find_data(f->k, f->sizek * sizeof(TValue));
	if (ro_k) {
	  luaM_freearray(L, f->k, f->sizek, TValue);
	  f->k = ro_k;
	  freed += f->sizek * sizeof(TValue);
	}
      }
    }
  }

#ifdef LUA_OPTIMIZE_DEBUG
  if (opts & OPT_LINEINFO) {
    if (f->packedlineinfo && !is_flash(f->packedlineinfo)) {
      int datalen = c_strlen(cast(char *, f->packedlineinfo))+1;
      unsigned char *packedlineinfo = (unsigned char *) find_data(f->packedlineinfo, datalen);
      if (packedlineinfo && (opts & OPT_WRITE)) {
	freed += datalen;
	luaM_freearray(L, f->packedlineinfo, datalen, unsigned char);
	f->packedlineinfo = packedlineinfo;
      }
    }
  }
#endif

  for (i = 0; i < f->sizep; i++) {
    freed += do_freeze_proto(L, f->p[i], depth + 1);
  }

  return freed;
}

static int do_freeze_closure(lua_State *L, Closure *cl) {
  if (cl->c.isC) {
    NODE_DBG("Skipping C Closure\n");
    return 0;
  }

  Proto *f = cl->l.p;

  // Now we have to freeze this block.....
  int result = do_freeze_proto(L, f, 0);

  if (!result) {
    return 0;
  }

  int i;
  UpVal **upval = cl->l.upvals;

  for (i = 0; i < cl->l.nupvalues; i++, upval++) {
    TValue *val = (*upval)->v;

    if (ttisfunction(val)) {
      Closure *inner = clvalue(val);

      result += do_freeze_closure(L, inner);
    }
  }

  return result;
}

// takes a function and returns the number of bytes of memory saved
static int freezer_freeze(lua_State *L) {
  if(!lua_isfunction(L, 1)) luaL_argerror(L, 1, "must be a Lua Function");
  Closure *cl = (Closure *) lua_topointer(L, 1);

  int result = do_freeze_closure(L, cl);

  lua_pushvalue(L, 1);
  lua_pushinteger(L, result);

  return 2;
}

static int freezer_opts(lua_State *L) {
  opts = luaL_checkinteger(L, 1);
  return 0;
}

static int freezer_defrost(lua_State *L) {
  needs_erase();
  return 0;
}

static int freezer_open(lua_State *L) {
 flash_area_phys = platform_flash_mapped2phys((uint32_t) flash_area); 

 if (!(flash_area[0] & ERASE_ON_NEXT_BOOT0) || !check_consistency()) {
   NODE_DBG("Resetting freezer area\n");
   int sector = -1;
   for (uint32_t offset = 0; offset < sizeof(flash_area); offset += 4096) {
     int sec = platform_flash_get_sector_of_address(flash_area_phys + offset);
     if (sec != sector) {
       NODE_DBG("Erasing sector %d\n", sec);
       platform_flash_erase_sector(sec);
       sector = sec;
     }
   }
 } else {
   uint8_t byte = flash_area[0];
   byte = byte & ~ERASED_ON_THIS_BOOT1;
   
   move_to_flash((uint8_t *) &flash_area[0], &byte, sizeof(byte));
 }

 return 0;
}

// Module function map

static const LUA_REG_TYPE freezer_map[] = {
  { LSTRKEY( "freeze" ),        LFUNCVAL( freezer_freeze ) },
  { LSTRKEY( "defrost" ),       LFUNCVAL( freezer_defrost ) },
  { LSTRKEY( "info" ),          LFUNCVAL( freezer_info ) },
  { LSTRKEY( "opts" ),          LFUNCVAL( freezer_opts ) },
  { LNILKEY, LNILVAL }
};

NODEMCU_MODULE(FREEZER, "freezer", freezer_map, freezer_open);
