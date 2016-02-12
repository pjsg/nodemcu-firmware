/*
 * Driver for interfacing to cheap rotary switches that
 * have a quadrature output with an optional press button
 *
 * This sets up the relevant gpio as interrupt and then keeps track of
 * the position of the switch in software. Changes are enqueued to task
 * level and a task message posted when required. If the queue fills up
 * then moves are ignored, but the last press/release will be included.
 *
 * Philip Gladstone, N1DQ
 */

#include "platform.h"
#include "c_types.h"
#include "../libc/c_stdlib.h"
#include "../libc/c_stdio.h"
#include "driver/rotary.h"
#include "gpio_intr.h"
#include "user_interface.h"
#include "task/task.h"
#include "ets_sys.h"

//
//  Queue is empty if read == write. 
//  However, we always want to keep the previous value
//  so writing is only allowed if write - read < QUEUE_SIZE - 1

#define QUEUE_SIZE 	8

#define GET_LAST_STATUS(d)	(d->queue[(d->writeOffset-1) & (QUEUE_SIZE - 1)])
#define GET_PREV_STATUS(d)	(d->queue[(d->writeOffset-2) & (QUEUE_SIZE - 1)])
#define HAS_QUEUED_DATA(d)	(d->readOffset < d->writeOffset)
#define REPLACE_STATUS(d, x)    (d->queue[(d->writeOffset-1) & (QUEUE_SIZE - 1)] = (x))
#define HAS_QUEUE_SPACE(d)	(d->readOffset + QUEUE_SIZE - 1 > d->writeOffset)
#define QUEUE_STATUS(d, x)      (d->queue[(d->writeOffset++) & (QUEUE_SIZE - 1)] = (x))
#define GET_READ_STATUS(d)	(d->queue[d->readOffset & (QUEUE_SIZE - 1)])
#define ADVANCE_IF_POSSIBLE(d)  if (d->readOffset < d->writeOffset) { d->readOffset++; }

#define STATUS_IS_PRESSED(x)	((x & 0x80000000) != 0)

#ifdef ROTARY_DEBUG
uint32_t rotary_interrupt_count;
#endif

typedef struct {
  int8_t   phaseA_pin;
  int8_t   phaseB_pin;
  int8_t   press_pin;
  uint32_t readOffset;  // Accessed by task
  uint32_t writeOffset;	// Accessed by ISR
  uint32_t pinMask;
  uint32_t phaseA;
  uint32_t phaseB;
  uint32_t press;
  uint32_t last_press_change_time;
  int	   tasknumber;
  uint32_t queue[QUEUE_SIZE];
} DATA;

static DATA *data[ROTARY_CHANNEL_COUNT];

static uint8_t taskQueued;

static void setGpioBits(void);

static void rotary_clear_pin(int pin) 
{
  if (pin >= 0) {
    gpio_pin_intr_state_set(GPIO_ID_PIN(pin_num[pin]), GPIO_PIN_INTR_DISABLE);
    platform_gpio_mode(pin, PLATFORM_GPIO_INPUT, PLATFORM_GPIO_PULLUP);
  }
}

// Just takes the channel number. Cleans up the resources used.
int rotary_close(uint32_t channel) 
{
  if (channel >= sizeof(data) / sizeof(data[0])) {
    return -1;
  }

  DATA *d = data[channel];

  if (!d) {
    return 0;
  }

  data[channel] = NULL;

  rotary_clear_pin(d->phaseA_pin);
  rotary_clear_pin(d->phaseB_pin);
  rotary_clear_pin(d->press_pin);

  c_free(d);

  setGpioBits();

  return 0;
}

static void ICACHE_RAM_ATTR rotary_interrupt(uint32_t bits) 
{
  // This function really is running at interrupt level with everything
  // else masked off. It should take as little time as necessary.
  //
  //
  (void) bits;

#ifdef ROTARY_DEBUG
  rotary_interrupt_count++;
#endif

  // This gets the set of pins which have changed status
  uint32 gpio_status = GPIO_REG_READ(GPIO_STATUS_ADDRESS);

  int i;
  for (i = 0; i < sizeof(data) / sizeof(data[0]); i++) {
    DATA *d = data[i];
    if (!d || (gpio_status & d->pinMask) == 0) {
      continue;
    }

    GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, gpio_status & d->pinMask);

    uint32_t bits = GPIO_REG_READ(GPIO_IN_ADDRESS);

    uint32_t lastStatus = GET_LAST_STATUS(d);

    uint32_t now = system_get_time();

    uint32_t newStatus;

    newStatus = lastStatus & 0x80000000;

    // This is the debounce logic for the press switch. We ignore changes
    // for 10ms after a change.
    if (now - d->last_press_change_time > 10 * 1000) {
      newStatus = (bits & d->press) ? 0 : 0x80000000;
      if (gpio_status & d->press) {
        d->last_press_change_time = now;
      }
    }

    //  A   B
    //  1   1   => 0
    //  1   0   => 1
    //  0   0   => 2
    //  0   1   => 3

    int micropos = 2;
    if (bits & d->phaseB) {
      micropos = 3;
    }
    if (bits & d->phaseA) {
      micropos ^= 3;
    }

    int32_t rotary_pos = lastStatus;

    switch ((micropos - lastStatus) & 3) {
      case 0:
        // No change, nothing to do
	break;
      case 1:
        // Incremented by 1
	rotary_pos++;
	break;
      case 3:
        // Decremented by 1
	rotary_pos--;
	break;
      default:
        // We missed an interrupt
	// We will ignore... but mark it.
	rotary_pos += 1000000;
	break;
    }

    newStatus |= rotary_pos & 0x7fffffff;
    
    if (lastStatus != newStatus) {
      // Either we overwrite the status or we add a new one
      if (!HAS_QUEUED_DATA(d) 
	  || STATUS_IS_PRESSED(lastStatus ^ newStatus)
	  || STATUS_IS_PRESSED(lastStatus ^ GET_PREV_STATUS(d))) {
	if (HAS_QUEUE_SPACE(d)) {
	  QUEUE_STATUS(d, newStatus);
	  if (!taskQueued) {
	    if (task_post_medium(d->tasknumber, &taskQueued)) {
	      taskQueued = 1;
	    }
	  }
	} else {
	  REPLACE_STATUS(d, newStatus);
	}
      } else {
	REPLACE_STATUS(d, newStatus);
      }
    }
  }
}

// The pin numbers are actual platform GPIO numbers
int rotary_setup(uint32_t channel, int phaseA, int phaseB, int press, task_handle_t tasknumber )
{
  if (channel >= sizeof(data) / sizeof(data[0])) {
    return -1;
  }

  if (data[channel]) {
    if (rotary_close(channel)) {
      return -1;
    }
  }

  DATA *d = (DATA *) c_zalloc(sizeof(DATA));
  if (!d) {
    return -1;
  }

  data[channel] = d;
  int i;

  d->tasknumber = tasknumber;

  d->phaseA = 1 << pin_num[phaseA];
  platform_gpio_mode(phaseA, PLATFORM_GPIO_INT, PLATFORM_GPIO_PULLUP);
  gpio_pin_intr_state_set(GPIO_ID_PIN(pin_num[phaseA]), GPIO_PIN_INTR_ANYEDGE);
  d->phaseA_pin = phaseA;

  d->phaseB = 1 << pin_num[phaseB];
  platform_gpio_mode(phaseB, PLATFORM_GPIO_INT, PLATFORM_GPIO_PULLUP);
  gpio_pin_intr_state_set(GPIO_ID_PIN(pin_num[phaseB]), GPIO_PIN_INTR_ANYEDGE);
  d->phaseB_pin = phaseB;

  if (press >= 0) {
    d->press = 1 << pin_num[press];
    platform_gpio_mode(press, PLATFORM_GPIO_INT, PLATFORM_GPIO_PULLUP);
    gpio_pin_intr_state_set(GPIO_ID_PIN(pin_num[press]), GPIO_PIN_INTR_ANYEDGE);
  }
  d->press_pin = press;

  d->pinMask = d->phaseA | d->phaseB | d->press;

  setGpioBits();

  return 0;
}

static void setGpioBits()
{
  uint32_t bits = 0;
  for (int i = 0; i < ROTARY_CHANNEL_COUNT; i++) {
    DATA *d = data[i];

    if (d) {
      bits = bits | d->pinMask;
    }
  }

  platform_gpio_register_callback(bits, rotary_interrupt);
}

// Get the oldest event in the queue and remove it (if possible)
int32_t rotary_getevent(uint32_t channel) 
{
  if (channel >= sizeof(data) / sizeof(data[0])) {
    return 0;
  }

  DATA *d = data[channel];

  if (!d) {
    return 0;
  }

  int32_t result;
  
  ETS_GPIO_INTR_DISABLE();

  if (HAS_QUEUED_DATA(d)) {
    result = GET_READ_STATUS(d);
    d->readOffset++;
  } else {
    result = GET_LAST_STATUS(d);
  }

  ETS_GPIO_INTR_ENABLE();

  return result;
}

int rotary_getpos(uint32_t channel)
{
  if (channel >= sizeof(data) / sizeof(data[0])) {
    return -1;
  }

  DATA *d = data[channel];

  if (!d) {
    return -1;
  }

  return d->queue[(d->writeOffset - 1) & (QUEUE_SIZE - 1)];
}

#ifdef ROTARY_DEBUG
// Get a copy of the queue of events. Only used for debugging.
size_t rotary_getstate(uint32_t channel, int32_t *buffer, size_t maxlen) 
{
  if (channel >= sizeof(data) / sizeof(data[0])) {
    return 0;
  }

  DATA *d = data[channel];

  if (!d) {
    return 0;
  }

  size_t i;

  if (!maxlen) {
    return 0;
  }

  buffer[0] = d->queue[(d->readOffset - 1) & (QUEUE_SIZE - 1)];

  for (i = 0; i < maxlen  - 1 && ((d->readOffset + i - d->writeOffset) & (QUEUE_SIZE - 1)); i++) {
    buffer[i + 1] = d->queue[(d->readOffset + i) & (QUEUE_SIZE - 1)];
  }

  return i + 1;
}
#endif
