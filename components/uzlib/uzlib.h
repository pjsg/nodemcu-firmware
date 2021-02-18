/*
 * uzlib  -  tiny deflate/inflate library (deflate, gzip, zlib)
 *
 * Copyright (c) 2003 by Joergen Ibsen / Jibz
 * All Rights Reserved
 * http://www.ibsensoftware.com/
 *
 * Copyright (c) 2014-2016 by Paul Sokolovsky
 */

#ifndef UZLIB_INFLATE_H
#define UZLIB_INFLATE_H

#include <setjmp.h>

#include <stdint.h>
#include <stdlib.h>
#define uz_malloc malloc
#define uz_free free

#if defined(__XTENSA__) || defined(CONFIG_IDF_TARGET_ESP32C3)

#define UZLIB_THROW(v) longjmp(unwindAddr, (v))
#define UZLIB_SETJMP setjmp

#else  /* Host */

extern int dbg_break(void);
#if defined(_MSC_VER) || defined(__MINGW32__) //msvc requires old name for longjmp
#define UZLIB_THROW(v) {dbg_break();longjmp(unwindAddr, (v));}
#define UZLIB_SETJMP(n) setjmp(n)
#else
#define UZLIB_THROW(v) {dbg_break();_longjmp(unwindAddr, (v));}
#define UZLIB_SETJMP(n) _setjmp(n)
#endif

#endif /* defined(__XTENSA__) */

extern jmp_buf unwindAddr;

/* ok status, more data produced */
#define UZLIB_OK             0
/* end of compressed stream reached */
#define UZLIB_DONE           1
#define UZLIB_DATA_ERROR    (-3)
#define UZLIB_CHKSUM_ERROR  (-4)
#define UZLIB_DICT_ERROR    (-5)
#define UZLIB_MEMORY_ERROR  (-6)

/* checksum types */
#define UZLIB_CHKSUM_NONE  0
#define UZLIB_CHKSUM_ADLER 1
#define UZLIB_CHKSUM_CRC   2

/* Gzip header codes */
#define UZLIB_FTEXT    1
#define UZLIB_FHCRC    2
#define UZLIB_FEXTRA   4
#define UZLIB_FNAME    8
#define UZLIB_FCOMMENT 16

/* Compression API */

typedef struct uzlib_data UZLIB_DATA;

int uzlib_inflate (uint8_t (*)(void), void (*)(uint8_t),
                   uint8_t (*)(uint32_t), uint32_t len, uint32_t *crc, void **state);

int uzlib_compress (uint8_t **dest, uint32_t *destLen,
                    const uint8_t *src, uint32_t srcLen);

/* Checksum API */
/* crc is previous value for incremental computation, 0xffffffff initially */
uint32_t uzlib_crc32(const void *data, uint32_t length, uint32_t crc);

#endif /* UZLIB_INFLATE_H */
