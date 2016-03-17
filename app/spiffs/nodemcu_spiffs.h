#include "c_stdint.h"
#include "c_stddef.h"
#include "c_stdio.h"
#include "user_interface.h"

// Turn off stats
#define SPIFFS_CACHE_STATS 	    0
#define SPIFFS_GC_STATS             0

// Enable magic so we can find the file system (but not yet)
//#define SPIFFS_USE_MAGIC            1
//#define SPIFFS_USE_MAGIC_LENGTH     1

// Reduce the chance of returning disk full
#define SPIFFS_GC_MAX_RUNS          256

