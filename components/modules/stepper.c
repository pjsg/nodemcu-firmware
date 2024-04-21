#define LUA_LIB

#include "driver/gpio.h"
#include "lauxlib.h"
#include "task/task.h"


#ifndef LOCAL_LUA
#include "module.h"
#include <string.h>
#include <math.h>
#include <limits.h>
#endif
#include <esp_timer.h>

#ifdef CONFIG_NODEMCU_CMODULE_STEPPER

#define MAX_PINS  8

typedef unsigned char uint8;

typedef struct {
    uint8 level[MAX_PINS];
} PHASE;

typedef enum {
  STOPPED,
  STARTING,
  RUNNING,
  STOPPING
} STATE;

typedef struct {
  bool running;
  bool closed;
  signed char direction;
  signed char pending_direction;
  uint8 pin_count;
  uint8 pins[MAX_PINS];    

  uint8 phase_count;
  PHASE *phases;

  int self_ref;
  int cb_ref;
  int position;
  int expected_end_position;
  int step;
  int end_step;
  int max_step_time;
  int min_step_time;
  int pending_end_step;

  int accel;
  int decel;

  STATE state;

  esp_timer_handle_t timer; 

  PHASE *idle_phase;
} MOTOR;

volatile bool update_in_progress = false;

static task_handle_t motor_task_id;

static void timer_interrupt(void *arg) {
  MOTOR *motor = (MOTOR *)arg;

  while (true) {
    if (motor->state == STOPPED || motor->closed) {
      task_post_high(motor_task_id, (task_param_t) motor);
      return;
    }

    PHASE *phase;
    int step = motor->step;
    int end_step = motor->end_step;
    int accel = motor->accel;
    int decel = motor->decel;
    int phase_count = motor->phase_count;
    int max_step_time = motor->max_step_time;
    int min_step_time = motor->min_step_time;
    int position = motor->position;
    uint32_t next_time = max_step_time;

    if (motor->state == STARTING) {
      phase = motor->phases + (position % phase_count);
      motor->state = RUNNING;
    } else if (motor->state == STOPPING) {
      phase = motor->idle_phase;
      motor->state = STOPPED;
    } else {
      position += motor->direction;
      motor->position = position;

      if (step < accel) {
        next_time = max_step_time - (max_step_time - min_step_time) * step / accel;
      } else if (step < end_step - decel) {
        next_time = min_step_time;
      } else {
        next_time = max_step_time - (max_step_time - min_step_time) * (end_step - step) / decel;
      }
      phase = motor->phases + (position % phase_count);
      motor->step = step + 1;

      if (step + 1 >= end_step) {
        motor->state = STOPPING;
      }
    }
  
    if (phase) {
      for (int i = 0; i < motor->pin_count; i++) {
        gpio_set_level(motor->pins[i], phase->level[i]);
      }

      esp_timer_start_once(motor->timer, next_time);
      return;
    }
  }
}

/*
    ## stepper.init()

    Specifies the GPIO pins to use and the various patterns (phases) and timing information.

    #### Syntax
    `motor = stepper.init(pins, phases, options)`

    #### Parameters
    - `pins` The list of GPIO pins connected to the motor driver.
    - `phases` The list of phases for a complete cycle. Each phase is a list of GPIO settings. The phases are run in the forwards direction for increasing step numbers.
    - `options` A table of options
      - `start` The GPIO pins are driven into this state before starting. The current speed is used.
      - `stop` The GPIO pins are driven into this state after completing the movement.
      - `min` The minimum number of microseconds per phase.
      - `max` The maximum number of microseconds per phase.
      - `accel` The number of phase steps used for acceleration from stop to required speed.
      - `decel` The number of phase steps used for deceleration down to stop.

    #### Returns
    A stepper motor object.

*/

static int stepper_init(lua_State *L) {
  // The first argument must be a table with 1 or more elements and <= MAX_PINS. These are the pin numbers.
  // The number of element is the pin_count
  // Each element must be a number
  // The second argument must be a table with 1 or more elements. Each element must be a table with pin_count elements
  // The third argument is a table of options.

  // Verify the arguments
  luaL_checktype(L, 1, LUA_TTABLE); // Verify that the first argument is a table
  luaL_checktype(L, 2, LUA_TTABLE); // Verify that the second argument is a table
  luaL_checktype(L, 3, LUA_TTABLE); // Verify that the third argument is a table

  // Get the pin_count from the first argument
  int pin_count = luaL_len(L, 1);

  // Verify that pin_count is within the valid range
  if (pin_count < 1 || pin_count > MAX_PINS) {
    return luaL_error(L, "Invalid pin count");
  }

  // Verify that each element in the first argument is a number
  for (int i = 1; i <= pin_count; i++) {
    lua_rawgeti(L, 1, i);
    if (!lua_isnumber(L, -1)) {
      return luaL_error(L, "Invalid pin number");
    }
    lua_pop(L, 1);
  }

  // Verify that each element in the second argument is a table with pin_count elements
  int phase_count = luaL_len(L, 2);
  if (phase_count < 1) {
    return luaL_error(L, "Invalid phase count");
  }
  for (int i = 1; i <= phase_count; i++) {
    lua_rawgeti(L, 2, i);
    if (!lua_istable(L, -1)) {
      return luaL_error(L, "Invalid phase");
    }
    int phase_length = luaL_len(L, -1);
    if (phase_length != pin_count) {
      return luaL_error(L, "Invalid phase length");
    }
    lua_pop(L, 1);
  }

  // Verify that the third argument is a table
  if (!lua_istable(L, 3)) {
    return luaL_error(L, "Invalid options");
  }

  // Create the motor object
  MOTOR *motor = (MOTOR *)lua_newuserdata(L, sizeof(MOTOR) + (phase_count + 1) * sizeof(PHASE));
  luaL_getmetatable(L, "motor");
  lua_setmetatable(L, -2);

  memset(motor, 0, sizeof(MOTOR) + (phase_count + 1) * sizeof(PHASE));

  // Initialize the motor object
  motor->pin_count = pin_count;
  for (int i = 0; i < pin_count; i++) {
    lua_rawgeti(L, 1, i + 1);
    motor->pins[i] = luaL_checkinteger(L, -1);
    lua_pop(L, 1);
  }

  motor->phase_count = phase_count;
  motor->phases = (PHASE *)(motor + 1);
  for (int i = 0; i < phase_count; i++) {
    lua_rawgeti(L, 2, i + 1);
    for (int j = 0; j < pin_count; j++) {
      lua_rawgeti(L, -1, j + 1);
      motor->phases[i].level[j] = luaL_checkinteger(L, -1);
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }

  motor->max_step_time = 10000;
  motor->min_step_time = 1000;
  motor->accel = 25;
  motor->decel = 25;
  motor->state = STOPPED;

  motor->cb_ref = LUA_NOREF;
  motor->self_ref = LUA_NOREF;

  // Process the options
  lua_pushnil(L);
  while (lua_next(L, 3) != 0) {
    const char *key = luaL_checkstring(L, -2);
    if (strcmp(key, "idle") == 0) {
      if (lua_istable(L, -1)) {
        motor->idle_phase = motor->phases + phase_count;
        for (int i = 0; i < pin_count; i++) {
          lua_rawgeti(L, -1, i + 1);
          motor->idle_phase->level[i] = luaL_checkinteger(L, -1);
          lua_pop(L, 1);
        }
      }
    } else if (strcmp(key, "min") == 0) {
      motor->min_step_time = luaL_checkinteger(L, -1);
    } else if (strcmp(key, "max") == 0) {
      motor->max_step_time = luaL_checkinteger(L, -1);
    } else if (strcmp(key, "accel") == 0) {
      motor->accel = luaL_checkinteger(L, -1);
    } else if (strcmp(key, "decel") == 0) {
      motor->decel = luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);
  }

  if (motor->min_step_time < 100 || motor->max_step_time < 100) {
    return luaL_error(L, "Invalid min/max values");
  }

  if (motor->accel < 0 || motor->decel < 0) {
    return luaL_error(L, "Invalid accel/decel values");
  }

  if (motor->min_step_time > motor->max_step_time) {
    return luaL_error(L, "Invalid min/max values");
  }

  gpio_config_t config;
  config.mode = GPIO_MODE_OUTPUT;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;  
  config.intr_type = GPIO_INTR_DISABLE;
  config.pin_bit_mask = 0;

  // Initialize the GPIO pins to be output
  for (int i = 0; i < motor->pin_count; i++) {
    config.pin_bit_mask |= 1ll << motor->pins[i];
  }
  if (gpio_config(&config) != ESP_OK) {
    return luaL_error(L, "Failed to configure GPIO pins");
  }

  // Create the timer
  esp_timer_create_args_t timer_args = {
    .callback = timer_interrupt,
    .arg = motor,
    .dispatch_method = ESP_TIMER_ISR,
    .name = "stepper"
  };

  esp_timer_create(&timer_args, &motor->timer);

  return 1;
}

static void set_gpio_to_input(MOTOR *motor) {
  if (motor->pin_count == 0) {
    return;
  } 
  gpio_config_t config;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;  
  config.intr_type = GPIO_INTR_DISABLE;
  config.pin_bit_mask = 0;

  for (int i = 0; i < motor->pin_count; i++) {
    config.pin_bit_mask |= 1ll << motor->pins[i];
  }
  gpio_config(&config);

  motor->pin_count = 0;
}

static void resave_cb(lua_State *L, MOTOR *motor) {
  luaL_unref2(L, LUA_REGISTRYINDEX, motor->cb_ref);
  if (lua_isfunction(L, 3)) {
    lua_pushvalue(L, 3);
    motor->cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  } else {
    motor->cb_ref = LUA_NOREF;
  }
}

static void start_motor(lua_State *L, MOTOR *motor, int steps) {
  if (motor->closed) {
    luaL_error(L, "Motor is closed");
    return;   // not strictly necessary, but makes the compiler happy
  }
  // push the motor object onto the stack
  lua_pushvalue(L, 1);
  motor->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  // save the optional cb as cb_ref
  if (lua_isfunction(L, 3)) {
    lua_pushvalue(L, 3);
    motor->cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  } else {
    motor->cb_ref = LUA_NOREF;
  }

  motor->step = 0;
  motor->end_step = abs(steps);
  motor->direction = steps < 0 ? -1 : 1;
  motor->state = STARTING;
  motor->running = true;
  timer_interrupt(motor);    // First kick to get things going
}

static void end_motor(lua_State *L, MOTOR *motor) {
  // We need to be careful as the callback can invoke another motor movement.
  int cb_ref = motor->cb_ref;
  int self_ref = motor->self_ref;

  motor->cb_ref = LUA_NOREF;
  motor->self_ref = LUA_NOREF;

  motor->running = false;
  if (cb_ref != LUA_NOREF && !motor->closed) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, cb_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, self_ref);

    // use pcall to protect against errors
    lua_pcall(L, 1, 0, 0);
  } 
  luaL_unref(L, LUA_REGISTRYINDEX, self_ref);
  luaL_unref(L, LUA_REGISTRYINDEX, cb_ref);
} 

static void adjust_pending_by(MOTOR *motor, int steps) {
  signed char new_direction = steps < 0 ? -1 : 1;
  if (motor->pending_direction == 0) {
    motor->pending_direction = new_direction;
    motor->pending_end_step = abs(steps);
  } else if (motor->pending_direction == new_direction) {
    motor->pending_end_step += abs(steps);
  } else {
    motor->pending_end_step -= abs(steps);
    if (motor->pending_end_step < 0) {
      motor->pending_direction = 2 - motor->pending_direction;
      motor->pending_end_step = -motor->pending_end_step;
    }
  }
}

/*
    ## motor:moveto()

    Moves the motor to the specified position.

    #### Syntax
    `motor:moveto(position, [cb])`

    #### Parameters
    - `position` The position to move to.
    - `cb` An optional callback function to call when the movement is complete.

    #### Returns
    Nothing.
*/
static int motor_moveto(lua_State *L) {
  MOTOR *motor = (MOTOR *)luaL_checkudata(L, 1, "motor");
  int position = luaL_checkinteger(L, 2);

  motor->expected_end_position = position;

  if (motor->running) {
    resave_cb(L, motor);
    adjust_pending_by(motor, position - motor->expected_end_position);
  } else {
    start_motor(L, motor, position - motor->position);
  }
  return 0;
}

/*
    ## motor:moveby()

    Moves the motor by the specified number of steps.

    #### Syntax
    `motor:moveby(steps)`

    #### Parameters
    - `steps` The number of steps to move by.

    #### Returns
    Nothing.
*/
static int motor_moveby(lua_State *L) {
  MOTOR *motor = (MOTOR *)luaL_checkudata(L, 1, "motor");
  int steps = luaL_checkinteger(L, 2);

  motor->expected_end_position += steps;
  if (motor->running) {
    resave_cb(L, motor);
    adjust_pending_by(motor, steps);
  } else {
    start_motor(L, motor, steps);
  }

  return 0;
}

/*
    ## motor:close()

    Closes the motor. Stops stepping and suppresses any callbacks.

    #### Syntax
    `motor:close()`

    #### Parameters
    None.

    #### Returns
    Nothing.
*/
static int motor_close(lua_State *L) {
  MOTOR *motor = (MOTOR *)luaL_checkudata(L, 1, "motor");

  motor->closed = true;

  set_gpio_to_input(motor);
  return 0;
}

static int motor_destructor(lua_State *L) {
  MOTOR *motor = (MOTOR *)luaL_checkudata(L, 1, "motor");

  // Make sure that we have nothing going on
  // I think that we know that motor->running is false as that is the
  // only time that we don't have a self_ref (or a cb_ref)

  set_gpio_to_input(motor);
  if (motor->timer) {
    esp_timer_delete(motor->timer);
  } 
  return 0;
}

static int motor_isrunning(lua_State *L) {
  MOTOR *motor = (MOTOR *)luaL_checkudata(L, 1, "motor");

  lua_pushboolean(L, motor->running);
  lua_pushinteger(L, motor->position);

  return 2;
}

static int motor_stop(lua_State *L) {
  MOTOR *motor = (MOTOR *)luaL_checkudata(L, 1, "motor");

  if (motor->running) {
    motor->expected_end_position -= motor->pending_direction * motor->pending_end_step;
    // check if first argument is true
    if (lua_isboolean(L, 2) && lua_toboolean(L, 2)) {
      motor->state = STOPPING;
      motor->end_step = motor->step;
      motor->expected_end_position = motor->position;
    } else {
      int new_end_step = motor->step + motor->decel;
      if (new_end_step < motor->end_step) {
        motor->expected_end_position -= motor->direction * (motor->end_step - new_end_step);
        motor->end_step = new_end_step;
      }
    }

    motor->pending_direction = 0;
    motor->pending_end_step = 0;
  }

  return 0;
}

static int motor_tostring(lua_State *L) {
  MOTOR *motor = (MOTOR *)luaL_checkudata(L, 1, "motor");

  lua_pushfstring(L, "Motor: %p, position %d -> %d, running %d, state %d, step %d of %d. Pending steps %d", 
    motor, motor->position, motor->expected_end_position, motor->running, 
    motor->state, motor->step, motor->end_step, motor->pending_end_step * motor->pending_direction);

  return 1;
}

static void motor_task(task_param_t param, task_prio_t prio) {
  MOTOR *motor = (MOTOR *)param;

  if (motor->running || motor->closed) {
    if (motor->pending_direction != 0 && !motor->closed) {
      motor->direction = motor->pending_direction;
      motor->step = 0;
      motor->end_step = motor->pending_end_step;
      motor->pending_direction = 0;
      motor->state = STARTING;
      timer_interrupt(motor);
    } else {  
      end_motor(lua_getstate(), motor);
    }
  }
}

LROT_BEGIN(motor_map, NULL, LROT_MASK_GC_INDEX)
  LROT_FUNCENTRY( __gc, motor_destructor )
  LROT_TABENTRY(  __index, motor_map )
  LROT_FUNCENTRY(__tostring, motor_tostring)
  LROT_FUNCENTRY(moveto, motor_moveto)
  LROT_FUNCENTRY( moveby, motor_moveby )
  LROT_FUNCENTRY( close, motor_close )
  LROT_FUNCENTRY( stop, motor_stop )
  LROT_FUNCENTRY(isrunning, motor_isrunning)
LROT_END(motor_map, NULL, LROT_MASK_GC_INDEX)


LROT_BEGIN(stepper, NULL, 0)
  LROT_FUNCENTRY( init, stepper_init )
  LROT_INTENTRY( MAX_PINS, MAX_PINS )
LROT_END(stepper, NULL, 0)

LUALIB_API int luaopen_stepper (lua_State *L) {
  luaL_rometatable(L, "motor", LROT_TABLEREF(motor_map));
  motor_task_id = task_get_id(motor_task);
  return 0;
}

NODEMCU_MODULE(STEPPER, "stepper", stepper, luaopen_stepper);

#endif
