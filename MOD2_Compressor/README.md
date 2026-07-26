# Compressor

> Not yet tested on hardware. Compiles and runs, but nobody has listened to it.

A feed-forward compressor with a gate sidechain.

| Control | |
|---|---|
| POT1 | Threshold, -40dB to 0 |
| POT2 | Ratio, 1:1 through to limiting |
| POT3 | Input DC bias. Leave centered, it is not a parameter |
| IN1 | Sidechain trigger. Ducks on the gate |
| IN2 | Bypass while held, for comparing |
| Button | Cycles the timing, fast / medium / slow |
| LED | Gain reduction, brighter as it works harder |

## The sidechain

There is only one audio input on this module, so a conventional sidechain is off the table.
A gate sidechain is not: send the same trigger that fires your kick into IN1 and the
compressor ducks on it, which is the thing people actually reach for a sidechain to do. The
constraint picks the useful half.

## Notes

Makeup gain is applied automatically from the threshold and ratio, so moving the threshold
changes how it sounds rather than how loud it is, and the bypass on IN2 stays an honest
comparison.

The gain law lives in decibels, and `logf` and `expf` cannot be called from the interrupt.
The sketch reads the exponent straight out of the float and interpolates the mantissa
linearly instead: a couple of percent off, which is a fraction of a dB and nowhere near
enough to hear on a gain curve.

Attack is applied to the gain rather than to the detector, so a transient is measured
properly before anything is done about it.

---

See the [top-level README](../README.md) for hardware notes and build instructions.
