/*
 * Driver for interfacing to cheap rotary switches that
 * have a quadrature output with an optional press button
 *
 * NodeMcu integration by Philip Gladstone, N1DQ
 */

#include "platform.h"
#include "c_types.h"
#include "../libc/c_stdlib.h"
#include "../libc/c_stdio.h"
#include "driver/rotary.h"
#include "gpio_intr.h"
#include "user_interface.h"

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

  return 0;
}

static void ICACHE_RAM_ATTR rotary_interrupt(void *p) 
{
  // This function really is running at interrupt level with everything
  // else masked off. It should take as little time as necessary.
  //
  //
  (void) p;

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

    int micropos = 0;
    if (bits & d->phaseB) {
      micropos = 1;
    }
    if (bits & d->phaseA) {
      micropos ^= 3;
    }

    // This is so when both inputs are high, there is a zero output
    micropos -= 2;

    int32_t rotary_pos = lastStatus;

    switch ((micropos - (lastStatus & 3)) & 3) {
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
	// We will ignore...
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
	  // post task message to d->tasknumber
	} else {
	  REPLACE_STATUS(d, newStatus);
	}
      } else {
	REPLACE_STATUS(d, newStatus);
      }
    }
  }

#ifdef GPIO_INTERRUPT_ENABLE
  platform_gpio_intr_dispatcher(gpio_intr_callback);
#endif
}

// The pin numbers are actual platform GPIO numbers
int rotary_setup(uint32_t channel, int phaseA, int phaseB, int press, int tasknumber )
{
  if (channel >= sizeof(data) / sizeof(data[0])) {
    return -1;
  }

  if (data[channel]) {
    if (rotary_close(channel)) {
      return -1;
    }
  }

  if (!data[0] && !data[1] && !data[2]) {
    ETS_GPIO_INTR_ATTACH(rotary_interrupt, 0);
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

  c_printf("Mask=0x%x\n", d->pinMask);

  for (i = GPIO_OUT_ADDRESS; i < GPIO_OUT_ADDRESS + 0x60; i += 16) {
    c_printf("0x%02x:  %08x %08x %08x %08x\n", i, GPIO_REG_READ(i), GPIO_REG_READ(i + 4), GPIO_REG_READ(i + 8), GPIO_REG_READ(i + 12));
  }

  return 0;
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

  int32_t result = GET_READ_STATUS(d);

  ADVANCE_IF_POSSIBLE(d);

  return result;
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
