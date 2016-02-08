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
#include "../libc/c_stdio.h"
#include "driver/switec.h"
#include "ets_sys.h"
#include "os_type.h"
#include "osapi.h"
#include "user_interface.h"

#define N_STATES 6
//
// First pin passed to setup corresponds to bit 3
// On the motor, the pins are arranged
//
//    4           1
//
//    3           2
//
// The direction of rotation can be reversed by reordering the pins
//
// State  3 2 1 0  A B  Value
// 0      1 0 0 1  - -  0x9
// 1      0 0 0 1  . -  0x1
// 2      0 1 1 1  + .  0x7
// 3      0 1 1 0  + +  0x6
// 4      1 1 1 0  . +  0xE
// 5      1 0 0 0  - .  0x8
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
  uint16_t minDelay;
} DATA;

static DATA *data[SWITEC_CHANNEL_COUNT];
static volatile char timerActive;

#define MAXVEL 255

// Note that this has to be global so that the compiler does not
// put it into ROM.
uint8_t switec_accel_table[][2] = {
    {   20, 3000 >> 4},
    {   50, 1500 >> 4},
    {  100, 1000 >> 4},
    {  150,  800 >> 4},
    {  MAXVEL,  600 >> 4}
};

static void ICACHE_RAM_ATTR timer_interrupt(void *);

//-----
// The following code is heavily copied from the Espressif sample hw_timer.c
//-----

/******************************************************************************
* Copyright 2013-2014 Espressif Systems (Wuxi)
*
* FileName: hw_timer.c
*
* Description: hw_timer driver
*
* Modification history:
*     2014/5/1, v1.0 create this file.
*******************************************************************************/

#define US_TO_RTC_TIMER_TICKS(t)          \
    ((t) ?                                   \
     (((t) > 0x35A) ?                   \
      (((t)>>2) * ((APB_CLK_FREQ>>4)/250000) + ((t)&0x3) * ((APB_CLK_FREQ>>4)/1000000))  :    \
      (((t) *(APB_CLK_FREQ>>4)) / 1000000)) :    \
     0)

#define FRC1_ENABLE_TIMER  BIT7
#define FRC1_AUTO_LOAD  BIT6

//TIMER PREDIVED MODE
typedef enum {
    DIVDED_BY_1 = 0,		//timer clock
    DIVDED_BY_16 = 4,	//divided by 16
    DIVDED_BY_256 = 8,	//divided by 256
} TIMER_PREDIVED_MODE;

typedef enum {			//timer interrupt mode
    TM_LEVEL_INT = 1,	// level interrupt
    TM_EDGE_INT   = 0,	//edge interrupt
} TIMER_INT_MODE;

/******************************************************************************
* FunctionName : hw_timer_arm
* Description  : set a trigger timer delay for this timer.
* Parameters   : uint32 val :
in autoload mode
                        50 ~ 0x7fffff;  for FRC1 source.
                        100 ~ 0x7fffff;  for NMI source.
in non autoload mode:
                        10 ~ 0x7fffff;
* Returns      : NONE
*******************************************************************************/
static __attribute__((always_inline)) inline void hw_timer_arm(u32 val)
{
    RTC_REG_WRITE(FRC1_LOAD_ADDRESS, US_TO_RTC_TIMER_TICKS(val));
}

/******************************************************************************
* FunctionName : hw_timer_init
* Description  : initilize the hardware isr timer
* Parameters   :
u8 req:
                        0,  not autoload,
                        1,  autoload mode,
* Returns      : NONE
*******************************************************************************/
static void hw_timer_init(u8 req)
{
    if (req == 1) {
        RTC_REG_WRITE(FRC1_CTRL_ADDRESS,
                      FRC1_AUTO_LOAD | DIVDED_BY_16 | FRC1_ENABLE_TIMER | TM_EDGE_INT);
    } else {
        RTC_REG_WRITE(FRC1_CTRL_ADDRESS,
                      DIVDED_BY_16 | FRC1_ENABLE_TIMER | TM_EDGE_INT);
    }

    ETS_FRC_TIMER1_INTR_ATTACH(timer_interrupt, NULL);

    TM1_EDGE_INT_ENABLE();
    ETS_FRC1_INTR_ENABLE();
}


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

  // See if there are any other channels active
  for (channel = 0; channel < sizeof(data)/sizeof(data[0]); channel++) {
    if (data[channel]) {
      break;
    }
  }

  // If not, then disable the interrupt
  if (channel >= sizeof(data) / sizeof(data[0])) {
    ETS_FRC1_INTR_DISABLE();
  }

  return 0;
}

static __attribute__((always_inline)) inline void writeIO(DATA *d) 
{
  uint32_t pinState = d->pinstate[d->currentState];

  gpio_output_set(pinState, d->mask & ~pinState, 0, 0);
}

static __attribute__((always_inline)) inline  void stepUp(DATA *d) 
{
  d->currentStep++;
  d->currentState = (d->currentState + 1) % N_STATES;
  writeIO(d);
}

static __attribute__((always_inline)) inline  void stepDown(DATA *d) 
{
  d->currentStep--;
  d->currentState = (d->currentState + N_STATES - 1) % N_STATES;
  writeIO(d);
}

static void ICACHE_RAM_ATTR timer_interrupt(void *p) 
{
  // This function really is running at interrupt level with everything
  // else masked off. It should take as little time as necessary.
  //
  (void) p;

  int i;
  uint32_t delay = 0xffffffff;

  // Loop over the channels to figure out which one needs action
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

    // This channel is past it's action time. Need to process it

    // Are we done yet?
    if (d->currentStep == d->targetStep && d->vel == 0) {
      d->stopped = 1;
      d->dir = 0;
      // TODO: We need to post a message to say that the motion is complete
      // system_os_post(LUA_TASK, , i);
      continue;
    }

    // if stopped, determine direction
    if (d->vel == 0) {
      d->dir = d->currentStep < d->targetStep ? 1 : -1;
      // do not set to 0 or it could go negative in case 2 below
      d->vel = 1; 
    }
    
    // Move the pointer by one step in the correct direction
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
    while (switec_accel_table[row][0] < d->vel) {
      row++;
    }

    uint32_t microDelay = switec_accel_table[row][1] << 4;
    if (microDelay < d->minDelay) {
      microDelay = d->minDelay;
    }

    // Figure out when we next need to take action
    d->nextTime = d->nextTime + microDelay;
    if (d->nextTime < now) {
      d->nextTime = now + microDelay;
    }

    // Figure out how long to wait
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
int switec_setup(uint32_t channel, int *pin, int maxDegPerSec )
{
  if (channel >= sizeof(data) / sizeof(data[0])) {
    return -1;
  }

  if (data[channel]) {
    if (switec_close(channel)) {
      return -1;
    }
  }

  DATA *d = (DATA *) c_zalloc(sizeof(DATA));
  if (!d) {
    return -1;
  }

  if (!data[0] && !data[1] && !data[2]) {
    // We need to stup the timer as no channel was active before
    // no autoreload
    hw_timer_init(0);
  }

  data[channel] = d;
  int i;

  for (i = 0; i < 4; i++) {
    // Build the mask for the pins to be output pins
    d->mask |= 1 << pin[i];

    int j;
    // Now build the hi states for the pins according to the 6 phases above
    for (j = 0; j < N_STATES; j++) {
      if (stateMap[j] & (1 << (3 - i))) {
        d->pinstate[j] |= 1 << pin[i];
      }
    }
  }

  d->maxVel = MAXVEL;
  if (maxDegPerSec == 0) {
    maxDegPerSec = 400;
  }
  d->minDelay = 1000000 / (3 * maxDegPerSec);

#ifdef SWITEC_DEBUG
  for (i = 0; i < 4; i++) {
    c_printf("pin[%d]=%d\n", i, pin[i]);
  }

  c_printf("Mask=0x%x\n", d->mask);
  for (i = 0; i < N_STATES; i++) {
    c_printf("pinstate[%d]=0x%x\n", i, d->pinstate[i]);
  }
#endif

  // Set all pins as outputs
  gpio_output_set(0, 0, d->mask, 0);

  ETS_FRC1_INTR_ENABLE();

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

  // If the pointer is not moving, setup so that we start it
  if (d->stopped) {
    // reset the timer to avoid possible time overflow giving spurious deltas
    d->nextTime = system_get_time() + 1000;
    d->stopped = false;

    if (!timerActive) {
      timer_interrupt(0);
    }
  }

  return 0;  
}

// Get the current position, direction and target position
int switec_getpos(uint32_t channel, int32_t *pos, int32_t *dir, int32_t *target) 
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
  *target = d->targetStep;

  return 0;
}
