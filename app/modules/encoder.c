// Module encoders

#include "module.h"
#include "lauxlib.h"
#include "lmem.h"
#include <string.h>
#define BASE64_INVALID '\xff'
#define BASE64_PADDING '='
#define ISBASE64(c) (unbytes64[c] != BASE64_INVALID)

static const uint8 b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint8 *toBase64 ( lua_State* L, const uint8 *msg, size_t *len){
  size_t i, n = *len;

  if (!n)  // handle empty string case
    return NULL;

  int buf_size = (n + 2) / 3 * 4; // estimated encoded size
  uint8 * q, *out = (uint8 *)luaM_malloc(L, buf_size);
  uint8 bytes64[sizeof(b64)];
  memcpy(bytes64, b64, sizeof(b64));   //Avoid lots of flash unaligned fetches

  for (i = 0, q = out; i < n; i += 3) {
    int a = msg[i];
    int b = (i + 1 < n) ? msg[i + 1] : 0;
    int c = (i + 2 < n) ? msg[i + 2] : 0;
    *q++ = bytes64[a >> 2];
    *q++ = bytes64[((a & 3) << 4) | (b >> 4)];
    *q++ = (i + 1 < n) ? bytes64[((b & 15) << 2) | (c >> 6)] : BASE64_PADDING;
    *q++ = (i + 2 < n) ? bytes64[(c & 63)] : BASE64_PADDING;
  }
  *len = q - out;
  out = luaM_realloc_(L, out, buf_size, *len); //reallocate to actual encoded length
  return out;
}

static uint8 *fromBase64 ( lua_State* L, const uint8 *enc_msg, size_t *len){
  int i, n = *len, blocks = (n>>2), pad = 0;
  const uint8 *p;
  uint8 unbytes64[UCHAR_MAX+1], *msg, *q;

  if (!n)  // handle empty string case
    return NULL;

  if (n & 3)
    luaL_error (L, "Invalid base64 string");

  memset(unbytes64, BASE64_INVALID, sizeof(unbytes64));
  for (i = 0; i < sizeof(b64)-1; i++) unbytes64[b64[i]] = i;  // sequential so no exceptions

  if (enc_msg[n-1] == BASE64_PADDING) {
    pad =  (enc_msg[n-2] != BASE64_PADDING) ? 1 : 2;
    blocks--;  //exclude padding block
  }

 for (i = 0; i < n - pad; i++) if (!ISBASE64(enc_msg[i])) luaL_error (L, "Invalid base64 string");
  unbytes64[BASE64_PADDING] = 0;

  int buf_size=1+ (3 * n / 4); // estimate decoded length
  msg = q = (uint8 *) luaM_malloc(L, buf_size);
  for (i = 0, p = enc_msg; i<blocks; i++)  {
    uint8 a = unbytes64[*p++];
    uint8 b = unbytes64[*p++];
    uint8 c = unbytes64[*p++];
    uint8 d = unbytes64[*p++];
    *q++ = (a << 2) | (b >> 4);
    *q++ = (b << 4) | (c >> 2);
    *q++ = (c << 6) | d;
  }

  if (pad) { //now process padding block bytes
    uint8 a = unbytes64[*p++];
    uint8 b = unbytes64[*p++];
    *q++ = (a << 2) | (b >> 4);
    if (pad == 1) *q++ = (b << 4) | (unbytes64[*p] >> 2);
  }
  *len = q - msg;
  msg = luaM_realloc_(L, msg, buf_size, *len); //reallocate to actual decoded length
  return msg;
}

static inline uint8 to_hex_nibble(uint8 b) {
  return b + ( b < 10 ? '0' : 'a' - 10 );
  }

static uint8 *toHex ( lua_State* L, const uint8 *msg, size_t *len){
  int i, n = *len;
  *len <<= 1;
  uint8 *q, *out = (uint8 *)luaM_malloc(L, *len);
  for (i = 0, q = out; i < n; i++) {
    *q++ = to_hex_nibble(msg[i] >> 4);
    *q++ = to_hex_nibble(msg[i] & 0xf);
  }
  return out;
}

static uint8 *fromHex ( lua_State* L, const uint8 *msg, size_t *len){
  int i, n = *len;
  const uint8 *p;

  if (n &1)
    luaL_error (L, "Invalid hex string");

  *len >>= 1;
  uint8 b, *q, *out = (uint8 *)luaM_malloc(L, *len);
  uint8 c = 0;

  for (i = 0, p = msg, q = out; i < n; i++) {
     if (*p >= '0' && *p <= '9') {
       b = *p++ - '0';
     } else if (*p >= 'a' && *p <= 'f') {
       b = *p++ - ('a' - 10);
     } else if (*p >= 'A' && *p <= 'F') {
       b = *p++ - ('A' - 10);
     } else {
       luaN_freearray(L, out, *len);
       luaL_error (L, "Invalid hex string");
     }
     if ((i&1) == 0) {
       c = b<<4;
     } else {
       *q++ = c+ b;
     }
  }
  return out;
}

// All encoder functions are of the form:
// Lua:  output_string = encoder.function(input_string)
// Where input string maybe empty, but not nil
// Hence these all call the do_func wrapper
static int do_func (lua_State *L, uint8 * (*conv_func)(lua_State *, const uint8 *, size_t *)) {
  size_t len;
  const uint8 *input = luaL_checklstring(L, 1, &len);
  uint8 *output = conv_func(L, input, &len);

  if (output) {
    lua_pushlstring(L, output, len);
    luaN_freearray(L, output, len);
  } else {
    lua_pushstring(L, "");
  }
  return 1;
}

#define DECLARE_FUNCTION(f) static int encoder_ ## f (lua_State *L) \
{ return do_func(L, f); }

  DECLARE_FUNCTION(fromBase64);
  DECLARE_FUNCTION(toBase64);
  DECLARE_FUNCTION(fromHex);
  DECLARE_FUNCTION(toHex);

// Module function map
LROT_BEGIN(encoder, NULL, 0)
  LROT_FUNCENTRY( fromBase64, encoder_fromBase64 )
  LROT_FUNCENTRY( toBase64, encoder_toBase64 )
  LROT_FUNCENTRY( fromHex, encoder_fromHex )
  LROT_FUNCENTRY( toHex, encoder_toHex )
LROT_END(encoder, NULL, 0)


NODEMCU_MODULE(ENCODER, "encoder", encoder, NULL);
