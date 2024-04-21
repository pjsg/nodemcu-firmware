# Stepper Module
| Since  | Origin / Contributor  | Maintainer  | Source  |
| :----- | :-------------------- | :---------- | :------ |
| 2024-04-16 | [Philip Gladstone](https://github.com/pjsg) | [Philip Gladstone](https://github.com/pjsg) | [stepper.c](../../components/modules/stepper.c)|

This module can drive stepper motors in  very flexible way. This includes using simple drivers 
such as the DRV8833 and the more complex ones which have a direction input.

The approach is that the pins are specified to the module `init` method along with the pin patterns and acceleration table and it returns a stepper object.
The stepper object can be used to invoke rotations at specific speeds.

This is implemented using timers and interrupts to toggle the GPIO pins. The RMT peripheral has a limited number of outputs and
the synchronizer is not available on all ESP32 variants. This does mean that timing will not be especially accurate.

## stepper.init()

Specifies the GPIO pins to use and the various patterns (phases) and timing information.

#### Syntax
`motor = stepper.init(pins, phases, options)`

#### Parameters
- `pins` The list of GPIO pins connected to the motor driver. There is a compile time limit on the number of pins (`stepper.MAX_PINS` -- currently set to 8)
- `phases` The list of phases for a complete cycle. Each phase is a list of GPIO settings. The phases are run in the forwards direction for increasing step numbers.
- `options` A table of options
  - `idle` The GPIO pins are driven into this state after completing the movement.
  - `min` The minimum number of microseconds per phase. This is top speed
  - `max` The maximum number of microseconds per phase.
  - `accel` The number of phase steps used for acceleration from stop to required speed.
  - `decel` The number of phase steps used for deceleration down to stop.

#### Returns
A stepper motor object.

#### Example

`motor = stepper.init({9, 10, 11, 12}, {{1, 0, 0, 0}, {0, 0, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}}, {max=100000, min=10000, accel=10, decel=10, idle={0,0,0,0}})`

## motor:moveby()

This drives the motor the given number of steps, using top speed if possible. The callback is invoked when the motor has stopped. 
Note that if `moveby` is called while the motor is already moving, then the current callback (if any) is removed and replaced with the new
callback (if any). The distance is added.

#### Syntax
`motor:moveby(steps, [cb])`

#### Parameters
- `steps` The number of steps to advance where each step corresponds to a phase.
- `cb`  An optional callback invoked when the motor has stopped moving.

#### Returns
`nil`


## motor:moveto()

This drives the motor a number of steps such that it ends on the given step, using top speed if possible. After initialization, the motor is considered to be at step 0. The callback is invoked when the motor has stopped. 
Note that if `moveto` is called while the motor is already moving, then the current callback (if any) is removed and replaced with the new
callback (if any). The target position is modified to the new value. This means that a rapid sequence (assuming starting at 0) of `moveto(1000)` then `moveto(0)` will end up at zero and will not necessarily visit offset 1000.

#### Syntax
`motor:moveto(step, [cb])`

#### Parameters
- `step` The absolute step position to move to.
- `cb`  An optional callback invoked when the motor has stopped moving.

#### Returns
`nil`

## motor:stop()

This tries to stop the motor as quickly as possible.

#### Syntax
`motor:stop([abort])`

#### Parameters

- `abort` This is a boolean (default false). If True, then the deceleration is not performed and the pulse sequence is just stopped immediately.

#### Returns
`nil`

## motor:isrunning()

This returns a boolean which indicates if the motor is running and a number which is the current position.

#### Syntax
`motor:isrunning()`

#### Returns
`boolean`   True if the motor is running
`position`  The current position. 

## motor.close()

This releases all resources associated with the motor. It also reverts the GPIO pins to be inputs.

#### Syntax
`motor.close()`

#### Returns
`nil`
