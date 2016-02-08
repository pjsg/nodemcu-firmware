/*
 * Definitions to access the Rotary driver
 */
#ifndef __ROTARY_H__
#define __ROTARY_H__

#include "c_types.h"

#define ROTARY_CHANNEL_COUNT	3

#define ROTARY_DEBUG 1

int rotary_setup(uint32_t channel, int phaseA, int phaseB, int press, task_handle_t tasknumber);

int32_t rotary_getevent(uint32_t channel);

size_t rotary_getstate(uint32_t channel, int32_t *buffer, size_t maxlen);

int rotary_close(uint32_t channel);

#endif
