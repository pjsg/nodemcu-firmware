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

#include "platform.h"
#include "c_types.h"
#include "../libc/c_stdlib.h"
#include "driver/switec.h"

#define N_STATES 6

// State  3 2 1 0   Value
// 0      1 0 0 1   0x9
// 1      0 0 0 1   0x1
// 2      0 1 1 1   0x7
// 3      0 1 1 0   0x6
// 4      1 1 1 0   0xE
// 5      1 0 0 0   0x8
static const uint8_t stateMap[N_STATES] = {0x9, 0x1, 0x7, 0x6, 0xE, 0x8};

typedef struct {
  uint8_t  currentState;
  uint8_t  stopped;
  int8_t   dir;
  uint32_t mask;
  uint32_t pinstate[N_STATES];
  uint32_t nextTime;
  int16_t  targetStep;
  int16_t  currentStep;
  uint16_t vel;
  uint16_t maxVel;
} DATA;

static DATA *data[2];
static volatile char timerActive;

#define MAXVEL 300

static const uint16_t accelTable[][2] = {
    {   20, 3000},
    {   50, 1500},
    {  100, 1000},
    {  150,  800},
    {  MAXVEL,  600}
};

// Just takes the channel number
int switec_close(uint32_t channel) 
{
  if (channel >= sizeof(data) / sizeof(data[0])) {
    return -1;
  }

  DATA *d = data[channel];

  if (!d) {
    return 0;
  }

  if (!d->stopped) {
    return -1;
  }

  // Set pins as input
  gpio_output_set(0, 0, 0, d->mask);

  data[channel] = NULL;
  c_free(d);

  return 0;
}

static inline void writeIO(DATA *d) 
{
  uint32_t pinState = d->pinstate[d->currentState];

  gpio_output_set(pinState, d->mask & ~pinState, 0, 0);
}

static inline void stepUp(DATA *d) 
{
  d->currentStep++;
  d->currentState = (d->currentState + 1) % N_STATES;
  writeIO(d);
}

static inline void stepDown(DATA *d) 
{
  d->currentStep--;
  d->currentState = (d->currentState + N_STATES - 1) % N_STATES;
  writeIO(d);
}

static void timer_interrupt(void)
{
  // This function really is running at interrupt level with everything
  // else masked off. It should take as little time as necessary.
  //

  int i;
  uint32_t delay = 0xffffffff;
  for (i = 0; i < sizeof(data) / sizeof(data[0]); i++) {
    DATA *d = data[i];
    if (!d || d->stopped) {
      continue;
    }

    uint32_t now = system_get_time();
    if (now < d->nextTime) {
      int needToWait = d->nextTime - now;
      if (needToWait < delay) {
	delay = needToWait;
      }
      continue;
    }

    // Are we done yet?
    if (d->currentStep == d->targetStep && d->vel == 0) {
      d->stopped = 1;
      d->dir = 0;
      // TODO: We need to post a message to say that the motion is complete
      continue;
    }

    // if stopped, determine direction
    if (d->vel == 0) {
      d->dir = d->currentStep < d->targetStep ? 1 : -1;
      // do not set to 0 or it could go negative in case 2 below
      d->vel = 1; 
    }
    
    if (d->dir > 0) {
      stepUp(d);
    } else {
      stepDown(d);
    }

    // determine delta, number of steps in current direction to target.
    // may be negative if we are headed away from target
    int delta = d->dir > 0 ? d->targetStep - d->currentStep : d->currentStep - d->targetStep;
    
    if (delta > 0) {
      // case 1 : moving towards target (maybe under accel or decel)
      if (delta <= d->vel) {
	// time to declerate
	d->vel--;
      } else if (d->vel < d->maxVel) {
	// accelerating
	d->vel++;
      } else {
	// at full speed - stay there
      }
    } else {
      // case 2 : at or moving away from target (slow down!)
      d->vel--;
    }
    
    // vel now defines delay
    uint8_t row = 0;
    // this is why vel must not be greater than the last vel in the table.
    while (accelTable[row][0] < d->vel) {
      row++;
    }
    int32_t microDelay = accelTable[row][1];
    d->nextTime = d->nextTime + microDelay;
    if (d->nextTime < now) {
      d->nextTime = now + microDelay;
    }

    int needToWait = d->nextTime - now;
    if (needToWait < delay) {
      delay = needToWait;
    }
  } 

  if (delay < 1000000) {
    if (delay < 50) {
      delay = 50;
    }
    timerActive = 1;
    hw_timer_arm(delay);
  } else {
    timerActive = 0;
  }
}


// The pin numbers are actual platform GPIO numbers
int switec_setup(uint32_t channel, int *pin )
{
  if (channel >= sizeof(data) / sizeof(data[0])) {
    return -1;
  }

  if (data[channel]) {
    if (switec_close(channel)) {
      return -1;
    }
  }

  if (!data[0] && !data[1]) {
    // NMI with no autoreload
    hw_timer_init(0 /*FRC1_SOURCE*/, 0);
    hw_timer_set_func(timer_interrupt);
  }

  DATA *d = (DATA *) c_zalloc(sizeof(DATA));
  if (!d) {
    return -1;
  }

  data[channel] = d;
  int i;

  for (i = 0; i < 4; i++) {
    d->mask |= 1 << pin[i];

    int j;
    for (j = 0; j < N_STATES; j++) {
      if (stateMap[j] & (1 << i)) {
        d->pinstate[j] |= 1 << pin[i];
      }
    }
  }

  d->maxVel = MAXVEL;

  // Set all pins as outputs
  gpio_output_set(0, 0, d->mask, 0);

  return 0;
}

// All this does is to assert that the current position is 0
int switec_reset(uint32_t channel)
{
  if (channel >= sizeof(data) / sizeof(data[0])) {
    return -1;
  }

  DATA *d = data[channel];

  if (!d || !d->stopped) {
    return -1;
  }

  d->currentStep = d->targetStep = 0;

  return 0;
}

// Just takes the channel number and the position
int switec_moveto(uint32_t channel, int pos)
{
  if (channel >= sizeof(data) / sizeof(data[0])) {
    return -1;
  }

  DATA *d = data[channel];

  if (!d) {
    return -1;
  }

  if (pos < 0) {
    // This ensures that we don't slam into the endstop
    d->maxVel = 50;
  } else {
    d->maxVel = MAXVEL;
  }

  d->targetStep = pos;
  if (d->stopped) {
    // reset the timer to avoid possible time overflow giving spurious deltas
    d->nextTime = system_get_time() + 1000;
    d->stopped = false;

    if (!timerActive) {
      timer_interrupt();
    }
  }

  return 0;  
}

int switec_getpos(uint32_t channel, int32_t *pos, int32_t *dir) 
{
  if (channel >= sizeof(data) / sizeof(data[0])) {
    return -1;
  }

  DATA *d = data[channel];

  if (!d) {
    return -1;
  }

  *pos = d->currentStep;
  *dir = d->stopped ? 0 : d->dir;

  return 0;
}
