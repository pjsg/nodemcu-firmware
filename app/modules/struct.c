/*
** {======================================================
** Library for packing/unpacking structures.
** $Id: struct.c,v 1.4 2012/07/04 18:54:29 roberto Exp $
** See Copyright Notice at the end of this file
** =======================================================
*/
// Original: http://www.inf.puc-rio.br/~roberto/struct/
// This was ported to NodeMCU by Philip Gladstone, N1DQ
/*
** Valid formats:
** > - big endian
** < - little endian
** ![num] - alignment
** x - pading
** b/B - signed/unsigned byte
** h/H - signed/unsigned short
** l/L - signed/unsigned long
** T   - size_t
** i/In - signed/unsigned integer with size `n' (default is size of int)
** cn - sequence of `n' chars (from/to a string); when packing, n==0 means
        the whole string; when unpacking, n==0 means use the previous
        read number as the string length
** s - zero-terminated string
** f - float
** d - double
** ' ' - ignored
*/


#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include "module.h"

#include "lauxlib.h"


/* basic integer type */
#if !defined(STRUCT_INT)
#define STRUCT_INT	long
#endif

typedef STRUCT_INT Inttype;

/* corresponding unsigned version */
typedef unsigned STRUCT_INT Uinttype;


/* maximum size (in bytes) for integral types */
#ifdef LUA_NUMBER_INTEGRAL
#ifdef LUA_INTEGRAL_LONGLONG
#define MAXINTSIZE	8
#else
#define MAXINTSIZE	4
#endif
#else
#define MAXINTSIZE	32
#endif

/* is 'x' a power of 2? */
#define isp2(x)		((x) > 0 && ((x) & ((x) - 1)) == 0)

/* dummy structure to get alignment requirements */
struct cD {
  char c;
  double d;
};


#define PADDING		(sizeof(struct cD) - sizeof(double))
#define MAXALIGN  	(PADDING > sizeof(int) ? PADDING : sizeof(int))


/* endian options */
#define BIG	0
#define LITTLE	1


static union {
  int dummy;
  char endian;
} const native = {1};


typedef struct Header {
  int endian;
  int align;
} Header;


static int getnum (const char **fmt, int df) {
  if (!isdigit(**fmt))  /* no number? */
    return df;  /* return default value */
  else {
    int a = 0;
    do {
      a = a*10 + *((*fmt)++) - '0';
    } while (isdigit(**fmt));
    return a;
  }
}


#define defaultoptions(h)	((h)->endian = native.endian, (h)->align = 1)



static size_t optsize (lua_State *L, char opt, const char **fmt) {
  switch (opt) {
    case 'B': case 'b': return sizeof(char);
    case 'H': case 'h': return sizeof(short);
    case 'L': case 'l': return sizeof(long);
    case 'T': return sizeof(size_t);
#ifndef LUA_NUMBER_INTEGRAL
    case 'f':  return sizeof(float);
    case 'd':  return sizeof(double);
#endif
    case 'x': return 1;
    case 'c': return getnum(fmt, 1);
    case 'i': case 'I': {
      int sz = getnum(fmt, sizeof(int));
      if (sz > MAXINTSIZE)
        luaL_error(L, "integral size %d is larger than limit of %d",
                       sz, MAXINTSIZE);
      return sz;
    }
    default: return 0;  /* other cases do not need alignment */
  }
}


/*
** return number of bytes needed to align an element of size 'size'
** at current position 'len'
*/
static int gettoalign (size_t len, Header *h, int opt, size_t size) {
  if (size == 0 || opt == 'c') return 0;
  if (size > (size_t)h->align)
    size = h->align;  /* respect max. alignment */
  return (size - (len & (size - 1))) & (size - 1);
}


/*
** options to control endianess and alignment
*/
static void controloptions (lua_State *L, int opt, const char **fmt,
                            Header *h) {
  switch (opt) {
    case  ' ': return;  /* ignore white spaces */
    case '>': h->endian = BIG; return;
    case '<': h->endian = LITTLE; return;
    case '!': {
      int a = getnum(fmt, MAXALIGN);
      if (!isp2(a))
        luaL_error(L, "alignment %d is not a power of 2", a);
      h->align = a;
      return;
    }
    default: {
      const char *msg = lua_pushfstring(L, "invalid format option '%c'", opt);
      luaL_argerror(L, 1, msg);
    }
  }
}


static void putinteger (lua_State *L, luaL_Buffer *b, int arg, int endian,
                        int size) {
  int32_t n = luaL_checkinteger(L, arg);
  Uinttype value;
  char buff[MAXINTSIZE];
  if (n < 0)
    value = (Uinttype)(Inttype)n;
  else
    value = (Uinttype)n;
  if (endian == LITTLE) {
    int i;
    for (i = 0; i < size; i++) {
      buff[i] = (value & 0xff);
      value >>= 8;
    }
  }
  else {
    int i;
    for (i = size - 1; i >= 0; i--) {
      buff[i] = (value & 0xff);
      value >>= 8;
    }
  }
  luaL_addlstring(b, buff, size);
}


static void correctbytes (char *b, int size, int endian) {
  if (endian != native.endian) {
    int i = 0;
    while (i < --size) {
      char temp = b[i];
      b[i++] = b[size];
      b[size] = temp;
    }
  }
}


static int b_pack (lua_State *L) {
  luaL_Buffer b;
  const char *fmt = luaL_checkstring(L, 1);
  Header h;
  int arg = 2;
  size_t totalsize = 0;
  defaultoptions(&h);
  lua_pushnil(L);  /* mark to separate arguments from string buffer */
  luaL_buffinit(L, &b);
  while (*fmt != '\0') {
    int opt = *fmt++;
    size_t size = optsize(L, opt, &fmt);
    int toalign = gettoalign(totalsize, &h, opt, size);
    totalsize += toalign;
    while (toalign-- > 0) luaL_addchar(&b, '\0');
    switch (opt) {
      case 'b': case 'B': case 'h': case 'H':
      case 'l': case 'L': case 'T': case 'i': case 'I': {  /* integer types */
        putinteger(L, &b, arg++, h.endian, size);
        break;
      }
      case 'x': {
        luaL_addchar(&b, '\0');
        break;
      }
#ifndef LUA_NUMBER_INTEGRAL
      case 'f': {
        float f = (float)luaL_checknumber(L, arg++);
        correctbytes((char *)&f, size, h.endian);
        luaL_addlstring(&b, (char *)&f, size);
        break;
      }
      case 'd': {
        double d = luaL_checknumber(L, arg++);
        correctbytes((char *)&d, size, h.endian);
        luaL_addlstring(&b, (char *)&d, size);
        break;
      }
#endif
      case 'c': case 's': {
        size_t l;
        const char *s = luaL_checklstring(L, arg++, &l);
        if (size == 0) size = l;
        luaL_argcheck(L, l >= (size_t)size, arg, "string too short");
        luaL_addlstring(&b, s, size);
        if (opt == 's') {
          luaL_addchar(&b, '\0');  /* add zero at the end */
          size++;
        }
        break;
      }
      default: controloptions(L, opt, &fmt, &h);
    }
    totalsize += size;
  }
  luaL_pushresult(&b);
  return 1;
}


static int64_t getinteger (const char *buff, int endian,
                        int issigned, int size) {
  uint64_t l = 0;
  int i;
  if (endian == BIG) {
    for (i = 0; i < size; i++) {
      l <<= 8;
      l |= (unsigned char)buff[i];
    }
  }
  else {
    for (i = size - 1; i >= 0; i--) {
      l <<= 8;
      l |= (unsigned char)buff[i];
    }
  }
  if (!issigned)
    return (int64_t)l;
  else {  /* signed format */
    uint64_t mask = (uint64_t)(~((uint64_t)0)) << (size*8 - 1);
    if (l & mask)  /* negative value? */
      l |= mask;  /* signal extension */
    return (int64_t)l;
  }
}


static int b_unpack (lua_State *L) {
  Header h;
  const char *fmt = luaL_checkstring(L, 1);
  size_t ld;
  const char *data = luaL_checklstring(L, 2, &ld);
  size_t pos = luaL_optinteger(L, 3, 1) - 1;
  defaultoptions(&h);
  lua_settop(L, 2);
  while (*fmt) {
    int opt = *fmt++;
    size_t size = optsize(L, opt, &fmt);
    pos += gettoalign(pos, &h, opt, size);
    luaL_argcheck(L, pos+size <= ld, 2, "data string too short");
    luaL_checkstack(L, 2, "too many results");
    switch (opt) {
      case 'b': case 'B': case 'h': case 'H':
      case 'l': case 'L': case 'T': case 'i':  case 'I': {  /* integer types */
        int issigned = islower(opt);
        int64_t res = getinteger(data+pos, h.endian, issigned, size);
        if (res >= INT_MIN && res <= INT_MAX) {
          lua_pushinteger(L, res);
        } else {
          lua_pushnumber(L, res);
        }
        break;
      }
      case 'x': {
        break;
      }
#ifndef LUA_NUMBER_INTEGRAL
      case 'f': {
        float f;
        memcpy(&f, data+pos, size);
        correctbytes((char *)&f, sizeof(f), h.endian);
        lua_pushnumber(L, f);
        break;
      }
      case 'd': {
        double d;
        memcpy(&d, data+pos, size);
        correctbytes((char *)&d, sizeof(d), h.endian);
        lua_pushnumber(L, d);
        break;
      }
#endif
      case 'c': {
        if (size == 0) {
          if (!lua_isnumber(L, -1))
            luaL_error(L, "format `c0' needs a previous size");
          size = lua_tointeger(L, -1);
          lua_pop(L, 1);
          luaL_argcheck(L, pos+size <= ld, 2, "data string too short");
        }
        lua_pushlstring(L, data+pos, size);
        break;
      }
      case 's': {
        const char *e = (const char *)memchr(data+pos, '\0', ld - pos);
        if (e == NULL)
          luaL_error(L, "unfinished string in data");
        size = (e - (data+pos)) + 1;
        lua_pushlstring(L, data+pos, size - 1);
        break;
      }
      default: controloptions(L, opt, &fmt, &h);
    }
    pos += size;
  }
  lua_pushinteger(L, pos + 1);
  return lua_gettop(L) - 2;
}


static int b_size (lua_State *L) {
  Header h;
  const char *fmt = luaL_checkstring(L, 1);
  size_t pos = 0;
  defaultoptions(&h);
  while (*fmt) {
    int opt = *fmt++;
    size_t size = optsize(L, opt, &fmt);
    pos += gettoalign(pos, &h, opt, size);
    if (opt == 's')
      luaL_argerror(L, 1, "option 's' has no fixed size");
    else if (opt == 'c' && size == 0)
      luaL_argerror(L, 1, "option 'c0' has no fixed size");
    if (!isalnum(opt))
      controloptions(L, opt, &fmt, &h);
    pos += size;
  }
  lua_pushinteger(L, pos);
  return 1;
}

/* }====================================================== */


LROT_BEGIN(thislib, NULL, 0)
  LROT_FUNCENTRY( pack, b_pack )
  LROT_FUNCENTRY( unpack, b_unpack )
  LROT_FUNCENTRY( size, b_size )
LROT_END(thislib, NULL, 0)



NODEMCU_MODULE(STRUCT, "struct", thislib, NULL);

/******************************************************************************
* Copyright (C) 2010-2012 Lua.org, PUC-Rio.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/

