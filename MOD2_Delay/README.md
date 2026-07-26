# Delay

A delay line with feedback, and the one thing a delay in a rack really wants: it can lock
to a clock.

| Control | |
|---|---|
| POT1 | Free running, delay time from 20ms to 1.3s. Clocked, the ratio against the incoming clock |
| POT2 | Feedback, reaching just past unity |
| POT3 | Input DC bias. Leave centered, it is not a parameter |
| IN1 | Clock, when clock mode is on |
| IN2 | Freeze. Feedback to unity with the input cut |
| Button | Tap tempo. Hold past 1.2s to switch between free and clocked |
| LED | Output level |

Holding the button hands IN1 over to a clock signal and back; the LED blinks once for free
and twice for clocked. In clocked mode the time knob stops setting milliseconds and starts
picking a ratio: `1/4, 1/3, 1/2, 2/3, 3/4, 1, 3/2, 2`. Triplets and dotted values are in
that list because a delay that can only do straight divisions is half a delay.

In free mode a tap overrides the knob, and moving the knob takes control back.

The clock is timed inside the audio interrupt rather than in `loop()`. `loop()` runs every
2ms, and a delay locked to a clock measured that coarsely drifts audibly against
everything else in the rack.

## Why it bends

Turning the time knob slews the read head rather than jumping it, so the pitch bends on
the way, like a tape or bucket brigade delay. A delay line whose length changes instantly
clicks instead, so something has to happen either way; this is the version that sounds
like an instrument.

Repeats also lose a little top end on each pass round the loop. Neither that nor the slew
is strictly necessary, and both are why it sounds like a delay rather than a buffer.

Feedback reaches just past unity, so it can be made to build rather than decay. `SLEW`,
`DAMP`, `DRY` and `WET` at the top of the sketch set the character.

---

See the [top-level README](../README.md) for hardware notes and build instructions.
