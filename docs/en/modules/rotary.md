# rotary Module

This modules can read the state of cheap rotary encoder switches. These are available at
all the standard places for a dollar or two. They are five pin devices where three are used
for a gray code encoder for rotation, and two are used for the push switch. These switches
are commonly used in car audio systems. 

These switches do not have absolute positioning, but only encode the number of positions
rotated clockwise / anticlockwise. To make use of this module, connect the common pin on the quadrature
encoder to ground and the A and B phases to the nodemcu. One pin of the push switch should
also be grounded and the other pin connected to the nodemcu.

## rotary.setup()
Initialize the nodemcu to talk to a rotary encoder switch.

#### Syntax
`rotary.setup(channel, pina, pinb[, pinpress])`

#### Parameters
- `channel` The rotary module supports three switches. The channel is either 0, 1 or 2.
- `pina` This is a GPIO number (excluding 0) and connects to pin phase A on the rotary switch.
- `pinb` This is a GPIO number (excluding 0) and connects to pin phase B on the rotary switch.
- `pinpress` (optional) This is a GPIO number (excluding 0) and connects to the press switch.

#### Returns
Nothing. If the arguments are in error, or the operation cannot be completed, then an error is thrown.

#### Example
```lua
rotary.setup(0, 5,6, 7)
```
## rotary.on()
Sets a callback on specific events.

#### Syntax
`rotary.on(channel, eventtype[, callback])`

#### Parameters
- `channel` The rotary module supports three switches. The channel is either 0, 1 or 2.
- `eventtype` This defines the type of event being registered. This is one of "press", "release", "turn".
- `callback` This is a function that will be invoked when the specified event happens. 

If the callback is None or omitted, then the registration is cancelled.

The callback will be invoked with two arguments when the event happens. The first argument is the 
current position of the rotary switch, and the second is the eventtype. The position is tracked
and is represented as a signed 32-bit integer. Increasing values indicate clockwise motion.

#### Notes

Events will be delivered in order, but there may be missing "turn" events. If there is a long 
queue of events, then "press" and "release" events may also be missed.

Some switches have 4 steps per detent. THis means that, in practice, the application
should divide the position by 4 and use that to determine the number of clicks. It is
unlikely that a switch will ever reach 31 bits of rotation in either direction -- some
are rated for under 50,000 revolutions.

#### Errors
If an invalid `eventtype` is supplied, then an error will be thrown.


## rotary.close()
Releases the resources associated with the rotary switch.

#### Syntax
`rotary.close(channel)`

#### Parameters
- `channel` The rotary module supports three switches. The channel is either 0, 1 or 2.

