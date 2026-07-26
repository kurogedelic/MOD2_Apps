# Granular

A granular texture processor. The last few seconds of input are always in memory, and a
cloud of short overlapping grains is drawn out of that buffer continuously. Move the
position knob and you travel backwards through what just happened; freeze it and those few
seconds become a fixed texture to pick over.

This is not a port of Clouds. Clouds is stereo with six controls, and this module has one
input, two knobs and a pair of gates, so the shape had to be worked out from the hardware
rather than translated onto it.

| Control | |
|---|---|
| POT1 | Position, from live at one end to a few seconds back at the other |
| POT2 | Grain size, 5ms to 500ms |
| POT3 | Input DC bias. Leave centered, it is not a parameter |
| IN1 | Throw a single grain, on top of whatever the cloud is already doing |
| IN2 | Grains play backwards |
| Button | Freeze. Recording stops and the buffer becomes fixed material |
| LED | Grain activity |

Freeze is where most of the fun is. Recording stops, the buffer holds whatever was going
past, and POT1 becomes a way of exploring those few seconds rather than trailing behind
the input.

Grain size is the other one worth sweeping. Down at 5ms pitch disappears and what is left
is a grainy fog; up at 500ms the grains are long enough to reconstruct the source almost
intact.

## Density

Clouds spends a knob on density. There is no knob to spare here, so grains are scheduled
to keep a constant overlap instead: short grains arrive quickly, long ones rarely, and
four are sounding at any given moment regardless of size. The texture stays coherent while
POT2 sweeps, which is worth more than the control would have been.

## Grains

Each grain is shaped by a Hann window. A grain that starts and stops abruptly is two
clicks with some audio in between, and overlapping windows sum to something close to flat,
so the level stays put as they come and go.

Start points are scattered by up to 35% of the grain length, and each grain is detuned by
up to 1%. Neither is decoration: grains taken from exactly the same spot at exactly the
same pitch sound like a loop, and the scatter is what makes a cloud out of a repeat.

There is no dry path, because at position zero with long grains the cloud reconstructs the
input closely enough not to need one.

`NVOICE`, `OVERLAP`, `DETUNE` and `JITTER` at the top of the sketch set the character.

---

See the [top-level README](../README.md) for hardware notes and build instructions.
