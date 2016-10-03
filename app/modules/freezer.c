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

// First 32 bits of the flash area contain flags
// flash_area[0]:
//     1:    0 -- erase on next boot
//     2:    1 -- erased on this boot
//
#define ERASE_ON_NEXT_BOOT0 	1
#define ERASED_ON_THIS_BOOT1	2

#ifndef FREEZER_FLASH_AREA_SIZE
#define FREEZER_FLASH_AREA_SIZE   65536
#endif

volatile uint8_t freezer_flash_area[(FREEZER_FLASH_AREA_SIZE)/4096 * 4096] __attribute__ ((aligned(4096), section(".text.freezer")));
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

typedef struct _pending_item {
  void *item;
  struct _pending_item *next;
} PENDING_ITEM;

typedef struct {
  struct _pending_item *current;	// the last element got with _get
  struct _pending_item *head;
} PENDING_LIST;

// this eliminates duplicates
static void pending_push(PENDING_LIST *list, void *item) {
  PENDING_ITEM **pp;

  for (pp = &(list->head); *pp; pp = &((*pp)->next)) {
    if ((*pp)->item == item) {
      return;
    }
  }

  *pp = (PENDING_ITEM *) c_malloc(sizeof(PENDING_ITEM));
  if (!*pp) {
    return;
  }
  (*pp)->item = item;
  (*pp)->next = NULL;
}

static void* pending_get(PENDING_LIST *list) {
  if (list->current == NULL) {
    list->current = list->head;
  } else {
    if (!list->current->next) {
      return NULL;
    }
    list->current = list->current->next;
  }

  if (list->current) {
    return list->current->item;
  }

  return NULL;
}

static void pending_free(PENDING_LIST *list) {
  PENDING_ITEM *ptr = list->head;
  list->current = NULL;

  while (ptr) {
    PENDING_ITEM *next = ptr->next;
    c_free(ptr);
    ptr = next;
  }
}

static inline bool is_flash(void *ptr) {
  return (uint8_t *) ptr >= freezer_flash_area && (uint8_t *) ptr < freezer_flash_area + sizeof(freezer_flash_area);
}

static void move_to_flash(uint8_t *dest, const uint8_t *src, size_t len) {
  if (len > 0) {
    lua_assert(dest >= freezer_flash_area && dest + len < freezer_flash_area + sizeof(freezer_flash_area));
    platform_flash_write(src, dest - freezer_flash_area + flash_area_phys, len);
  }
}

static void needs_erase() {
  uint8_t byte = freezer_flash_area[0];

  byte &= ~ERASE_ON_NEXT_BOOT0;

  move_to_flash((uint8_t *) &freezer_flash_area[0], &byte, sizeof(byte));
}

/* Look for the data block defined by both data pointers concatenated. The first
 * length must not be zero unless both are.
 */
static void *find_data(const void *src, size_t len, const void *src2, size_t len2) {
  if (!len) {
    return (void *) (freezer_flash_area + 4);
  }
  size_t blen = (len + len2 + 7) & ~7;

  if (opts & OPT_DEBUG) {
    NODE_DBG("Looking for %d bytes (blocklen %d): ", len + len2, blen);
    int i;
    for (i = 0; i < len && i < 24; i++) {
      NODE_DBG("%02x ", ((unsigned char *) src)[i]);
    }
    for (; i < len + len2 && i < 24; i++) {
      NODE_DBG("%02x ", ((unsigned char *) src2)[i - len]);
    }
    NODE_DBG("\n");
  }

  int32_t *ptr = ((int32_t *) freezer_flash_area) + 1;

  while (*ptr > 0) {
    int blocklen = *ptr;
    if (blocklen == blen) {
      if (memcmp(ptr + 1, src, len) == 0
	  && (len2 == 0 || memcmp((char *) (ptr + 1) + len, src2, len2) == 0)) {

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

  if (((uint8_t *) ptr) + 8 + blen > freezer_flash_area + sizeof(freezer_flash_area)) {
    // doesn't fit
    if (freezer_flash_area[0] & ERASED_ON_THIS_BOOT1) {
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
  move_to_flash((uint8_t *) ptr + len, src2, len2);
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
    NODE_DBG("0x%x, %d: BLK differs\n", ptr, len + len2);
  }
  if (len2 > 0 && memcmp((uint8_t *) ptr + len, src2, len2) != 0) {
    NODE_DBG("0x%x, %d: BLK(2) differs\n", ptr, len + len2);
  }
  if (blockend[-1] != blen) {
    NODE_DBG("0x%x: TRL %d != %d\n", blockend - 1, blockend[-1], blen);
  }

  return (void *) ptr;
}

static bool check_consistency() {
  int32_t *ptr = ((int32_t *) freezer_flash_area) + 1;

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
  int32_t *ptr = ((int32_t *) freezer_flash_area) + 1;

  while (*ptr > 0) {
    int blocklen = *ptr;

    ptr = (uint32_t *) (((uint8_t *) ptr) + blocklen + 4);
    if (*ptr != blocklen) {
      needs_erase();
      break;
    }
    ptr++;
  }

  lua_pushnumber(L, freezer_flash_area[0]);
  lua_pushnumber(L, ((uint8_t *) ptr) - freezer_flash_area);
  lua_pushnumber(L, sizeof(freezer_flash_area));
  lua_pushnumber(L, check_consistency());

  return 4;
}

static TString *freeze_tstring(lua_State *L, TString *s, size_t *freedp) {
  if ((uint8_t *) s >= freezer_flash_area) {
    return s;
  }

  NODE_DBG_OPT("Freezing string '%s'\n", getstr(s));

  // Make a new readonly TString object
  TString tstr = *s;

  tstr.tsv.next = NULL;
  tstr.tsv.marked &= READONLYMASK;
  ((unsigned short *) &tstr)[3] = -1;  // Zap unused field

  size_t len = sizestring(&s->tsv);

  TString *new_tstr = find_data(&tstr, sizeof(tstr), s + 1, len - sizeof(tstr));

  if (!new_tstr) {
    return s;
  }

  *freedp += len;

  if (!(opts & OPT_WRITE)) {
    return s;
  }

  return new_tstr;
}

static int do_freeze_proto(lua_State *L, Proto *f) {
  int i;

  PENDING_LIST protos;
  memset(&protos, 0, sizeof(protos));

  pending_push(&protos, f);

  size_t freed = 0;

  while (f = (Proto *) pending_get(&protos)) {
    if (!f || is_flash(f->code) || is_flash(f->source)) {
      NODE_DBG_OPT("Early exit proto=0x%x\n", f);
      continue;
    }

    if (opts & OPT_SOURCE) {
      if (f->source) {
	f->source = freeze_tstring(L, f->source, &freed);
      }
    }

    if (opts & OPT_CODE) {
      Instruction *newcode = (Instruction *) find_data(f->code, sizeof(f->code[0]) * f->sizecode, NULL, 0);
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
	if ((uint8_t *) f->upvalues[i] < freezer_flash_area) {
	  all_readonly = FALSE;
	}
      }

      if (opts & OPT_UPVAL_VECTOR) {
	if (all_readonly && f->sizeupvalues && !is_flash(f->upvalues)) {
	  TString **ro_upvalues = find_data(f->upvalues, f->sizeupvalues * sizeof(TString), NULL, 0);
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
	if ((uint8_t *) f->locvars[i].varname < freezer_flash_area) {
	  all_readonly = FALSE;
	}
      }

      if (opts & OPT_LOCVAR_VECTOR) {
	if (all_readonly && f->sizelocvars && !is_flash(f->locvars)) {
	  LocVar *ro_locvars = find_data(f->locvars, f->sizelocvars * sizeof(LocVar), NULL, 0);
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
	  if ((uint8_t *) frozen > freezer_flash_area) {
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
	  TValue *ro_k = find_data(f->k, f->sizek * sizeof(TValue), NULL, 0);
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
	unsigned char *packedlineinfo = (unsigned char *) find_data(f->packedlineinfo, datalen, NULL, 0);
	if (packedlineinfo && (opts & OPT_WRITE)) {
	  freed += datalen;
	  luaM_freearray(L, f->packedlineinfo, datalen, unsigned char);
	  f->packedlineinfo = packedlineinfo;
	}
      }
    }
#endif

    for (i = 0; i < f->sizep; i++) {
      pending_push(&protos, f->p[i]);
    }
  }

  pending_free(&protos);

  return freed;
}

static int do_freeze_closure(lua_State *L, Closure *cl) {
  PENDING_LIST closures;
  memset(&closures, 0, sizeof(closures));

  pending_push(&closures, cl);
  int result = 0;

  while (cl = (Closure *) pending_get(&closures)) {
    if (cl->c.isC) {
      NODE_DBG("Skipping C Closure\n");
      continue;
    }

    Proto *f = cl->l.p;

    // Now we have to freeze this block.....
    result += do_freeze_proto(L, f);

    int i;
    UpVal **upval = cl->l.upvals;

    for (i = 0; i < cl->l.nupvalues; i++, upval++) {
      TValue *val = (*upval)->v;

      if (ttisfunction(val)) {
	Closure *inner = clvalue(val);

	pending_push(&closures, inner);
      }
    }
  }

  pending_free(&closures);

  return result;
}

// takes a function and returns the number of bytes of memory saved
static int freezer_freeze(lua_State *L) {
  if (!lua_isfunction(L, 1)) {
    lua_pushvalue(L, 1);

    return 1;
  }
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
 flash_area_phys = platform_flash_mapped2phys((uint32_t) freezer_flash_area); 

 if (!(freezer_flash_area[0] & ERASE_ON_NEXT_BOOT0) || !check_consistency()) {
   NODE_DBG("Resetting freezer area\n");
   int sector = -1;
   for (uint32_t offset = 0; offset < sizeof(freezer_flash_area); offset += 4096) {
     int sec = platform_flash_get_sector_of_address(flash_area_phys + offset);
     if (sec != sector) {
       NODE_DBG("Erasing sector %d\n", sec);
       platform_flash_erase_sector(sec);
       sector = sec;
     }
   }
 } else {
   uint8_t byte = freezer_flash_area[0];
   byte = byte & ~ERASED_ON_THIS_BOOT1;
   
   move_to_flash((uint8_t *) &freezer_flash_area[0], &byte, sizeof(byte));
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
