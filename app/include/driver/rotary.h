/*
 * Definitions to access the Rotary driver
 */
#ifndef __ROTARY_H__
#define __ROTARY_H__

#include "c_types.h"

#define ROTARY_CHANNEL_COUNT	3

int rotary_setup(uint32_t channel, int phaseA, int phaseB, int press, int tasknumber);

int32_t rotary_getevent(uint32_t channel);

int rotary_close(uint32_t channel);

#endif
